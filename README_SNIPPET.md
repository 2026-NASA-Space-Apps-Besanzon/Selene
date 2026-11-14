## Selene : lancer l'application
Double-cliquer sur `selene.exe` (à la racine du projet). Glisser = tourner, molette = zoom, clic = choisir un site.

## Importer de vraies données (LOLA + JPL Horizons)
1. Installer Python 3 puis : `pip install rasterio numpy`
2. Télécharger un DEM LOLA du pôle Sud (https://pgda.gsfc.nasa.gov/products/90), par exemple `LDEM_80S_80MPP_ADJ.TIF`
   (ne pas commiter ce fichier, il est dans le .gitignore)
3. Lancer, depuis la racine du projet :

       python tools/prepare_data.py --dem LDEM_80S_80MPP_ADJ.TIF --lat -89.9 --lon 0 --start 2026-12-01

   Options : `--cell 100` (zone plus petite et plus fine), `--no-ephem` (DEM seul).
4. Relancer `selene.exe` : le message rouge "Données FICTIVES" disparaît quand tout est réel.
   Les fichiers `data/input/dem.f32`, `dem.txt` et `ephem.csv` sont petits : commitez-les pour partager la zone avec le binôme.

## Recompiler
Installer raylib pour Windows (https://www.raylib.com/), puis double-cliquer sur `build.bat`.
