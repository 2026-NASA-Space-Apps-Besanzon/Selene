/*
 * SELENE - Planificateur de site d'atterrissage au pôle Sud lunaire (v0, application native)
 *
 * Un seul programme : calcul (C pur) + interface (raylib). Aucun serveur, aucun navigateur.
 *
 * Données réelles (optionnelles, à placer à côté de l'exécutable) :
 *   data/input/dem.f32   DEM brut float32, GW*GH valeurs (mètres), ligne par ligne, nord en haut
 *   data/input/ephem.csv t_h,sun_az,sun_el,earth_az,earth_el   (N_STEPS lignes, pas de STEP_H heures)
 * Sans ces fichiers, le programme utilise un terrain et des éphémérides FICTIFS.
 *
 * Simplifications assumées (à citer dans le pitch) :
 *   - Lune plate (courbure négligeable sur ~25 km)
 *   - sortie de la grille = ciel libre (optimiste sur les bords)
 *   - Soleil et Terre = points (pas de disque solaire)
 */
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

/* ===================== Paramètres ===================== */
#define GW 128                  /* largeur de la grille (cellules) */
#define GH 128                  /* hauteur de la grille */
static float cell_m = 200.0f;   /* taille d'une cellule (m), lue dans data/input/dem.txt si présent */
#define N_STEPS 240             /* nombre de pas de temps */
#define STEP_H 3.0f             /* durée d'un pas (h) -> 240 x 3 h = 30 jours */
#define NCELL (GW * GH)
#define LUNAR_MONTH_H (29.53 * 24.0)
#define EARTH_PERIOD_H (27.32 * 24.0)

#define PANEL_W 310
#define TL_H 210

static float dem[NCELL];
static float slope_deg[NCELL];
static uint8_t illum[(size_t)N_STEPS * NCELL];   /* 1 = Soleil visible */
static uint8_t earthv[(size_t)N_STEPS * NCELL];  /* 1 = Terre visible  */
static double sun_az[N_STEPS], sun_el[N_STEPS], earth_az[N_STEPS], earth_el[N_STEPS];
static float dem_min, dem_max;
static int real_dem = 0, real_ephem = 0;

/* ===================== Moteur : données ===================== */
static void generate_synthetic_dem(void) {
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            double cx = 0.42 * GW, cy = 0.55 * GH, R = 28.0;
            double r = hypot(x - cx, y - cy), h = 0.0;
            if (r < R) h -= 1500.0 * (1.0 - (r / R) * (r / R));                 /* cuvette */
            h += 450.0 * exp(-pow((r - R) / 4.0, 2.0));                         /* rebord  */
            double px = 0.82 * GW, py = 0.25 * GH;
            h += 950.0 * exp(-(pow(x - px, 2.0) + pow(y - py, 2.0)) / (2.0 * 9.0 * 9.0)); /* massif */
            h += 0.5 * x + 18.0 * sin(x * 0.35) * cos(y * 0.29) + 9.0 * sin(x * 0.9 + y * 0.7);
            dem[y * GW + x] = (float)h;
        }
}

static void load_cell_size(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    float v;
    if (fscanf(f, "%f", &v) == 1 && v > 1.0f) cell_m = v;
    fclose(f);
}

static int load_real_dem(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(dem, sizeof(float), NCELL, f);
    fclose(f);
    return n == NCELL;
}

static void compute_slopes(void) {
    dem_min = dem_max = dem[0];
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int xm = x > 0 ? x - 1 : x, xp = x < GW - 1 ? x + 1 : x;
            int ym = y > 0 ? y - 1 : y, yp = y < GH - 1 ? y + 1 : y;
            double dzdx = (dem[y * GW + xp] - dem[y * GW + xm]) / ((xp - xm) * cell_m);
            double dzdy = (dem[yp * GW + x] - dem[ym * GW + x]) / ((yp - ym) * cell_m);
            slope_deg[y * GW + x] = (float)(atan(hypot(dzdx, dzdy)) * 180.0 / PI);
            float h = dem[y * GW + x];
            if (h < dem_min) dem_min = h;
            if (h > dem_max) dem_max = h;
        }
}

static int load_ephem(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f) && n < N_STEPS) {
        double t, sa, se, ea, ee;
        if (line[0] == '#') continue;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf", &t, &sa, &se, &ea, &ee) == 5) {
            sun_az[n] = sa; sun_el[n] = se; earth_az[n] = ea; earth_el[n] = ee; n++;
        }
    }
    fclose(f);
    return n == N_STEPS;
}

/* FICTIF : ordres de grandeur réalistes, valeurs fausses. */
static void generate_mock_ephem(void) {
    for (int i = 0; i < N_STEPS; i++) {
        double t = i * (double)STEP_H;
        sun_az[i] = fmod(360.0 * t / LUNAR_MONTH_H, 360.0);
        sun_el[i] = 1.2 + 0.3 * sin(2.0 * PI * t / LUNAR_MONTH_H);
        earth_az[i] = 180.0 + 25.0 * sin(2.0 * PI * t / EARTH_PERIOD_H);
        earth_el[i] = 6.0 * sin(2.0 * PI * t / EARTH_PERIOD_H + 1.0);
    }
}

/* ===================== Moteur : raycasting ===================== */
/* Vrai si l'astre (azimut depuis le Nord, élévation, en degrés) est visible depuis la cellule (x0, y0). */
static uint8_t visible(int x0, int y0, double az_deg, double el_deg) {
    if (el_deg <= 0.0) return 0;
    double az = az_deg * PI / 180.0;
    double dx = sin(az), dy = -cos(az);              /* x = est, y = sud (lignes vers le bas) */
    double tan_el = tan(el_deg * PI / 180.0);
    float h0 = dem[y0 * GW + x0];
    for (int d = 1;; d++) {
        if ((dem_max - h0) <= d * cell_m * tan_el) break;
        int ix = (int)lround(x0 + dx * d), iy = (int)lround(y0 + dy * d);
        if (ix < 0 || ix >= GW || iy < 0 || iy >= GH) break;
        if (dem[iy * GW + ix] - h0 > d * cell_m * tan_el) return 0;
    }
    return 1;
}

static void compute_visibility(void) {
    for (int t = 0; t < N_STEPS; t++) {
        size_t off = (size_t)t * NCELL;
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++) {
                illum[off + y * GW + x] = visible(x, y, sun_az[t], sun_el[t]);
                earthv[off + y * GW + x] = visible(x, y, earth_az[t], earth_el[t]);
            }
    }
}

static float pct_visible(const uint8_t *arr, int k) {
    int n = 0;
    for (int t = 0; t < N_STEPS; t++) n += arr[(size_t)t * NCELL + k];
    return 100.0f * n / N_STEPS;
}
static float longest_run_days(const uint8_t *arr, int k, int val) {
    int best = 0, cur = 0;
    for (int t = 0; t < N_STEPS; t++) {
        if (arr[(size_t)t * NCELL + k] == val) { if (++cur > best) best = cur; } else cur = 0;
    }
    return best * STEP_H / 24.0f;
}

/* ===================== Interface : widgets minimalistes ===================== */
static const Color C_BG = {16, 21, 28, 255}, C_PANEL = {24, 32, 42, 255}, C_LINE = {42, 53, 68, 255};
static const Color C_TXT = {232, 236, 242, 255}, C_MUT = {142, 155, 176, 255};
static const Color C_SUN = {242, 184, 75, 255}, C_EARTH = {88, 166, 232, 255};
static const Color C_SHADE = {35, 45, 59, 255}, C_BAD = {229, 96, 77, 255}, C_OK = {95, 191, 138, 255};
static const Color C_BTN = {42, 53, 68, 255}, C_HOT = {60, 74, 94, 255};

static const void *g_active = NULL;   /* widget en cours de glissement */

static bool ui_button(Rectangle r, const char *txt, bool on) {
    bool hot = CheckCollisionPointRec(GetMousePosition(), r);
    DrawRectangleRec(r, on ? C_SUN : (hot ? C_HOT : C_BTN));
    int w = MeasureText(txt, 16);
    DrawText(txt, (int)(r.x + (r.width - w) / 2), (int)(r.y + (r.height - 16) / 2), 16, on ? (Color){26, 20, 5, 255} : C_TXT);
    return hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static bool ui_slider(float x, float y, float w, const char *label, float *v, float lo, float hi, const char *valtxt) {
    DrawText(label, (int)x, (int)y, 16, C_TXT);
    int vw = MeasureText(valtxt, 16);
    DrawText(valtxt, (int)(x + w - vw), (int)y, 16, C_MUT);
    Rectangle track = {x, y + 26, w, 8};
    Rectangle hit = {x, y + 18, w, 24};
    Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_active = v;
    bool changed = false;
    if (g_active == v) {
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            float f = Clamp((m.x - track.x) / track.width, 0.0f, 1.0f);
            float nv = lo + f * (hi - lo);
            if (nv != *v) { *v = nv; changed = true; }
        } else g_active = NULL;
    }
    float f = (*v - lo) / (hi - lo);
    DrawRectangleRec(track, C_LINE);
    DrawRectangle((int)x, (int)track.y, (int)(w * f), 8, C_SUN);
    DrawCircle((int)(x + w * f), (int)(track.y + 4), 8, C_TXT);
    return changed;
}

/* ===================== Terrain 3D ===================== */
static Mesh mesh;
static float shade_k[NCELL];

static void update_geometry(float exag) {
    for (int j = 0; j < GH; j++)
        for (int i = 0; i < GW; i++) {
            int k = j * GW + i;
            mesh.vertices[3 * k] = i - GW / 2.0f;
            mesh.vertices[3 * k + 1] = dem[k] / cell_m * exag;
            mesh.vertices[3 * k + 2] = j - GH / 2.0f;
            int im = i > 0 ? i - 1 : i, ip = i < GW - 1 ? i + 1 : i;
            int jm = j > 0 ? j - 1 : j, jp = j < GH - 1 ? j + 1 : j;
            float dhdx = (dem[j * GW + ip] - dem[j * GW + im]) / (ip - im) / cell_m * exag;
            float dhdz = (dem[jp * GW + i] - dem[jm * GW + i]) / (jp - jm) / cell_m * exag;
            Vector3 n = Vector3Normalize((Vector3){-dhdx, 1.0f, -dhdz});
            mesh.normals[3 * k] = n.x; mesh.normals[3 * k + 1] = n.y; mesh.normals[3 * k + 2] = n.z;
            Vector3 l = Vector3Normalize((Vector3){-0.4f, 0.8f, 0.45f});
            float d = Vector3DotProduct(n, l);
            shade_k[k] = 0.55f + 0.45f * (d > 0 ? d : 0);
        }
    UpdateMeshBuffer(mesh, 0, mesh.vertices, NCELL * 3 * sizeof(float), 0);
    UpdateMeshBuffer(mesh, 2, mesh.normals, NCELL * 3 * sizeof(float), 0);
}

static void recolor(int layer, int t, float smax) {
    size_t off = (size_t)t * NCELL;
    for (int k = 0; k < NCELL; k++) {
        float r, g, b;
        if (layer == 0) { if (illum[off + k]) { r = 0.95f; g = 0.72f; b = 0.29f; } else { r = 0.14f; g = 0.18f; b = 0.25f; } }
        else if (layer == 1) { if (earthv[off + k]) { r = 0.35f; g = 0.65f; b = 0.91f; } else { r = 0.14f; g = 0.18f; b = 0.25f; } }
        else if (layer == 2) {
            float v = (dem[k] - dem_min) / (dem_max - dem_min);
            r = 0.18f + 0.65f * v; g = 0.20f + 0.60f * v; b = 0.26f + 0.50f * v;
        } else {
            float v = Clamp(slope_deg[k] / 30.0f, 0.0f, 1.0f);
            r = 0.2f + 0.7f * v; g = 0.7f - 0.5f * v; b = 0.4f - 0.2f * v;
        }
        if (slope_deg[k] > smax && layer != 3) { r = r * 0.5f + 0.45f; g = g * 0.5f + 0.10f; b = b * 0.5f + 0.08f; }
        float s = shade_k[k];
        mesh.colors[4 * k] = (unsigned char)(Clamp(r * s, 0, 1) * 255);
        mesh.colors[4 * k + 1] = (unsigned char)(Clamp(g * s, 0, 1) * 255);
        mesh.colors[4 * k + 2] = (unsigned char)(Clamp(b * s, 0, 1) * 255);
        mesh.colors[4 * k + 3] = 255;
    }
    UpdateMeshBuffer(mesh, 3, mesh.colors, NCELL * 4, 0);
}

static void build_mesh(void) {
    int tris = (GW - 1) * (GH - 1) * 2;
    mesh.vertexCount = NCELL;
    mesh.triangleCount = tris;
    mesh.vertices = (float *)MemAlloc(NCELL * 3 * sizeof(float));
    mesh.normals = (float *)MemAlloc(NCELL * 3 * sizeof(float));
    mesh.texcoords = (float *)MemAlloc(NCELL * 2 * sizeof(float));
    mesh.colors = (unsigned char *)MemAlloc(NCELL * 4);
    mesh.indices = (unsigned short *)MemAlloc(tris * 3 * sizeof(unsigned short));
    int q = 0;
    for (int j = 0; j < GH - 1; j++)
        for (int i = 0; i < GW - 1; i++) {
            unsigned short a = (unsigned short)(j * GW + i), b = a + 1, c = (unsigned short)(a + GW), d = c + 1;
            mesh.indices[q++] = a; mesh.indices[q++] = c; mesh.indices[q++] = b;
            mesh.indices[q++] = b; mesh.indices[q++] = c; mesh.indices[q++] = d;
        }
}

/* ===================== Globe lunaire ===================== */
#define GLON 256                 /* doit correspondre à tools/prepare_globe.py */
#define GLAT 128
#define GVERT ((GLON + 1) * (GLAT + 1))
#define GLOBE_R 100.0f           /* rayon d'affichage de la sphère */
#define MOON_R_M 1737400.0f

static Mesh gmesh;
static float *g_h = NULL;        /* altitudes (m) */
static Vector3 *g_dir = NULL;    /* direction unitaire de chaque sommet */
static Vector3 *g_nrm = NULL;    /* normales (pour l'éclairage) */
static Texture2D g_tex;

/* latitude / longitude (deg) -> vecteur unitaire (est vers la droite, nord en haut) */
static Vector3 sph(float lat_deg, float lon_deg) {
    float la = lat_deg * DEG2RAD, lo = lon_deg * DEG2RAD;
    return (Vector3){cosf(la) * cosf(lo), sinf(la), -cosf(la) * sinf(lo)};
}

static int load_globe(void) {
    FILE *f = fopen("data/globe/dem_globe.f32", "rb");
    if (!f) return 0;
    g_h = (float *)MemAlloc(GVERT * sizeof(float));
    size_t n = fread(g_h, sizeof(float), GVERT, f);
    fclose(f);
    if (n != GVERT) return 0;
    g_tex = LoadTexture("data/globe/moon_color.png");
    if (g_tex.id == 0) return 0;
    SetTextureFilter(g_tex, TEXTURE_FILTER_BILINEAR);

    int tris = GLON * GLAT * 2;
    gmesh.vertexCount = GVERT;
    gmesh.triangleCount = tris;
    gmesh.vertices = (float *)MemAlloc(GVERT * 3 * sizeof(float));
    gmesh.texcoords = (float *)MemAlloc(GVERT * 2 * sizeof(float));
    gmesh.colors = (unsigned char *)MemAlloc(GVERT * 4);
    gmesh.indices = (unsigned short *)MemAlloc(tris * 3 * sizeof(unsigned short));
    g_dir = (Vector3 *)MemAlloc(GVERT * sizeof(Vector3));
    g_nrm = (Vector3 *)MemAlloc(GVERT * sizeof(Vector3));
    for (int j = 0; j <= GLAT; j++)
        for (int i = 0; i <= GLON; i++) {
            int k = j * (GLON + 1) + i;
            g_dir[k] = sph(90.0f - 180.0f * j / GLAT, -180.0f + 360.0f * i / GLON);
            gmesh.texcoords[2 * k] = (float)i / GLON;
            gmesh.texcoords[2 * k + 1] = (float)j / GLAT;
        }
    int q = 0;
    for (int j = 0; j < GLAT; j++)
        for (int i = 0; i < GLON; i++) {
            unsigned short a = (unsigned short)(j * (GLON + 1) + i), b = a + 1, c = (unsigned short)(a + GLON + 1), d = c + 1;
            gmesh.indices[q++] = a; gmesh.indices[q++] = c; gmesh.indices[q++] = b;
            gmesh.indices[q++] = b; gmesh.indices[q++] = c; gmesh.indices[q++] = d;
        }
    for (int k = 0; k < GVERT; k++) { gmesh.colors[4 * k + 3] = 255; }
    UploadMesh(&gmesh, true);
    return 1;
}

static Vector3 gpos(int i, int j) {
    int k = j * (GLON + 1) + i;
    return (Vector3){gmesh.vertices[3 * k], gmesh.vertices[3 * k + 1], gmesh.vertices[3 * k + 2]};
}

/* Relief exagéré : exag = 1 est le relief réel (très plat à cette échelle) */
static void globe_update_geometry(float exag) {
    for (int k = 0; k < GVERT; k++) {
        float r = GLOBE_R * (1.0f + g_h[k] * exag / MOON_R_M);
        gmesh.vertices[3 * k] = g_dir[k].x * r;
        gmesh.vertices[3 * k + 1] = g_dir[k].y * r;
        gmesh.vertices[3 * k + 2] = g_dir[k].z * r;
    }
    for (int j = 0; j <= GLAT; j++)
        for (int i = 0; i <= GLON; i++) {
            int im = i > 0 ? i - 1 : GLON - 1, ip = i < GLON ? i + 1 : 1;
            int jm = j > 0 ? j - 1 : j, jp = j < GLAT ? j + 1 : j;
            Vector3 east = Vector3Subtract(gpos(ip, j), gpos(im, j));
            Vector3 south = Vector3Subtract(gpos(i, jp), gpos(i, jm));
            Vector3 n = Vector3CrossProduct(south, east);
            int k = j * (GLON + 1) + i;
            g_nrm[k] = Vector3Length(n) > 1e-6f ? Vector3Normalize(n) : g_dir[k];
        }
    UpdateMeshBuffer(gmesh, 0, gmesh.vertices, GVERT * 3 * sizeof(float), 0);
}

/* Éclairage : lumière rasante venant de la caméra, pour faire ressortir le relief */
static void globe_shade(Vector3 light) {
    for (int k = 0; k < GVERT; k++) {
        float d = Vector3DotProduct(g_nrm[k], light);
        unsigned char s = (unsigned char)(255.0f * (0.30f + 0.70f * (d > 0 ? d : 0)));
        gmesh.colors[4 * k] = s; gmesh.colors[4 * k + 1] = s; gmesh.colors[4 * k + 2] = s;
    }
    UpdateMeshBuffer(gmesh, 3, gmesh.colors, GVERT * 4, 0);
}

/* Lit "lat=... lon=..." sur la 2e ligne de dem.txt (écrite par prepare_data.py) */
static int load_zone_center(const char *path, float *lat, float *lon) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int ok = 0;
    while (fgets(line, sizeof line, f)) {
        char *a = strstr(line, "lat="), *b = strstr(line, "lon=");
        if (a && b && sscanf(a + 4, "%f", lat) == 1 && sscanf(b + 4, "%f", lon) == 1) { ok = 1; break; }
    }
    fclose(f);
    return ok;
}

/* ===================== Programme principal ===================== */
typedef struct { bool valid; int x, y; } Site;

int main(void) {
    /* --- calculs --- */
    real_dem = load_real_dem("data/input/dem.f32");
    if (real_dem) load_cell_size("data/input/dem.txt");
    if (!real_dem) generate_synthetic_dem();
    compute_slopes();
    real_ephem = load_ephem("data/input/ephem.csv");
    if (!real_ephem) generate_mock_ephem();
    compute_visibility();

    /* --- fenêtre --- */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1440, 860, "Selene - Planificateur de site lunaire");
    SetWindowMinSize(900, 600);
    SetTargetFPS(60);

    build_mesh();
    UploadMesh(&mesh, true);
    Material mat = LoadMaterialDefault();

    int have_globe = load_globe();
    Material gmat = LoadMaterialDefault();
    if (have_globe) SetMaterialTexture(&gmat, MATERIAL_MAP_DIFFUSE, g_tex);
    float zone_lat = -90.0f, zone_lon = 0.0f;
    int zone_valid = real_dem && load_zone_center("data/input/dem.txt", &zone_lat, &zone_lon);

    int mode = have_globe ? 0 : 1;               /* 0 = globe, 1 = site */
    float gyaw = 0.0f, gpitch = 0.3f, gdist = 260.0f, gexag = 15.0f, prev_gexag = -1.0f;
    int gpick_valid = 0;
    float gpick_lat = 0, gpick_lon = 0;

    float exag = 2.0f, smax = 15.0f, time_f = 0.0f;
    int layer = 0, prev_t = -1, prev_layer = -1;
    float prev_smax = -1, prev_exag = -1;
    bool eng = false, playing = false;
    float play_acc = 0.0f;
    Site site = {0};

    float yaw = 0.0f, pitch = 0.9f, dist = 120.0f;
    Camera3D cam = {0};
    cam.up = (Vector3){0, 1, 0}; cam.fovy = 45.0f; cam.projection = CAMERA_PERSPECTIVE;

    RenderTexture2D rt = {0};
    int rt_w = 0, rt_h = 0;
    Vector2 press = {0};
    bool press_in_view = false;
    const char *layer_names[4] = {"Éclairement solaire", "Visibilité de la Terre", "Altitude", "Pente"};

    while (!WindowShouldClose()) {
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        int gm = (mode == 0);
        Rectangle view = {PANEL_W, 0, (float)(sw - PANEL_W), (float)(gm ? sh : sh - TL_H)};
        Rectangle tl = {PANEL_W, (float)(sh - TL_H), (float)(sw - PANEL_W), TL_H};
        Vector2 m = GetMousePosition();
        int vw = (int)view.width, vh = (int)view.height;
        if (vw != rt_w || vh != rt_h) {
            if (rt.id) UnloadRenderTexture(rt);
            rt = LoadRenderTexture(vw, vh);
            rt_w = vw; rt_h = vh;
        }

        /* --- caméra orbitale (réglages propres à chaque vue) --- */
        float *cy = gm ? &gyaw : &yaw, *cp = gm ? &gpitch : &pitch, *cd = gm ? &gdist : &dist;
        float pmin = gm ? -1.5f : 0.12f, dmin = gm ? 130.0f : 25.0f, dmax = gm ? 600.0f : 400.0f;
        bool in_view = CheckCollisionPointRec(m, view);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { press = m; press_in_view = in_view; }
        if (press_in_view && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            *cy -= d.x * 0.006f;
            *cp = Clamp(*cp + d.y * 0.006f, pmin, 1.5f);
        }
        if (in_view) *cd = Clamp(*cd * (1.0f - GetMouseWheelMove() * 0.08f), dmin, dmax);
        cam.target = (Vector3){0, 0, 0};
        cam.position = (Vector3){*cd * cosf(*cp) * sinf(*cy), *cd * sinf(*cp), *cd * cosf(*cp) * cosf(*cy)};

        /* --- clic = sélection --- */
        if (press_in_view && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && Vector2Distance(m, press) < 4.0f) {
            Ray ray = GetScreenToWorldRayEx((Vector2){m.x - view.x, m.y - view.y}, cam, vw, vh);
            if (!gm) {
                RayCollision hit = GetRayCollisionMesh(ray, mesh, MatrixIdentity());
                if (hit.hit) {
                    site.valid = true;
                    site.x = (int)Clamp(roundf(hit.point.x + GW / 2.0f), 0, GW - 1);
                    site.y = (int)Clamp(roundf(hit.point.z + GH / 2.0f), 0, GH - 1);
                }
            } else {
                RayCollision hit = GetRayCollisionSphere(ray, (Vector3){0, 0, 0}, GLOBE_R);
                if (hit.hit) {
                    Vector3 n = Vector3Normalize(hit.point);
                    gpick_lat = asinf(n.y) * RAD2DEG;
                    gpick_lon = atan2f(-n.z, n.x) * RAD2DEG;
                    gpick_valid = 1;
                }
            }
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) press_in_view = false;

        /* --- lecture automatique --- */
        if (playing) {
            play_acc += GetFrameTime();
            while (play_acc > 0.07f) { play_acc -= 0.07f; time_f += 1.0f; if (time_f >= N_STEPS) time_f = 0; }
        }
        int t = (int)Clamp(time_f, 0, N_STEPS - 1);

        /* --- mises à jour du terrain si nécessaire --- */
        if (exag != prev_exag) { update_geometry(exag); prev_exag = exag; prev_t = -1; }
        if (t != prev_t || layer != prev_layer || smax != prev_smax) {
            recolor(layer, t, smax);
            prev_t = t; prev_layer = layer; prev_smax = smax;
        }
        if (gm && have_globe) {
            if (gexag != prev_gexag) { globe_update_geometry(gexag); prev_gexag = gexag; }
            Vector3 to_cam = Vector3Normalize(cam.position);
            Vector3 right = Vector3Normalize(Vector3CrossProduct(Vector3Negate(to_cam), (Vector3){0, 1, 0}));
            Vector3 up = Vector3CrossProduct(right, Vector3Negate(to_cam));
            Vector3 light = Vector3Add(to_cam, Vector3Add(Vector3Scale(right, -0.55f), Vector3Scale(up, 0.45f)));
            globe_shade(Vector3Normalize(light));
        }

        /* --- rendu 3D dans la texture --- */
        BeginTextureMode(rt);
        ClearBackground(C_BG);
        BeginMode3D(cam);
        rlDisableBackfaceCulling();
        if (gm) {
            if (have_globe) {
                DrawMesh(gmesh, gmat, MatrixIdentity());
                if (zone_valid) DrawSphere(Vector3Scale(sph(zone_lat, zone_lon), GLOBE_R * 1.02f), 1.1f, C_SUN);
                if (gpick_valid) DrawSphere(Vector3Scale(sph(gpick_lat, gpick_lon), GLOBE_R * 1.02f), 0.9f, WHITE);
            }
        } else {
            DrawMesh(mesh, mat, MatrixIdentity());
            if (site.valid) {
                int k = site.y * GW + site.x;
                DrawSphere((Vector3){site.x - GW / 2.0f, dem[k] / cell_m * exag + 1.2f, site.y - GH / 2.0f}, 1.0f, WHITE);
            }
        }
        EndMode3D();
        EndTextureMode();

        /* --- dessin 2D --- */
        BeginDrawing();
        ClearBackground(C_BG);
        DrawTextureRec(rt.texture, (Rectangle){0, 0, (float)vw, -(float)vh}, (Vector2){view.x, view.y}, WHITE);
        DrawText(gm ? "Glisser = tourner  |  molette = zoom  |  clic = marquer un point"
                    : "Rouge : pente au-dessus du seuil  |  glisser = tourner, molette = zoom, clic = choisir un site",
                 (int)view.x + 12, 10, 14, C_MUT);

        /* panneau gauche */
        DrawRectangle(0, 0, PANEL_W, sh, C_PANEL);
        DrawRectangle(PANEL_W - 1, 0, 1, sh, C_LINE);
        float y = 16;
        DrawText("Selene", 18, (int)y, 26, C_TXT); y += 32;
        DrawText(TextFormat("Zone %.1f km  |  %.0f m par cellule", GW * cell_m / 1000.0f, cell_m), 18, (int)y, 14, C_MUT); y += 30;

        if (ui_button((Rectangle){18, y, 134, 28}, "Globe", gm) && have_globe) mode = 0;
        if (ui_button((Rectangle){158, y, 134, 28}, "Site", !gm)) mode = 1;
        y += 42;

        if (gm) {
            if (!have_globe) {
                DrawText("Globe indisponible.", 18, (int)y, 16, C_BAD);
                DrawText("Lancez tools/prepare_globe.py", 18, (int)y + 22, 14, C_MUT);
            } else {
                ui_slider(18, y, 274, "Exagération du relief", &gexag, 1, 40, TextFormat("x%.0f", gexag)); y += 58;
                DrawRectangle(18, (int)y, 274, 1, C_LINE); y += 12;
                if (zone_valid) {
                    DrawText(TextFormat("Zone préparée : %.1f, %.1f", zone_lat, zone_lon), 18, (int)y, 14, C_SUN); y += 20;
                    DrawText("(point orange, bouton Site pour l'ouvrir)", 18, (int)y, 14, C_MUT); y += 30;
                }
                if (gpick_valid) {
                    int gi = (int)Clamp(roundf((gpick_lon + 180.0f) / 360.0f * GLON), 0, GLON);
                    int gj = (int)Clamp(roundf((90.0f - gpick_lat) / 180.0f * GLAT), 0, GLAT);
                    DrawText("Point marqué", 18, (int)y, 18, C_TXT); y += 26;
                    DrawText(TextFormat("Latitude : %.2f°", gpick_lat), 18, (int)y, 16, C_TXT); y += 22;
                    DrawText(TextFormat("Longitude : %.2f°", gpick_lon), 18, (int)y, 16, C_TXT); y += 22;
                    DrawText(TextFormat("Altitude : %.0f m", g_h[gj * (GLON + 1) + gi]), 18, (int)y, 16, C_TXT); y += 22;
                } else {
                    DrawText("Cliquez sur la Lune pour marquer un point.", 18, (int)y, 14, C_MUT);
                }
                DrawText("Relief LOLA et couleurs LRO (NASA)", 18, sh - 28, 14, C_MUT);
            }
        } else {
            DrawText("Couche affichée", 18, (int)y, 16, C_TXT); y += 24;
            for (int i = 0; i < 4; i++) {
                if (ui_button((Rectangle){18, y, 274, 26}, layer_names[i], layer == i)) layer = i;
                y += 30;
            }
            y += 6;
            if (ui_button((Rectangle){18, y, 274, 26}, eng ? "Mode ingénieur : activé" : "Mode ingénieur : désactivé", eng)) eng = !eng;
            y += 40;

            DrawText(TextFormat("Date : J+%.1f", t * STEP_H / 24.0f), 18, (int)y, 16, C_TXT);
            if (ui_button((Rectangle){190, y - 4, 102, 26}, playing ? "Pause" : "Lecture", playing)) playing = !playing;
            y += 28;
            ui_slider(18, y, 274, "", &time_f, 0, N_STEPS - 1, ""); y += 48;
            ui_slider(18, y, 274, "Pente maximale admise", &smax, 2, 30, TextFormat("%.0f°", smax)); y += 54;
            ui_slider(18, y, 274, "Exagération verticale", &exag, 1, 6, TextFormat("x%.1f", exag)); y += 58;

            /* fiche du site */
            DrawRectangle(18, (int)y, 274, 1, C_LINE); y += 12;
            if (!site.valid) {
                DrawText("Aucun site sélectionné.", 18, (int)y, 16, C_MUT);
                DrawText("Cliquez sur le terrain.", 18, (int)y + 22, 16, C_MUT);
            } else {
                int k = site.y * GW + site.x;
                float ps = pct_visible(illum, k), pe = pct_visible(earthv, k);
                DrawText(TextFormat("Soleil visible : %.0f %%", ps), 18, (int)y, 18, C_SUN); y += 26;
                DrawText(TextFormat("Terre visible : %.0f %%", pe), 18, (int)y, 18, C_EARTH); y += 26;
                DrawText(TextFormat("Plus longue nuit : %.1f j", longest_run_days(illum, k, 0)), 18, (int)y, 16, C_TXT); y += 22;
                DrawText(TextFormat("Plus longue coupure radio : %.1f j", longest_run_days(earthv, k, 0)), 18, (int)y, 16, C_TXT); y += 26;
                if (eng) {
                    DrawText(TextFormat("Cellule (x, y) : %d, %d", site.x, site.y), 18, (int)y, 14, C_MUT); y += 20;
                    DrawText(TextFormat("Altitude : %.0f m   Pente : %.1f°", dem[k], slope_deg[k]), 18, (int)y, 14, C_MUT); y += 20;
                    DrawText(TextFormat("Soleil az/él : %.0f° / %.1f°", sun_az[t], sun_el[t]), 18, (int)y, 14, C_MUT); y += 20;
                    DrawText(TextFormat("Terre az/él : %.0f° / %.1f°", earth_az[t], earth_el[t]), 18, (int)y, 14, C_MUT); y += 26;
                }
                bool ok = slope_deg[k] <= smax;
                DrawRectangle(18, (int)y, 274, 34, ok ? (Color){29, 58, 44, 255} : (Color){65, 35, 31, 255});
                DrawText(ok ? "Pente compatible avec un atterrissage" : TextFormat("Pente de %.0f° : trop forte", slope_deg[k]),
                         26, (int)y + 9, 15, ok ? C_OK : C_BAD);
                y += 46;
            }
            if (!real_dem || !real_ephem)
                DrawText("Données FICTIVES (terrain et/ou éphémérides)", 18, sh - 28, 14, C_BAD);

            /* timeline Soleil / Terre */
            DrawRectangleRec(tl, C_PANEL);
            DrawRectangle((int)tl.x, (int)tl.y, (int)tl.width, 1, C_LINE);
            float L = tl.x + 70, plotW = tl.width - 70 - 14, laneH = (tl.height - 16 - 30 - 10) / 2;
            float ly1 = tl.y + 16, ly2 = ly1 + laneH + 10;
            DrawRectangle((int)L, (int)ly1, (int)plotW, (int)laneH, C_SHADE);
            DrawRectangle((int)L, (int)ly2, (int)plotW, (int)laneH, C_SHADE);
            DrawText("Soleil", (int)tl.x + 12, (int)(ly1 + laneH / 2 - 8), 16, C_MUT);
            DrawText("Terre", (int)tl.x + 12, (int)(ly2 + laneH / 2 - 8), 16, C_MUT);
            if (site.valid) {
                int k = site.y * GW + site.x;
                for (int i = 0; i < N_STEPS; i++) {
                    float x0 = L + plotW * i / N_STEPS, w = plotW / N_STEPS + 1.0f;
                    if (illum[(size_t)i * NCELL + k]) DrawRectangleRec((Rectangle){x0, ly1, w, laneH}, C_SUN);
                    if (earthv[(size_t)i * NCELL + k]) DrawRectangleRec((Rectangle){x0, ly2, w, laneH}, C_EARTH);
                }
            } else {
                DrawText("Choisissez un site pour afficher ses fenêtres de lumière et de communication",
                         (int)L + 20, (int)(tl.y + tl.height / 2 - 16), 16, C_MUT);
            }
            float days = N_STEPS * STEP_H / 24.0f;
            for (int d = 0; d <= (int)days; d += 5) {
                float x = L + plotW * d / days;
                DrawRectangle((int)x, (int)(tl.y + tl.height - 30), 1, 5, C_MUT);
                DrawText(TextFormat("J+%d", d), (int)x - 12, (int)(tl.y + tl.height - 22), 14, C_MUT);
            }
            float cx = L + plotW * (t + 0.5f) / N_STEPS;
            DrawRectangle((int)cx - 1, (int)tl.y + 10, 2, (int)tl.height - 36, C_TXT);
            if (CheckCollisionPointRec(m, tl) && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_active == NULL) {
                float f = (m.x - L) / plotW;
                if (f >= 0 && f <= 1) { time_f = f * N_STEPS; if (time_f > N_STEPS - 1) time_f = N_STEPS - 1; }
            }
        }
        EndDrawing();
    }
    CloseWindow();
    return 0;
}