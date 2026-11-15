#!/usr/bin/env python3
"""
Selene - préparation des données réelles (LOLA + JPL Horizons)

Produit, dans data/input/ :
  dem.f32    altitudes float32 (128 x 128, mètres, "haut de l'image" = haut de la carte LOLA)
  dem.txt    taille d'une cellule en mètres (première ligne), puis infos
  ephem.csv  t_h,sun_az,sun_el,earth_az,earth_el  (240 lignes, pas de 3 h)
             azimuts exprimés dans le repère de l'IMAGE (rotation déjà appliquée)
  frame.f32       repère local de chaque cellule (le moteur en déduit Soleil/Terre cellule par cellule)
  ephem_vec.csv   directions Soleil/Terre sur 30 jours (pas de 3 h), repère lunaire
  ephem_year.csv  idem sur une année entière (pas de 6 h) : utilisé par l'onglet Classer

Usage :
  pip install rasterio numpy
  python tools/prepare_data.py --lat -89.9 --lon 0         (cherche tout seul LDEM_80S_* et/ou LDEM128_*)
  python tools/prepare_data.py --dem LDEM_80S_80MPP_ADJ.TIF --lat -89.9 --lon 0
  python tools/prepare_data.py --dem LDEM128_PA_pixel_202405.tif --lat -60 --lon 30
     (MNT mondial reconnu quel que soit son format : degrés ou mètres, longitudes -180..180 ou 0..360, entiers LOLA x 0,5 m ou flottants)
  python tools/prepare_data.py --lat -85 --lon 10 --source global     (forcer le MNT mondial)
  python tools/prepare_data.py --lat -89.9 --lon 0 --start 2026-12-01
Les deux types de fichiers sont reconnus automatiquement (polaire stéréographique ou mondial cylindrique).
  python tools/prepare_data.py --dem ... --no-ephem        (DEM seul)
  python tools/prepare_data.py --ephem-only --lat -89.9 --lon 0 --theta 0   (éphémérides seules)

Géométrie : le moteur C utilise frame.f32 + ephem_vec.csv pour calculer le Soleil et la Terre cellule par cellule
(courbure de la Lune comprise). ephem.csv (valeurs au centre) ne sert plus qu'en secours pour d'anciens sites.
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


# ------------------------------------------------------------------ repère local de chaque cellule
def _vec(lat_deg, lon_deg):
    la, lo = np.radians(lat_deg), np.radians(lon_deg)
    return np.stack([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)], axis=-1)


def _tangent(p0, p1):
    d = p1 - p0
    d = d - np.sum(d * p0, axis=-1, keepdims=True) * p0
    return d / np.maximum(np.linalg.norm(d, axis=-1, keepdims=True), 1e-12)


def write_frame(n, ex, ey):
    """frame.f32 : pour chaque cellule (ligne par ligne), 9 float32 = verticale locale, 'droite de l'image', 'haut de l'image',
    exprimées dans le repère lunaire. Le moteur C en déduit la hauteur et la direction du Soleil et de la Terre cellule par cellule."""
    os.makedirs(OUT, exist_ok=True)
    np.concatenate([n, ex, ey], axis=2).astype("<f4").tofile(os.path.join(OUT, "frame.f32"))


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
        # repère local de chaque cellule (centres des cellules de sortie)
        cc = c0 + (np.arange(GRID) + 0.5) * wpx / GRID
        rr = r0 + (np.arange(GRID) + 0.5) * wpx / GRID
        CC, RR = np.meshgrid(cc, rr)
        tr = ds.transform
        X = (tr.c + tr.a * CC).ravel()
        Y = (tr.f + tr.e * RR).ravel()
        dlt = 100.0                                                   # pas (m) pour les axes de l'image
        def to_vec(xs, ys):
            lons, lats = transform(crs, ll, xs.tolist(), ys.tolist())
            return _vec(np.array(lats), np.array(lons)).reshape(GRID, GRID, 3)
        n0 = to_vec(X, Y)
        ex = _tangent(n0, to_vec(X + dlt, Y))                          # vers la droite de l'image (+x projeté)
        ey = _tangent(n0, to_vec(X, Y + dlt))                          # vers le haut de l'image (+y projeté)
    write_frame(n0, ex, ey)

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


def _aeqd_inverse(lat0, lon0, X, Y):
    """Grille plane (m, x = est, y = nord au centre) -> vecteurs unitaires, lat, lon (deg).
    Projection azimutale équidistante centrée en (lat0, lon0) : exacte partout, y compris aux pôles."""
    la, lo = math.radians(lat0), math.radians(lon0)
    c = np.array([math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la)])
    e = np.array([-math.sin(lo), math.cos(lo), 0.0])
    n = np.array([-math.sin(la) * math.cos(lo), -math.sin(la) * math.sin(lo), math.cos(la)])
    d = np.hypot(X, Y)
    s = d / MOON_R
    dd = np.maximum(d, 1e-9)
    dirv = (X / dd)[..., None] * e + (Y / dd)[..., None] * n
    p = np.cos(s)[..., None] * c + np.sin(s)[..., None] * dirv
    lat = np.degrees(np.arcsin(np.clip(p[..., 2], -1.0, 1.0)))
    lon = np.degrees(np.arctan2(p[..., 1], p[..., 0]))
    return p, lat, lon


def sample_values(ds, nx=24, ny=12, win=64):
    """Valeurs prises dans une grille de petites fenêtres réparties sur toute l'image (rapide, même pour un fichier de plusieurs Go)."""
    from rasterio.windows import Window
    w, h = min(win, ds.width), min(win, ds.height)
    vals = []
    for j in range(ny):
        for i in range(nx):
            c = min(max(int((i + 0.5) * ds.width / nx - w / 2), 0), ds.width - w)
            r = min(max(int((j + 0.5) * ds.height / ny - h / 2), 0), ds.height - h)
            vals.append(ds.read(1, window=Window(c, r, w, h)).astype("float64").ravel())
    return np.concatenate(vals)


def vertical_scale(ds, vscale=None, voffset=None, name=""):
    """Renvoie (échelle, décalage, description) tels que mètres = valeur * échelle + décalage."""
    if vscale is not None:
        return vscale, voffset or 0.0, "imposé (--vscale)"
    sc = ds.scales[0] if ds.scales else None
    of = ds.offsets[0] if ds.offsets else None
    if (sc not in (None, 1.0)) or (of not in (None, 0.0)):
        return (sc if sc is not None else 1.0), (of or 0.0), "métadonnées du fichier"
    dtype = ds.dtypes[0]
    sm = sample_values(ds)
    if ds.nodata is not None:
        sm = sm[sm != ds.nodata]
    sm = sm[np.isfinite(sm)]
    amax = float(np.abs(sm).max()) if sm.size else 0.0
    if dtype == "uint16":
        return 0.5, -10000.0, "uint16 décalé : (v - 20000) x 0,5"
    if dtype.startswith("float"):
        return (1000.0, 0.0, "kilomètres") if amax < 30 else (1.0, 0.0, "mètres")
    # entiers signés : les LDEM de la mission LOLA sont en demi-mètres (DN x 0,5 = altitude par rapport à 1737,4 km)
    if name.lower().startswith("ldem") or amax > 12500:
        return 0.5, 0.0, "demi-mètres (convention LOLA : valeur x 0,5)"
    return 1.0, 0.0, "mètres"


def geo_origin(ds):
    """(longitude du bord gauche, latitude du bord haut) de l'image, en degrés, quel que soit le système de coordonnées :
    géographique -180..180 ou 0..360, ou projection cylindrique en mètres (équirectangulaire). Les LDEM sont souvent en 0..360."""
    left, top = ds.bounds.left, ds.bounds.top
    lon_w, lat_n = -180.0, 90.0
    try:
        if ds.crs is not None and ds.crs.is_geographic:
            lon_w, lat_n = left, top
        elif ds.crs is not None:
            from rasterio.crs import CRS
            from rasterio.warp import transform
            (lo,), (la,) = transform(ds.crs, CRS.from_string(LL_PROJ), [left], [top])
            lon_w, lat_n = lo, la
    except Exception as ex:
        print(f"[!] Géoréférencement illisible ({ex}) : longitudes -180..180 et latitude 90 supposées.")
    if not (-360.0 <= lon_w <= 360.0 and 80.0 <= lat_n <= 90.5):
        print(f"[!] Géoréférencement inattendu (bord gauche {lon_w:.2f}, bord haut {lat_n:.2f}) : -180..180 et 90 supposés.")
        lon_w, lat_n = -180.0, 90.0
    return lon_w, min(lat_n, 90.0)


def prepare_dem_global(path, lat, lon, cell_m, zoom=1.0, vscale=None, voffset=None):
    """Zone locale prise dans un DEM mondial (projection cylindrique : LDEM128_PA_pixel_*, ldem_64...).
    La zone est rééchantillonnée dans une grille plane centrée sur le site (nord en haut au centre, theta = 0).
    Fonctionne à toute latitude, pôles compris."""
    import rasterio
    from rasterio.windows import Window

    with rasterio.open(path) as ds:
        W, H = ds.width, ds.height
        deg = 360.0 / W                                       # degrés par pixel
        native = math.radians(deg) * MOON_R                   # m / pixel à l'équateur
        if cell_m is None:
            cell_m = round(native * zoom)
        scale, offset, how = vertical_scale(ds, vscale, voffset, os.path.basename(path))
        lon_w, lat_n = geo_origin(ds)
        if "_pa" in os.path.basename(path).lower():
            print("[i] Fichier en repère PA (axes principaux) : ses coordonnées sont utilisées telles quelles. L'écart avec le repère\n"
                  "    « Terre moyenne » des éphémérides est inférieur à 0,03 deg (moins d'1 km) : négligeable pour une zone de 30 km.")
        nodata = ds.nodata
        print(f"MNT mondial {os.path.basename(path)} : {W}x{H}, type {ds.dtypes[0]}, altitudes en {how}, longitudes de {round(lon_w, 3) + 0.0:.1f} à {lon_w + 360.0:.1f}, pas {deg * 60:.2f} arcmin")

        # grille locale : x vers l'est, y vers le nord, centrée sur le site
        idx = np.arange(GRID)
        xs = (idx - (GRID - 1) / 2.0) * cell_m
        ys = ((GRID - 1) / 2.0 - idx) * cell_m
        X, Y = np.meshgrid(xs, ys)
        n0, lat_g, lon_g = _aeqd_inverse(lat, lon, X, Y)
        lonu = lon + ((lon_g - lon + 180.0) % 360.0 - 180.0)  # longitude continue autour du site

        r0 = max(int(math.floor((lat_n - lat_g.max()) / deg)) - 2, 0)
        r1 = min(int(math.ceil((lat_n - lat_g.min()) / deg)) + 2, H)
        c0 = int(math.floor((lonu.min() - lon_w) / deg)) - 2
        c1 = int(math.ceil((lonu.max() - lon_w) / deg)) + 2
        cols = np.arange(c0, c1) % W                          # la longitude boucle à 180 deg
        if (r1 - r0) * len(cols) > 120_000_000:
            sys.exit("[!] Zone trop grande pour ce site : choisissez une zone plus petite (--zoom).")
        block = np.empty((r1 - r0, len(cols)), dtype="float64")
        i = 0
        while i < len(cols):                                  # lecture par morceaux contigus
            j = i
            while j + 1 < len(cols) and cols[j + 1] == cols[j] + 1:
                j += 1
            block[:, i:j + 1] = ds.read(1, window=Window(int(cols[i]), r0, j - i + 1, r1 - r0)).astype("float64")
            i = j + 1

    bad = ~np.isfinite(block)
    if nodata is not None:
        bad |= (block == nodata)
    block = block * scale + offset
    if bad.any():
        block[bad] = float(block[~bad].mean()) if (~bad).any() else 0.0

    fr = np.clip((lat_n - lat_g) / deg - 0.5 - r0, 0, block.shape[0] - 1.001)
    fc = np.clip((lonu - lon_w) / deg - 0.5 - c0, 0, block.shape[1] - 1.001)
    ri, ci = fr.astype(int), fc.astype(int)
    wr, wc = fr - ri, fc - ci
    arr = ((1 - wr) * (1 - wc) * block[ri, ci] + (1 - wr) * wc * block[ri, ci + 1]
           + wr * (1 - wc) * block[ri + 1, ci] + wr * wc * block[ri + 1, ci + 1]).astype("<f4")
    if arr.min() < -20000 or arr.max() > 20000:
        print("[!] Altitudes hors norme (attendu environ -9000 à +11000 m) : corrigez avec --vscale (ex. 0.5, 1, 1000).")

    # axes de l'image dans le repère lunaire (même méthode que la version polaire)
    dlt = 100.0
    px, _, _ = _aeqd_inverse(lat, lon, X + dlt, Y)
    py, _, _ = _aeqd_inverse(lat, lon, X, Y + dlt)
    write_frame(n0, _tangent(n0, px), _tangent(n0, py))

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
    vec_path = os.path.join(OUT, "ephem_vec.csv")
    if online:
        if os.path.exists(vec_path):
            os.remove(vec_path)                      # incohérent avec Horizons : le moteur utilisera ephem.csv
    else:
        with open(vec_path, "w", encoding="utf-8") as f:
            f.write("# t_h, direction du Soleil (unitaire), vecteur Terre (km), repere lunaire (x lon0, y est, z nord)\n")
            for i, (sv, ev) in enumerate(ephem_offline.vectors(start, N_STEPS, STEP_MIN)):
                f.write(f"{i * STEP_MIN / 60:.1f},{sv[0]:.7f},{sv[1]:.7f},{sv[2]:.7f},{ev[0]:.2f},{ev[1]:.2f},{ev[2]:.2f}\n")
    if not online:                                   # année complète à pas de 6 h, pour l'onglet Classer
        with open(os.path.join(OUT, "ephem_year.csv"), "w", encoding="utf-8") as f:
            f.write("# annee complete, pas de 6 h : t_h, direction du Soleil (unitaire), vecteur Terre (km), repere lunaire\n")
            for i, (sv, ev) in enumerate(ephem_offline.vectors(start, 1461, 360)):
                f.write(f"{i * 6.0:.1f},{sv[0]:.7f},{sv[1]:.7f},{sv[2]:.7f},{ev[0]:.1f},{ev[1]:.1f},{ev[2]:.1f}\n")
    print(f"ephem.csv écrit ({N_STEPS} lignes). Soleil : élévation {min(r[1] for r in sun):.2f} à {max(r[1] for r in sun):.2f} deg ; "
          f"Terre : {min(r[1] for r in earth):.2f} à {max(r[1] for r in earth):.2f} deg")


# ------------------------------------------------------------------ main
SEARCHED = []
SKIP = ("_ERR", "_EFFRES", "_HILL", "LDSM", "LDEC", "_RMSD", "_ROUGH")


def find_dems(explicit):
    """Fichiers MNT candidats : ceux donnés en option, puis ceux trouvés dans ., data/source, data."""
    found = [p for p in explicit if p]
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))      # racine du projet (parent de tools/)
    dirs = []
    for base in (".", root):
        for sub in ("", os.path.join("data", "source"), "data"):
            d = os.path.abspath(os.path.join(base, sub))
            if d not in dirs:
                dirs.append(d)
    for base in (".", root):                                            # tous les sous-dossiers de data/ (source, sources, ...)
        top = os.path.join(base, "data")
        if os.path.isdir(top):
            for cur, subs, _ in os.walk(top):
                subs[:] = [x for x in subs if x not in ("input", "sites")]
                d = os.path.abspath(cur)
                if d not in dirs:
                    dirs.append(d)
    SEARCHED[:] = dirs
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            low = name.lower()
            if (low.startswith("ldem") and low.endswith((".tif", ".tiff"))
                    and not any(k.lower() in low for k in SKIP)):
                p = os.path.join(d, name)
                if p not in found:
                    found.append(p)
    return found


def dem_width(path):
    """Largeur en pixels : sert à choisir le MNT mondial le plus détaillé."""
    import rasterio
    with rasterio.open(path) as ds:
        return ds.width


def dem_kind(path):
    """'global' (cylindrique, longitude/latitude) ou 'polar' (projeté, stéréographique)."""
    import rasterio
    with rasterio.open(path) as ds:
        if abs(ds.width / ds.height - 2.0) < 0.05:           # carte mondiale 2:1 (en degrés OU en mètres, ex. LDEM128_PA_pixel_*)
            return "global"
        if ds.crs is not None and ds.crs.is_geographic:
            return "global"
        return "polar"


def main():
    ap = argparse.ArgumentParser(description="Prépare les données réelles pour Selene")
    ap.add_argument("--dem", help="GeoTIFF LOLA (polaire 80S ou mondial : le type est détecté ; cherché automatiquement si absent)")
    ap.add_argument("--global-dem", help="second MNT (optionnel) ; le type est détecté aussi")
    ap.add_argument("--source", choices=["auto", "polar", "global"], default="auto", help="forcer le type de MNT utilisé")
    ap.add_argument("--vscale", type=float, default=None, help="MNT mondial : mètres = valeur x vscale + voffset (si les altitudes sortent fausses)")
    ap.add_argument("--voffset", type=float, default=None, help="voir --vscale")
    ap.add_argument("--out", default=os.path.join("data", "input"), help="dossier de sortie (un dossier par site)")
    ap.add_argument("--lat", type=float, default=-89.9, help="latitude du centre de la zone (deg, négatif au sud)")
    ap.add_argument("--lon", type=float, default=0.0, help="longitude Est du centre (deg)")
    ap.add_argument("--zoom", type=float, default=1.0, help="multiplicateur de la taille de zone (2 = côté doublé)")
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
        cands = find_dems([a.dem, a.global_dem])
        kinds = {}
        for p in cands:
            try:
                kinds[p] = dem_kind(p)
            except Exception as ex:
                print(f"[!] {p} illisible par rasterio : {ex}")
        print("MNT trouvés :", ", ".join(f"{os.path.basename(p)} ({kinds.get(p, 'illisible')})" for p in cands) or "aucun")
        if not cands:
            print("Dossiers examinés :", "; ".join(SEARCHED))
        polar = next((p for p in cands if kinds.get(p) == "polar"), None)
        globs = [p for p in cands if kinds.get(p) == "global"]
        glob_dem = max(globs, key=dem_width) if globs else None            # le plus fin : ldem_64 plutôt que ldem_16
        cell = (a.cell or 200.0) * a.zoom
        half_deg = math.degrees(GRID * cell / 2.0 / MOON_R) * 1.5          # demi-fenêtre, diagonale comprise
        polar_ok = polar is not None and a.lat + half_deg <= -80.0         # le MNT polaire s'arrête à 80 deg S
        if a.source == "polar":
            use = "polar"
        elif a.source == "global":
            use = "global"
        else:
            use = "polar" if polar_ok else "global"
        if use == "polar":
            if not polar:
                sys.exit("[!] Aucun MNT polaire (LDEM_80S_*.tif) trouvé : placez-le dans data/source/.")
            theta = prepare_dem(polar, a.lat, a.lon, cell)
        else:
            if not glob_dem:
                sys.exit("[!] Aucun MNT mondial (LDEM128_*.tif) trouvé dans data/source/ pour ce site"
                         + (" (hors de la couverture du MNT polaire)." if polar else "."))
            if a.source == "auto" and polar:
                print("[i] Site hors de la couverture du MNT polaire : utilisation du MNT mondial.")
            if a.source == "auto" and not polar and a.lat <= -80.0:
                print("[i] Pas de MNT polaire : le MNT mondial est moins précis près du pôle.")
            theta = prepare_dem_global(glob_dem, a.lat, a.lon, a.cell * a.zoom if a.cell else None, a.zoom,
                                       a.vscale, a.voffset)
    elif theta is None:
        ap.error("--ephem-only demande --theta (voir dem.txt)")
    if not a.no_ephem:
        prepare_ephem(a.lat, a.lon, start, theta, online=a.horizons)
    print("Terminé. Relancez selene.exe.")


if __name__ == "__main__":
    main()