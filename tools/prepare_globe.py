#!/usr/bin/env python3
"""
Selene - prépare le globe lunaire (relief LOLA + couleurs LRO) pour selene.exe

Produit dans data/globe/ :
  dem_globe.f32     altitudes en mètres, grille 129 x 257 (lat 90 -> -90, lon -180 -> 180)
  moon_color.png    carte de couleurs 8 bits

Usage (depuis la racine du projet) :
  pip install rasterio numpy pillow
  python tools/prepare_globe.py
      -> cherche tout seul le MNT mondial le plus fin (ex. LDEM128_PA_pixel_*.tif) et la carte de couleurs (lroc_color_*.tif)
         dans data/, data/source, data/globe...
  python tools/prepare_globe.py --dem data/source/LDEM128_PA_pixel_202405.tif --color data/globe/lroc_color_16bit_srgb_4k.tif

Le MNT est lu bande par bande (un fichier de plusieurs Go ne tient pas en mémoire) et moyenné par case de 1,4 deg.
Tous les formats sont reconnus (degrés ou mètres, longitudes -180..180 ou 0..360, entiers LOLA x 0,5 m ou flottants).
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prepare_data as pdm                         # même détection de format que la préparation des sites

GLON, GLAT = 256, 128          # doit correspondre à GLON/GLAT dans src/main.c
OUT = os.path.join("data", "globe")


def find_color(explicit):
    """Carte de couleurs : option --color, sinon un .tif dont le nom contient « lroc » ou « color » (dans data/ ou à la racine)."""
    if explicit:
        return explicit
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dirs = list(pdm.SEARCHED) + [os.path.join(root, "data", "globe"), root]
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            low = name.lower()
            if low.endswith((".tif", ".tiff")) and ("lroc" in low or "color" in low) and "ldem" not in low:
                return os.path.join(d, name)
    return None


def build_globe_dem(path):
    """Altitudes (m) sur la grille 129 x 257 des sommets de la sphère, depuis un MNT mondial quelconque."""
    import rasterio
    from rasterio.enums import Resampling
    from rasterio.windows import Window

    with rasterio.open(path) as ds:
        W, H = ds.width, ds.height
        scale, offset, how = pdm.vertical_scale(ds, None, None, os.path.basename(path))
        lon_w, lat_n = pdm.geo_origin(ds)
        print(f"MNT : {os.path.basename(path)}, {W}x{H}, type {ds.dtypes[0]}, altitudes en {how}, longitudes de {round(lon_w, 3) + 0.0:.1f} à {round(lon_w, 3) + 360.0:.1f}")
        if abs(lat_n - 90.0) > 0.5:
            print(f"[!] Le bord haut de l'image est à {lat_n:.1f} deg (90 attendu) : le MNT ne couvre pas toute la Lune ?")
        cells = np.zeros((GLAT, GLON))
        bad = np.zeros((GLAT, GLON), dtype=bool)
        print("Lecture du MNT par bandes (un gros fichier demande une à deux minutes) ...")
        for r in range(GLAT):
            r0, r1 = int(round(r * H / GLAT)), int(round((r + 1) * H / GLAT))
            band = ds.read(1, window=Window(0, r0, W, max(r1 - r0, 1)), out_shape=(1, GLON),
                           resampling=Resampling.average, masked=True)
            row = np.ma.filled(band.astype("float64"), np.nan)[0]
            cells[r] = row
            bad[r] = ~np.isfinite(row)
            if r % 16 == 15:
                print(f"  {100 * (r + 1) // GLAT} %")
    mean = float(np.nanmean(cells)) if (~bad).any() else 0.0
    cells[bad] = mean
    cells = cells * scale + offset
    shift = int(round((lon_w + 180.0) / (360.0 / GLON)))     # la case 0 de la sortie est à -180 deg
    cells = np.roll(cells, shift, axis=1)

    # cases (centres décalés d'une demi-case) -> sommets de la sphère : moyenne des 4 cases voisines, pôles = moyenne de la rangée
    cw = np.concatenate([cells[:, -1:], cells, cells[:, :1]], axis=1)          # la longitude boucle
    v = np.empty((GLAT + 1, GLON + 1))
    left, right = cw[:, 0:GLON + 1], cw[:, 1:GLON + 2]                          # cases à gauche et à droite de chaque sommet
    v[1:GLAT, :] = 0.25 * (left[0:GLAT - 1] + right[0:GLAT - 1] + left[1:GLAT] + right[1:GLAT])
    v[0, :] = cells[0].mean()
    v[GLAT, :] = cells[-1].mean()
    v[:, -1] = v[:, 0]                                                          # raccord de la couture à 180 deg
    return v


def main():
    ap = argparse.ArgumentParser(description="Prépare le globe de Selene (relief + couleurs)")
    ap.add_argument("--dem", help="MNT mondial (ex. LDEM128_PA_pixel_202405.tif) ; cherché automatiquement si absent")
    ap.add_argument("--color", help="carte de couleurs (ex. lroc_color_16bit_srgb_4k.tif) ; cherchée automatiquement si absente")
    a = ap.parse_args()

    import rasterio
    from PIL import Image

    cands = pdm.find_dems([a.dem])
    globs = []
    for p in cands:
        try:
            if pdm.dem_kind(p) == "global":
                globs.append(p)
        except Exception as ex:
            print(f"[!] {p} illisible : {ex}")
    if not globs:
        sys.exit("[!] Aucun MNT mondial trouvé : placez LDEM128_*.tif (ou ldem_64.tif) dans data/source/ ou indiquez --dem.")
    dem_path = max(globs, key=pdm.dem_width)                                    # le plus fin
    color_path = find_color(a.color)
    if not color_path:
        sys.exit("[!] Carte de couleurs introuvable : placez lroc_color_16bit_srgb_4k.tif dans data/globe/ ou indiquez --color.")

    os.makedirs(OUT, exist_ok=True)
    h = build_globe_dem(dem_path)
    print(f"Relief : {h.min():.0f} à {h.max():.0f} m (attendu environ -9000 à +11000 m ; les extrêmes sont lissés par la moyenne)")
    if h.min() < -20000 or h.max() > 20000:
        print("[!] Valeurs hors norme : vérifiez que c'est bien le fichier d'altitude.")
    h.astype("<f4").tofile(os.path.join(OUT, "dem_globe.f32"))

    with rasterio.open(color_path) as ds:
        img = ds.read(list(range(1, min(ds.count, 3) + 1)))
        if img.shape[0] == 1:
            img = np.repeat(img, 3, axis=0)
    if img.dtype == np.uint16:
        img = (img / 257.0).round().astype(np.uint8)
    img = np.transpose(img.astype(np.uint8), (1, 2, 0))
    Image.fromarray(img).save(os.path.join(OUT, "moon_color.png"))
    print(f"Couleurs : {os.path.basename(color_path)}, {img.shape[1]}x{img.shape[0]} px")
    print("Terminé : data/globe/dem_globe.f32 et data/globe/moon_color.png. Relancez selene.exe.")


if __name__ == "__main__":
    main()