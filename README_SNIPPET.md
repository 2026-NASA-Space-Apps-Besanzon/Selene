## Selene : lancer l'application
Double-cliquer sur `selene.exe` (à la racine du projet). Aucune installation, aucun navigateur.

Commandes : glisser = tourner, molette = zoom, clic = choisir un site.

## Recompiler
Installer raylib pour Windows (https://www.raylib.com/), puis double-cliquer sur `build.bat`.

## Données réelles
Déposer dans `data/input/` :
- `dem.f32` : altitudes en float32 (128x128, mètres, nord en haut)
- `ephem.csv` : `t_h,sun_az,sun_el,earth_az,earth_el` (240 lignes, pas de 3 h)
Sans ces fichiers, l'application utilise des données FICTIVES (signalé en rouge dans le panneau).
