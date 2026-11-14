#!/usr/bin/env python3
"""
Selene - prépare le globe lunaire (relief LOLA + couleurs LRO) pour selene.exe

Produit dans data/globe/ :
  dem_globe.f32     altitudes en mètres, grille 129 x 257 (lat 90 -> -90, lon -180 -> 180)
  moon_color.png    carte de couleurs 8 bits

Usage :
  pip install rasterio numpy pillow
  python tools/prepare_globe.py --dem ldem_16.tif --color lroc_color_16bit_srgb_4k.tif
"""
import argparse
import os

import numpy as np

GLON, GLAT = 256, 128          # doit correspondre à GLON/GLAT dans src/main.c
OUT = os.path.join("data", "globe")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dem", required=True, help="altitude CGI Moon Kit (ex. ldem_16.tif)")
    ap.add_argument("--color", required=True, help="couleurs (ex. lroc_color_16bit_srgb_4k.tif)")
    a = ap.parse_args()

    import rasterio
    from rasterio.enums import Resampling
    from PIL import Image

    os.makedirs(OUT, exist_ok=True)

    with rasterio.open(a.dem) as ds:
        raw = ds.read(1, out_shape=(GLAT + 1, GLON + 1), resampling=Resampling.average).astype("float64")
        is_float = ds.dtypes[0].startswith("float")
    h = raw * 1000.0 if is_float else (raw - 20000.0) * 0.5    # km -> m  ou  demi-mètres décalés -> m
    h[:, -1] = h[:, 0]                                          # raccord de la couture à 180 deg
    print(f"Relief : {h.min():.0f} à {h.max():.0f} m (attendu environ -9000 à +11000 m)")
    if h.min() < -20000 or h.max() > 20000:
        print("[!] Valeurs hors norme : vérifiez que c'est bien le fichier d'altitude.")
    h.astype("<f4").tofile(os.path.join(OUT, "dem_globe.f32"))

    with rasterio.open(a.color) as ds:
        img = ds.read(list(range(1, min(ds.count, 3) + 1)))
        if img.shape[0] == 1:
            img = np.repeat(img, 3, axis=0)
    if img.dtype == np.uint16:
        img = (img / 257.0).round().astype(np.uint8)
    img = np.transpose(img.astype(np.uint8), (1, 2, 0))
    Image.fromarray(img).save(os.path.join(OUT, "moon_color.png"))
    print(f"Couleurs : {img.shape[1]}x{img.shape[0]} px")
    print("Terminé. Relancez selene.exe.")


if __name__ == "__main__":
    main()
