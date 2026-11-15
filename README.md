# Séléné

- **Challenge:** Lunar Landing Site Navigator [link](https://www.spaceappschallenge.org/2026/challenges/lunar-landing-site-navigator/)
- **Space Apps team page / Page de l'équipe :** [link](https://www.spaceappschallenge.org/2026/find-a-team/)
- **Local event:** NASA Space Apps Challenge 2026 – Besançon (14–15 Nov 2026)

## Team members / Membres

| Name / Nom | GitHub     |
| ---------- | ---------- |
| Tom Nivet  | @Tom_Nivet |
| Lucas Otto | @ottolucas |

## The problem / Le problème

Choosing where to land near the lunar south pole is hard: the Sun skims the horizon, so a few meters of terrain decide whether a lander gets sunlight for power and a direct radio link to Earth. The challenge asks for a tool that compares candidate landing sites and dates by showing where the Sun and Earth sit relative to the local horizon.

## Our solution / Notre solution

Selene is an offline Windows desktop application (C + raylib). From NASA LOLA elevation data, it takes a 128 × 128 cell zone (about 25 km) around any chosen point. For every cell it computes the terrain horizon, including lunar curvature, and compares it with the positions and apparent disks of the Sun and Earth over 30 days and over a full year. Users explore a 3D globe, a site view, a "real view" with cast shadows, a 360° ground view and an analysis tab. The analysis tab ranks candidate sites by a weighted score: energy, communication, light/shadow cycling, terrain and science (distance to permanently shadowed ice). It also finds the best start date, tests how stable the ranking is, and exports the results to CSV. It is built for mission planners and for students and the general public: six languages, metric or imperial units and an adjustable interface scale. Results are early-screening estimates (200 m resolution), not final mission design.

## NASA data & resources used / Données NASA utilisées

- LRO LOLA (Lunar Orbiter Laser Altimeter) digital elevation models: LDEM 80S 80 m polar model, LDEM 128 pixels/degree global model, and the global LDEM from the CGI Moon Kit – https://lola.gsfc.nasa.gov/ and https://pds-geosciences.wustl.edu/missions/lro/lola.htm
- LROC (Lunar Reconnaissance Orbiter Camera) color mosaic used for the globe texture – https://www.lroc.asu.edu/
- NASA Scientific Visualization Studio, CGI Moon Kit – https://svs.gsfc.nasa.gov/4720/
- JPL Horizons, used as a reference to check our offline ephemeris (agreement within about 0.02°) – https://ssd.jpl.nasa.gov/horizons/

## Use of AI tools (required) / Utilisation d'outils d'IA (obligatoire)

We used **Claude (Anthropic)** as a coding and writing assistant throughout the project, for:

- writing and debugging the C application and the Python data-preparation tool;
- writing test scripts, for example to check data reading on synthetic elevation files;
- translating the interface into five languages (English, Spanish, Italian, Russian, Japanese) in addition to French, and drafting documentation and this README.

The team defined the concept and requirements, tested the software and made the design decisions. AI translations have not been reviewed by native speakers. No image generators were used.

## Deliverables / Livrables

- [Video prensentation Selene](https://mega.nz/file/BsQyRKTD#AGUAdAAAABhSIJWqJIWNEwAAAAAAAAAAUqSVmySFjRM)


## How to run it / Comment le lancer

**Windows 10/11 (64-bit):** download the installer `Setup-Selene.exe`, or unzip the portable version, and run `selene.exe`.

**Build from source:** install MSYS2 (UCRT64) with gcc and raylib, then run `build.bat`. It compiles `src/main.c` and `src/winproc.c` and produces `selene.exe`.

## Sources & credits

- raylib (zlib license) – https://www.raylib.com/
- sdefl / sinfl compression (public domain) – https://github.com/vurtun/lib
- Jean Meeus, _Astronomical Algorithms_ – basis of our analytical Sun, Moon and libration model
- Python libraries: rasterio and numpy – https://rasterio.readthedocs.io/ and https://numpy.org/
- Noto Sans CJK JP font (SIL Open Font License 1.1), subset – https://github.com/notofonts/noto-cjk
- MSYS2 / MinGW-w64, PyInstaller and NSIS for building and packaging

---

### Before the deadline (Sun 15 Nov, 23:59) / Avant la fin

- [ ] Every section above is filled in (in English)
- [ ] Demo video/slides link works for someone who is not logged in
- [ ] AI-use section filled in
- [ ] Project submitted on your Space Apps team page → **Project** tab, with the link to THIS repository
