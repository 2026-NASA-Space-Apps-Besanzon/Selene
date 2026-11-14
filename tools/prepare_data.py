#!/usr/bin/env python3
"""
Selene - préparation des données réelles (LOLA + JPL Horizons)

Produit, dans data/input/ :
  dem.f32    altitudes float32 (128 x 128, mètres, "haut de l'image" = haut de la carte LOLA)
  dem.txt    taille d'une cellule en mètres (première ligne), puis infos
  ephem.csv  t_h,sun_az,sun_el,earth_az,earth_el  (240 lignes, pas de 3 h)
             azimuts exprimés dans le repère de l'IMAGE (rotation déjà appliquée)

Usage :
  pip install rasterio numpy
  python tools/prepare_data.py --dem LDEM_80S_80MPP_ADJ.TIF --lat -89.9 --lon 0
  python tools/prepare_data.py --dem LDEM_80S_80MPP_ADJ.TIF --lat -89.9 --lon 0 --start 2026-12-01
  python tools/prepare_data.py --dem ... --no-ephem        (DEM seul)
  python tools/prepare_data.py --ephem-only --lat -89.9 --lon 0 --theta 0   (éphémérides seules)

Limite assumée : une seule éphéméride pour toute la zone (calculée au centre).
Sur 25 km, l'erreur d'élévation du Soleil peut atteindre ~0,2 deg aux bords : à citer dans le pitch.
"""
import argparse
import datetime as dt
import math
import os
import sys
import urllib.parse
import urllib.request

import numpy as np

GRID = 128                 # doit correspondre à GW/GH dans src/main.c
N_STEPS = 240              # idem
STEP_MIN = 180             # 3 h
MOON_R = 1737400.0         # rayon de référence LOLA (m)
OUT = os.path.join("data", "input")
LL_PROJ = f"+proj=longlat +R={MOON_R} +no_defs"
HORIZONS = "https://ssd.jpl.nasa.gov/api/horizons.api"


# ------------------------------------------------------------------ DEM
def prepare_dem(path, lat, lon, cell_m):
    import rasterio
    from rasterio.crs import CRS
    from rasterio.enums import Resampling
    from rasterio.warp import transform
    from rasterio.windows import Window

    with rasterio.open(path) as ds:
        crs = ds.crs or CRS.from_string(f"+proj=stere +lat_0=-90 +lat_ts=-90 +lon_0=0 +R={MOON_R} +no_defs")
        if ds.crs is None:
            print("[!] Pas de CRS dans le fichier : projection stéréographique polaire sud supposée.")
        if ds.transform.e > 0:
            sys.exit("[!] Image orientée sud-en-haut : non géré dans cette version.")
        res = ds.res[0]                                   # m / pixel
        ll = CRS.from_string(LL_PROJ)
        (x,), (y,) = transform(ll, crs, [lon], [lat])
        (xn,), (yn,) = transform(ll, crs, [lon], [lat + 0.01])   # un pas vers le NORD local
        theta = math.degrees(math.atan2(xn - x, yn - y)) % 360.0  # direction du nord local, depuis le haut de l'image
        row, col = ds.index(x, y)
        wpx = int(round(GRID * cell_m / res))
        r0, c0 = row - wpx // 2, col - wpx // 2
        if r0 < 0 or c0 < 0 or r0 + wpx > ds.height or c0 + wpx > ds.width:
            sys.exit("[!] La zone demandée sort de l'image : choisissez un site plus central ou une zone plus petite.")
        arr = ds.read(1, window=Window(c0, r0, wpx, wpx), out_shape=(GRID, GRID),
                      resampling=Resampling.average, masked=True)
        nodata = int(arr.mask.sum()) if np.ma.is_masked(arr) else 0
        fill = float(arr.mean()) if nodata < arr.size else 0.0
        arr = np.ma.filled(arr.astype("float64"), fill).astype("<f4")
        real_cell = wpx * res / GRID

    os.makedirs(OUT, exist_ok=True)
    arr.tofile(os.path.join(OUT, "dem.f32"))
    with open(os.path.join(OUT, "dem.txt"), "w", encoding="utf-8") as f:
        f.write(f"{real_cell:.3f}\n")
        f.write(f"# source={os.path.basename(path)} centre lat={lat} lon={lon} fenetre={GRID * real_cell / 1000:.1f} km\n")
        f.write(f"# rotation nord local / haut image = {theta:.2f} deg\n")
    print(f"DEM : {GRID}x{GRID}, {real_cell:.1f} m/cellule, fenêtre {GRID * real_cell / 1000:.1f} km, "
          f"altitudes {arr.min():.0f} à {arr.max():.0f} m ({nodata} pixels sans donnée comblés)")
    print(f"Rotation nord local -> haut d'image : {theta:.2f} deg")
    return theta


def prepare_dem_global(path, lat, lon, cell_m):
    """Zone locale prise dans un DEM mondial (projection cylindrique, ex. ldem_64 du CGI Moon Kit).
    La grille est orientée nord en haut : aucune rotation des azimuts (theta = 0)."""
    import rasterio
    from rasterio.windows import Window

    with rasterio.open(path) as ds:
        W, H = ds.width, ds.height
        deg = 360.0 / W                                       # degrés par pixel
        native = math.radians(deg) * MOON_R                   # m / pixel à l'équateur
        if cell_m is None:
            cell_m = round(native)
        is_float = ds.dtypes[0].startswith("float")
        half = GRID * cell_m / 2.0
        dlat = math.degrees(half / MOON_R)
        dlon = math.degrees(half / (MOON_R * max(math.cos(math.radians(lat)), 0.05)))
        r0 = max(int(math.floor((90 - (lat + dlat)) / deg)) - 2, 0)
        r1 = min(int(math.ceil((90 - (lat - dlat)) / deg)) + 2, H)
        c0 = int(math.floor((lon - dlon + 180) / deg)) - 2
        c1 = int(math.ceil((lon + dlon + 180) / deg)) + 2
        cols = np.arange(c0, c1) % W                          # la longitude boucle à 180 deg
        block = np.empty((r1 - r0, len(cols)), dtype="float64")
        i = 0
        while i < len(cols):                                  # lecture par morceaux contigus
            j = i
            while j + 1 < len(cols) and cols[j + 1] == cols[j] + 1:
                j += 1
            block[:, i:j + 1] = ds.read(1, window=Window(int(cols[i]), r0, j - i + 1, r1 - r0)).astype("float64")
            i = j + 1
    block = block * 1000.0 if is_float else (block - 20000.0) * 0.5   # -> mètres

    # grille locale : x vers l'est, y vers le nord, centrée sur le site
    idx = np.arange(GRID)
    xs = (idx - (GRID - 1) / 2.0) * cell_m
    ys = ((GRID - 1) / 2.0 - idx) * cell_m
    X, Y = np.meshgrid(xs, ys)
    lat_g = np.clip(lat + np.degrees(Y / MOON_R), -90.0, 90.0)
    lon_g = lon + np.degrees(X / (MOON_R * np.maximum(np.cos(np.radians(lat_g)), 0.05)))
    fr = np.clip((90.0 - lat_g) / deg - 0.5 - r0, 0, block.shape[0] - 1.001)
    fc = np.clip((lon_g + 180.0) / deg - 0.5 - c0, 0, block.shape[1] - 1.001)
    ri, ci = fr.astype(int), fc.astype(int)
    wr, wc = fr - ri, fc - ci
    arr = ((1 - wr) * (1 - wc) * block[ri, ci] + (1 - wr) * wc * block[ri, ci + 1]
           + wr * (1 - wc) * block[ri + 1, ci] + wr * wc * block[ri + 1, ci + 1]).astype("<f4")

    os.makedirs(OUT, exist_ok=True)
    arr.tofile(os.path.join(OUT, "dem.f32"))
    with open(os.path.join(OUT, "dem.txt"), "w", encoding="utf-8") as f:
        f.write(f"{cell_m:.3f}\n")
        f.write(f"# source={os.path.basename(path)} (mondial) centre lat={lat} lon={lon} fenetre={GRID * cell_m / 1000:.1f} km\n")
        f.write("# rotation nord local / haut image = 0.00 deg\n")
    print(f"DEM mondial : {GRID}x{GRID}, {cell_m:.0f} m/cellule (résolution native ~{native:.0f} m), "
          f"fenêtre {GRID * cell_m / 1000:.1f} km, altitudes {arr.min():.0f} à {arr.max():.0f} m")
    return 0.0


# ------------------------------------------------------------------ Horizons
def parse_horizons(text):
    """Renvoie [(azimut, élévation), ...] depuis une réponse Horizons (CSV, quantité 4)."""
    if "$$SOE" not in text or "$$EOE" not in text:
        raise RuntimeError("Réponse Horizons inattendue :\n" + text[:1500])
    body = text.split("$$SOE", 1)[1].split("$$EOE", 1)[0]
    rows = []
    for line in body.strip().splitlines():
        nums = []
        for fld in line.split(",")[1:]:                  # [0] = date
            try:
                nums.append(float(fld.strip()))
            except ValueError:
                pass                                      # marqueurs de présence solaire/lunaire
        if len(nums) >= 2:
            rows.append((nums[-2], nums[-1]))            # azimut, élévation
    return rows


def fetch_azel(body, lat, lon, start):
    stop = start + dt.timedelta(minutes=STEP_MIN * (N_STEPS - 1))
    params = {
        "format": "text", "COMMAND": f"'{body}'", "OBJ_DATA": "'NO'", "MAKE_EPHEM": "'YES'",
        "EPHEM_TYPE": "'OBSERVER'", "CENTER": "'coord@301'", "COORD_TYPE": "'GEODETIC'",
        "SITE_COORD": f"'{lon},{lat},0'",
        "START_TIME": f"'{start:%Y-%m-%d %H:%M}'", "STOP_TIME": f"'{stop:%Y-%m-%d %H:%M}'",
        "STEP_SIZE": f"'{STEP_MIN} m'", "QUANTITIES": "'4'", "ANG_FORMAT": "'DEG'",
        "APPARENT": "'AIRLESS'", "CSV_FORMAT": "'YES'",
    }
    url = HORIZONS + "?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": "selene-hackathon/0.1"})
    with urllib.request.urlopen(req, timeout=60) as r:
        text = r.read().decode("utf-8", "replace")
    rows = parse_horizons(text)
    if len(rows) < N_STEPS:
        raise RuntimeError(f"Horizons a renvoyé {len(rows)} lignes pour '{body}', {N_STEPS} attendues.\n" + text[:1500])
    return rows[:N_STEPS]


def prepare_ephem(lat, lon, start, theta, online=False):
    """Soleil et Terre vus depuis le site. Par défaut : modèle hors ligne (ephem_offline.py).
    Avec online=True : JPL Horizons (référence, demande internet)."""
    if online:
        print(f"Horizons : Soleil et Terre vus depuis lat={lat}, lon={lon}, début {start:%Y-%m-%d %H:%M} UTC ...")
        sun = fetch_azel("10", lat, lon, start)
        earth = fetch_azel("399", lat, lon, start)
        origin = "Horizons"
    else:
        import ephem_offline
        print(f"Modèle hors ligne : Soleil et Terre vus depuis lat={lat}, lon={lon}, début {start:%Y-%m-%d %H:%M} UTC ...")
        rows = ephem_offline.azel_series(lat, lon, start, N_STEPS, STEP_MIN)
        sun = [(r[0], r[1]) for r in rows]
        earth = [(r[2], r[3]) for r in rows]
        origin = "modele hors ligne (Meeus)"
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "ephem.csv"), "w", encoding="utf-8") as f:
        f.write(f"# {origin}, site lat={lat} lon={lon}, debut {start:%Y-%m-%d %H:%M} UTC, pas {STEP_MIN} min\n")
        f.write(f"# azimuts dans le repere image (nord local + {theta:.2f} deg)\n")
        for i in range(N_STEPS):
            sa = (sun[i][0] + theta) % 360.0
            ea = (earth[i][0] + theta) % 360.0
            f.write(f"{i * STEP_MIN / 60:.1f},{sa:.3f},{sun[i][1]:.3f},{ea:.3f},{earth[i][1]:.3f}\n")
    print(f"ephem.csv écrit ({N_STEPS} lignes). Soleil : élévation {min(r[1] for r in sun):.2f} à {max(r[1] for r in sun):.2f} deg ; "
          f"Terre : {min(r[1] for r in earth):.2f} à {max(r[1] for r in earth):.2f} deg")


# ------------------------------------------------------------------ main
def find_first(patterns):
    import glob
    for pat in patterns:
        found = sorted(glob.glob(pat))
        if found:
            return found[0]
    return None


def main():
    ap = argparse.ArgumentParser(description="Prépare les données réelles pour Selene")
    ap.add_argument("--dem", help="GeoTIFF LOLA polaire 80S (cherché automatiquement si absent)")
    ap.add_argument("--global-dem", help="DEM mondial type ldem_64 (cherché automatiquement si absent)")
    ap.add_argument("--out", default=os.path.join("data", "input"), help="dossier de sortie (un dossier par site)")
    ap.add_argument("--lat", type=float, default=-89.9, help="latitude du centre de la zone (deg, négatif au sud)")
    ap.add_argument("--lon", type=float, default=0.0, help="longitude Est du centre (deg)")
    ap.add_argument("--cell", type=float, default=None, help="taille de cellule en m (zone = 128 x cell). Défaut : 200 (polaire) ou résolution native (mondial)")
    ap.add_argument("--start", default=dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d"), help="début des éphémérides (UTC, AAAA-MM-JJ)")
    ap.add_argument("--no-ephem", action="store_true", help="ne pas produire ephem.csv")
    ap.add_argument("--horizons", action="store_true", help="utiliser JPL Horizons (internet) au lieu du modèle hors ligne")
    ap.add_argument("--ephem-only", action="store_true", help="ne produire que ephem.csv")
    ap.add_argument("--theta", type=float, default=None, help="rotation (deg) pour --ephem-only")
    a = ap.parse_args()
    global OUT
    OUT = a.out

    start = dt.datetime.strptime(a.start, "%Y-%m-%d")
    theta = a.theta
    if not a.ephem_only:
        polar = a.dem or find_first(["LDEM_80S*.tif*", "ldem_80s*.tif*"])
        glob_dem = a.global_dem or find_first(["ldem_64*.tif*", "LDEM_64*.tif*", "ldem_16*.tif*", "LDEM_16*.tif*"])
        if a.lat <= -81.0 and polar:
            theta = prepare_dem(polar, a.lat, a.lon, a.cell or 200.0)
        elif glob_dem:
            if a.lat <= -81.0:
                print("[!] DEM polaire introuvable : utilisation du DEM mondial (moins précis).")
            theta = prepare_dem_global(glob_dem, a.lat, a.lon, a.cell)
        else:
            sys.exit("[!] Aucun DEM trouvé : placez LDEM_80S_80MPP_ADJ.tiff (pôle Sud) et/ou ldem_64.tif (mondial) dans le dossier du projet.")
    elif theta is None:
        ap.error("--ephem-only demande --theta (voir dem.txt)")
    if not a.no_ephem:
        prepare_ephem(a.lat, a.lon, start, theta, online=a.horizons)
    print("Terminé. Relancez selene.exe.")


if __name__ == "__main__":
    main()