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
#define LUNAR_R_M 1737400.0
#define LUNAR_R_KM 1737.4
static float frame[NCELL * 9];              /* par cellule : verticale, "droite de l'image", "haut de l'image" (repère lunaire) */
static double sun_v[N_STEPS][3], earth_v[N_STEPS][3];   /* direction du Soleil, vecteur Terre (km), repère lunaire */
static int have_frame = 0;
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
/* Vrai si l'astre (azimut depuis le haut de l'image, élévation, en degrés) est visible depuis la cellule (x0, y0).
 * Le profil tient compte de la courbure de la Lune : le sol s'abaisse de s^2 / (2R) à la distance s. */
static uint8_t visible(int x0, int y0, double az_deg, double el_deg) {
    if (el_deg <= 0.0) return 0;
    double az = az_deg * PI / 180.0;
    double dx = sin(az), dy = -cos(az);              /* x = droite, y = bas de l'image */
    double tan_el = tan(el_deg * PI / 180.0);
    float h0 = dem[y0 * GW + x0];
    for (int d = 1;; d++) {
        double s = d * (double)cell_m, drop = s * s / (2.0 * LUNAR_R_M);
        if ((dem_max - h0) - drop <= s * tan_el) break;
        int ix = (int)lround(x0 + dx * d), iy = (int)lround(y0 + dy * d);
        if (ix < 0 || ix >= GW || iy < 0 || iy >= GH) break;
        if (dem[iy * GW + ix] - h0 - drop > s * tan_el) return 0;
    }
    return 1;
}

/* Hauteur et direction (dans le repère de l'image) d'un vecteur v vu depuis la cellule k */
static void cell_azel(int k, const double v[3], double *az, double *el) {
    const float *f = &frame[9 * k];
    double dn = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
    double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    *el = asin(dn / len) * 180.0 / PI;
    double hx = v[0] - dn * f[0], hy = v[1] - dn * f[1], hz = v[2] - dn * f[2];
    double right = hx * f[3] + hy * f[4] + hz * f[5], up = hx * f[6] + hy * f[7] + hz * f[8];
    *az = atan2(right, up) * 180.0 / PI;
}

static int load_frame(const char *dir) {
    char p[340];
    snprintf(p, sizeof p, "%s/frame.f32", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(frame, sizeof(float), (size_t)NCELL * 9, f);
    fclose(f);
    if (n != (size_t)NCELL * 9) return 0;
    snprintf(p, sizeof p, "%s/ephem_vec.csv", dir);
    f = fopen(p, "r");
    if (!f) return 0;
    char line[256];
    int k = 0;
    while (k < N_STEPS && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        double t, a, b, c, d, e, g;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &t, &a, &b, &c, &d, &e, &g) == 7) {
            sun_v[k][0] = a; sun_v[k][1] = b; sun_v[k][2] = c;
            earth_v[k][0] = d; earth_v[k][1] = e; earth_v[k][2] = g;
            k++;
        }
    }
    fclose(f);
    return k == N_STEPS;
}

static void compute_visibility(void) {
    for (int t = 0; t < N_STEPS; t++) {
        size_t off = (size_t)t * NCELL;
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++) {
                int k = y * GW + x;
                double sa = sun_az[t], se = sun_el[t], ea = earth_az[t], ee = earth_el[t];
                if (have_frame) {            /* géométrie propre à chaque cellule (courbure de la Lune incluse) */
                    const float *f = &frame[9 * k];
                    double v[3] = {earth_v[t][0] - LUNAR_R_KM * f[0], earth_v[t][1] - LUNAR_R_KM * f[1], earth_v[t][2] - LUNAR_R_KM * f[2]};
                    cell_azel(k, sun_v[t], &sa, &se);
                    cell_azel(k, v, &ea, &ee);
                }
                illum[off + k] = visible(x, y, sa, se);
                earthv[off + k] = visible(x, y, ea, ee);
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
static bool g_block = false;          /* vrai quand le menu déroulant est ouvert (les autres widgets ignorent la souris) */
static bool g_dd_open = false;

static bool ui_button(Rectangle r, const char *txt, bool on) {
    bool hot = !g_block && CheckCollisionPointRec(GetMousePosition(), r);
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
    if (!g_block && CheckCollisionPointRec(m, hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_active = v;
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


/* Menu déroulant : renvoie l'indice choisi, ou -1. Doit être appelé EN DERNIER (il se dessine par-dessus). */
static int ui_dropdown(Rectangle r, const char **items, int n, int sel) {
    Vector2 m = GetMousePosition();
    bool hot = CheckCollisionPointRec(m, r);
    DrawRectangleRec(r, hot ? C_HOT : C_BTN);
    DrawText(TextFormat("Point : %s", items[sel]), (int)r.x + 10, (int)r.y + 6, 16, C_TXT);
    DrawText("v", (int)(r.x + r.width - 18), (int)r.y + 6, 16, C_MUT);
    int chosen = -1;
    if (g_dd_open) {
        for (int i = 0; i < n; i++) {
            Rectangle it = {r.x, r.y + r.height + i * 26, r.width, 26};
            bool h = CheckCollisionPointRec(m, it);
            DrawRectangleRec(it, h ? C_HOT : C_BTN);
            DrawText(items[i], (int)it.x + 10, (int)it.y + 5, 16, i == sel ? C_SUN : C_TXT);
            if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) chosen = i;
        }
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (hot) g_dd_open = !g_dd_open;
        else if (g_dd_open) g_dd_open = false;
    }
    return chosen;
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

/* ===================== Vue du Sol (Horizon) ===================== */
/* Convertit (azimut deg depuis le haut/Nord, élévation deg) en vecteur unitaire dans le repère 3D du terrain */
static Vector3 azel_to_dir3d(double az_deg, double el_deg) {
    double az = az_deg * PI / 180.0;
    double el = el_deg * PI / 180.0;
    /* Dans le mesh : +X = Est (droite), -Z = Nord (haut de l'image), +Y = Zénith */
    return (Vector3){
        (float)(cos(el) * sin(az)),
        (float)(sin(el)),
        (float)(-cos(el) * cos(az))
    };
}

/* Recolore le terrain avec un éclairage directionnel réaliste basé sur le Soleil à l'instant t */
static void recolor_ground(int t, int site_k) {
    double sa = sun_az[t], se = sun_el[t];
    if (have_frame) cell_azel(site_k, sun_v[t], &sa, &se);
    Vector3 sun_dir = azel_to_dir3d(sa, se);
    bool sun_up = (se > -0.5);

    for (int k = 0; k < NCELL; k++) {
        Vector3 n = {mesh.normals[3 * k], mesh.normals[3 * k + 1], mesh.normals[3 * k + 2]};
        float diff = sun_up ? Vector3DotProduct(n, sun_dir) : 0.0f;
        if (diff < 0.0f) diff = 0.0f;
        /* Régolithe lunaire gris, faible lumière ambiante (clair de Terre / diffusion) */
        float lum = sun_up ? (0.06f + 0.88f * diff) : 0.04f;
        unsigned char c = (unsigned char)(Clamp(lum, 0.0f, 1.0f) * 255.0f);
        mesh.colors[4 * k] = c;
        mesh.colors[4 * k + 1] = c;
        mesh.colors[4 * k + 2] = (unsigned char)(Clamp(lum * 1.04f, 0.0f, 1.0f) * 255.0f);
        mesh.colors[4 * k + 3] = 255;
    }
    UpdateMeshBuffer(mesh, 3, mesh.colors, NCELL * 4, 0);
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

/* ===================== Points enregistrés et sites ===================== */
#define MAX_PTS 16
#define EPHEM_START "2026-12-01"   /* même date de départ pour tous les sites : indispensable pour les comparer */

typedef struct { char id[16]; char name[32]; float lat, lon; } SavedPt;   /* id = nom du dossier (stable), name = nom affiché (modifiable) */
static SavedPt pts[MAX_PTS];
static int npts = 0, next_id = 1;
static char status_msg[256] = "";

static void save_points(void) {
    FILE *f = fopen("data/points.txt", "w");
    if (!f) return;
    fprintf(f, "%d\n", next_id);
    for (int i = 0; i < npts; i++) fprintf(f, "%s %.5f %.5f %s\n", pts[i].id, pts[i].lat, pts[i].lon, pts[i].name);
    fclose(f);
}

static void load_points(void) {
    FILE *f = fopen("data/points.txt", "r");
    if (!f) return;
    char line[160];
    if (fgets(line, sizeof line, f)) next_id = atoi(line) > 0 ? atoi(line) : 1;
    while (npts < MAX_PTS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        SavedPt *p = &pts[npts];
        p->name[0] = 0;
        int n = sscanf(line, "%15s %f %f %31[^\n]", p->id, &p->lat, &p->lon, p->name);
        if (n < 3) continue;
        if (n == 3 || p->name[0] == 0) snprintf(p->name, sizeof p->name, "%s", p->id);   /* ancien format : nom = id */
        npts++;
    }
    fclose(f);
}

/* Valide un nouveau nom (espaces de début/fin retirés, vide refusé) */
static void commit_name(int idx, const char *buf) {
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%s", buf);
    int len = (int)strlen(tmp);
    while (len > 0 && tmp[len - 1] == ' ') tmp[--len] = 0;
    const char *b = tmp;
    while (*b == ' ') b++;
    if (*b == 0 || idx < 0 || idx >= npts) return;
    snprintf(pts[idx].name, sizeof pts[idx].name, "%s", b);
    save_points();
}

/* Zone initiale (data/input) : centre connu seulement si dem.txt l'indique */
static float zone_lat = -90.0f, zone_lon = 0.0f;
static int zone_valid = 0;

/* Niveaux de taille de zone : multiplicateurs du côté (le niveau 1 = taille d'origine) */
static const float zfac[5] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
static const char *zname[5] = {"zx0.5", "", "zx2", "zx4", "zx8"};

/* idx = -1 : zone initiale (data/input) ; sinon data/sites/<id> ; z = niveau de taille de zone */
static void site_dir(int idx, int z, char *out, size_t n) {
    char base[200];
    if (idx < 0) snprintf(base, sizeof base, "data/input");
    else snprintf(base, sizeof base, "data/sites/%s", pts[idx].id);
    if (z == 1) snprintf(out, n, "%s", base);
    else snprintf(out, n, "%s/%s", base, zname[z]);
}

static int site_prepared(int idx, int z) {
    char dir[300], p[340];
    site_dir(idx, z, dir, sizeof dir);
    snprintf(p, sizeof p, "%s/dem.f32", dir);
    if (!FileExists(p)) return 0;
    snprintf(p, sizeof p, "%s/ephem.csv", dir);
    return FileExists(p);
}

static void load_site_data(const char *dir) {
    char p[340];
    cell_m = 200.0f;
    snprintf(p, sizeof p, "%s/dem.f32", dir);
    real_dem = load_real_dem(p);
    if (real_dem) { snprintf(p, sizeof p, "%s/dem.txt", dir); load_cell_size(p); }
    else generate_synthetic_dem();
    compute_slopes();
    snprintf(p, sizeof p, "%s/ephem.csv", dir);
    real_ephem = load_ephem(p);
    if (!real_ephem) generate_mock_ephem();
    have_frame = real_dem && real_ephem && load_frame(dir);
    compute_visibility();
}

static void draw_message(const char *msg) {
    BeginDrawing();
    ClearBackground(C_BG);
    int w = MeasureText(msg, 22);
    DrawText(msg, (GetScreenWidth() - w) / 2, GetScreenHeight() / 2 - 11, 22, C_TXT);
    EndDrawing();
}

static void read_log_tail(void) {
    status_msg[0] = 0;
    FILE *f = fopen("data/prepare_log.txt", "r");
    if (!f) return;
    char line[256], last[256] = "";
    while (fgets(line, sizeof line, f)) if (line[0] != '\n' && line[0] != '\r') strcpy(last, line);
    fclose(f);
    last[strcspn(last, "\r\n")] = 0;
    snprintf(status_msg, sizeof status_msg, "%s", last);
}

/* Lance tools/prepare_data.py pour ce site et ce niveau de zone (DEM + éphémérides). Bloque quelques secondes. */
static void run_prepare(int idx, int z) {
    char dir[300], cmd[1400];
    site_dir(idx, z, dir, sizeof dir);
    float lat = idx < 0 ? zone_lat : pts[idx].lat, lon = idx < 0 ? zone_lon : pts[idx].lon;
    snprintf(cmd, sizeof cmd,
             "python tools/prepare_data.py --lat %.4f --lon %.4f --out %s --zoom %.2f --start %s > data\\prepare_log.txt 2>&1"
             " || py tools/prepare_data.py --lat %.4f --lon %.4f --out %s --zoom %.2f --start %s > data\\prepare_log.txt 2>&1",
             lat, lon, dir, zfac[z], EPHEM_START, lat, lon, dir, zfac[z], EPHEM_START);
    system(cmd);
    if (site_prepared(idx, z)) status_msg[0] = 0; else read_log_tail();
}

/* Boussole : indique où sont le pôle Sud (S) et le pôle Nord (N) par rapport à la vue.
 * Disque plein = pôle visible, anneau = caché derrière la Lune. */
static void draw_compass(Camera3D cam, Rectangle view, int vw, int vh) {
    Vector2 c0 = GetWorldToScreenEx((Vector3){0, 0, 0}, cam, vw, vh);
    Vector3 pole[2] = {{0, -GLOBE_R * 1.02f, 0}, {0, GLOBE_R * 1.02f, 0}};
    const char *lab[2] = {"S", "N"};
    Color col[2] = {C_SUN, C_MUT};
    Vector2 ctr = {view.x + view.width - 70, view.y + view.height - 70};
    DrawCircle((int)ctr.x, (int)ctr.y, 46, (Color){16, 21, 28, 220});
    DrawCircleLines((int)ctr.x, (int)ctr.y, 46, C_LINE);
    for (int i = 0; i < 2; i++) {
        Vector2 p = GetWorldToScreenEx(pole[i], cam, vw, vh);
        Vector2 d = {p.x - c0.x, p.y - c0.y};
        float len = sqrtf(d.x * d.x + d.y * d.y);
        bool front = Vector3DotProduct(Vector3Normalize(pole[i]), Vector3Normalize(cam.position)) > 0.0f;
        Vector2 tip = ctr;
        if (len > 8.0f) tip = (Vector2){ctr.x + d.x / len * 32.0f, ctr.y + d.y / len * 32.0f};
        DrawLineEx(ctr, tip, 2.0f, col[i]);
        if (front) DrawCircle((int)tip.x, (int)tip.y, 6, col[i]); else DrawCircleLines((int)tip.x, (int)tip.y, 6, col[i]);
        DrawText(lab[i], (int)tip.x - 3 + (int)((tip.x - ctr.x) * 0.3f), (int)tip.y - 7 + (int)((tip.y - ctr.y) * 0.3f), 14, WHITE);
        if (i == 0 && front) {   /* repère directement sur la Lune */
            DrawCircleLines((int)(view.x + p.x), (int)p.y, 7, C_SUN);
            DrawText("Pôle Sud", (int)(view.x + p.x) + 10, (int)p.y - 7, 14, C_SUN);
        }
    }
}

/* ===================== Échelle (onglet Site) ===================== */
/* Longueur "ronde" de la barre d'échelle, environ 1/4 du côté de la zone */
static float nice_len_m(void) {
    static const float steps[] = {200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000};
    float target = 0.25f * GW * cell_m, best = steps[0];
    for (int i = 0; i < 10; i++) if (steps[i] <= target) best = steps[i];
    return best;
}

/* Barre le long du bord sud du terrain (elle suit le relief) */
static void draw_scale_3d(float exag) {
    int n = (int)roundf(nice_len_m() / cell_m);
    if (n > GW - 2) n = GW - 2;
    if (n < 1) n = 1;
    float zrow = (GH - 1) - GH / 2.0f;
    Vector3 prev = {0};
    for (int i = 0; i <= n; i++) {
        Vector3 p = {i - GW / 2.0f, dem[(GH - 1) * GW + i] / cell_m * exag + 0.6f, zrow};
        if (i > 0) DrawCylinderEx(prev, p, 0.35f, 0.35f, 6, WHITE);
        prev = p;
    }
    Vector3 a = {-GW / 2.0f, dem[(GH - 1) * GW] / cell_m * exag + 0.6f, zrow};
    DrawSphere(a, 0.8f, WHITE);
    DrawSphere(prev, 0.8f, WHITE);
}

static void draw_scale_label(Camera3D cam, Rectangle view, int vw, int vh, float exag) {
    float len_m = nice_len_m();
    int n = (int)roundf(len_m / cell_m);
    if (n > GW - 2) n = GW - 2;
    if (n < 1) n = 1;
    int im = n / 2;
    float zrow = (GH - 1) - GH / 2.0f;
    Vector3 w = {im - GW / 2.0f, dem[(GH - 1) * GW + im] / cell_m * exag + 2.5f, zrow};
    Vector2 sp = GetWorldToScreenEx(w, cam, vw, vh);
    const char *txt = len_m >= 1000.0f ? TextFormat("%.0f km", len_m / 1000.0f) : TextFormat("%.0f m", len_m);
    int tw = MeasureText(txt, 16);
    int x = (int)(view.x + sp.x) - tw / 2, y = (int)sp.y - 8;
    DrawRectangle(x - 5, y - 2, tw + 10, 20, (Color){16, 21, 28, 215});
    DrawText(txt, x, y, 16, WHITE);
}

/* ===================== Programme principal ===================== */
typedef struct { bool valid; int x, y; } Site;

int main(void) {
    /* --- données initiales : zone préparée dans data/input (ou fictive) --- */
    load_points();
    load_site_data("data/input");
    zone_valid = real_dem && load_zone_center("data/input/dem.txt", &zone_lat, &zone_lon);

    /* --- fenêtre --- */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1440, 860, "Selene - Planificateur de site lunaire");
    SetWindowMinSize(900, 700);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);                        /* Échap sert à annuler une saisie, il ne ferme plus la fenêtre */

    build_mesh();
    UploadMesh(&mesh, true);
    Material mat = LoadMaterialDefault();

    int have_globe = load_globe();
    Material gmat = LoadMaterialDefault();
    if (have_globe) SetMaterialTexture(&gmat, MATERIAL_MAP_DIFFUSE, g_tex);

    int mode = have_globe ? 0 : 1;               /* 0 = globe, 1 = site, 2 = sol */
    int cur_site = -1;                           /* -1 = zone initiale, sinon indice dans pts[] */
    int site_ready = 1;
    int edit_idx = -1;                           /* point en cours de renommage (-1 = aucun) */
    char edit_buf[32] = "";
    Rectangle edit_rect = {0, 0, 0, 0};
    int pending_load = -2, pending_prepare = -2;
    int zlev = 1;                                /* niveau de taille de zone (index dans zfac) */
    float gyaw = 0.0f, gpitch = 0.3f, gdist = 260.0f, gexag = 15.0f, prev_gexag = -1.0f;
    int gpick_valid = 0;
    float gpick_lat = 0, gpick_lon = 0;

    float exag = 2.0f, smax = 15.0f, time_f = 0.0f;
    int layer = 0, prev_t = -1, prev_layer = -1, prev_mode = -1;
    float prev_smax = -1, prev_exag = -1;
    bool eng = false, playing = false;
    float play_acc = 0.0f;
    Site site = {true, GW / 2, GH / 2};

    float yaw = 0.0f, pitch = 0.9f, dist = 120.0f;
    float gnd_yaw = 0.0f, gnd_pitch = 0.05f;     /* orientation de la tête en vue du sol (0 = Nord) */
    Camera3D cam = {0};
    cam.up = (Vector3){0, 1, 0}; cam.fovy = 45.0f; cam.projection = CAMERA_PERSPECTIVE;

    RenderTexture2D rt = {0};
    int rt_w = 0, rt_h = 0;
    Vector2 press = {0};
    bool press_in_view = false;
    const char *layer_names[4] = {"Éclairement solaire", "Visibilité de la Terre", "Altitude", "Pente"};

    while (!WindowShouldClose()) {
        /* --- tâches lourdes demandées à l'image précédente --- */
        if (pending_prepare != -2) {
            int idx = pending_prepare; pending_prepare = -2;
            draw_message("Préparation du site (quelques secondes)...");
            run_prepare(idx, zlev);
            if (site_prepared(idx, zlev)) pending_load = idx;
        }
        if (pending_load != -2) {
            int idx = pending_load; pending_load = -2;
            cur_site = idx;
            if (!(idx < 0 && zlev == 1) && !site_prepared(idx, zlev)) {
                site_ready = 0;
            } else {
                draw_message("Calcul de l'éclairement et des communications...");
                char dir[300];
                site_dir(idx, zlev, dir, sizeof dir);
                load_site_data(dir);
                site_ready = 1;
                site.valid = true; site.x = GW / 2; site.y = GH / 2;
                prev_exag = -1.0f; prev_t = -1;
            }
        }
        g_block = g_dd_open;

        /* --- saisie du nouveau nom d'un point --- */
        if (edit_idx >= 0 && mode != 0) { commit_name(edit_idx, edit_buf); edit_idx = -1; }
        if (edit_idx >= 0) {
            int ch;
            while ((ch = GetCharPressed()) > 0) {
                int len = (int)strlen(edit_buf);
                if (ch >= 32 && ch != 127 && GetCodepointCount(edit_buf) < 16 && len < 27) {
                    int sz = 0;
                    const char *u = CodepointToUTF8(ch, &sz);
                    memcpy(edit_buf + len, u, sz);
                    edit_buf[len + sz] = 0;
                }
            }
            if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
                int len = (int)strlen(edit_buf);
                while (len > 0 && ((unsigned char)edit_buf[len - 1] & 0xC0) == 0x80) len--;   /* caractère UTF-8 entier */
                if (len > 0) edit_buf[len - 1] = 0;
            }
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) { commit_name(edit_idx, edit_buf); edit_idx = -1; }
            else if (IsKeyPressed(KEY_ESCAPE)) edit_idx = -1;
            else if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !CheckCollisionPointRec(GetMousePosition(), edit_rect)) {
                commit_name(edit_idx, edit_buf);
                edit_idx = -1;
            }
        }

        int sw = GetScreenWidth(), sh = GetScreenHeight();
        int gm = (mode == 0);
        int sm = (mode == 2);                    /* mode vue du sol */
        float eff_exag = sm ? 1.0f : exag;       /* en vue du sol, relief réel (x1) indispensable pour un horizon exact */

        Rectangle view = {PANEL_W, 0, (float)(sw - PANEL_W), (float)(gm ? sh : sh - TL_H)};
        Rectangle tl = {PANEL_W, (float)(sh - TL_H), (float)(sw - PANEL_W), TL_H};
        Vector2 m = GetMousePosition();
        int vw = (int)view.width, vh = (int)view.height;
        if (vw != rt_w || vh != rt_h) {
            if (rt.id) UnloadRenderTexture(rt);
            rt = LoadRenderTexture(vw, vh);
            rt_w = vw; rt_h = vh;
        }

        /* --- caméra (orbitale pour Globe/Site, première personne pour Sol) --- */
        float *cy = gm ? &gyaw : (sm ? &gnd_yaw : &yaw);
        float *cp = gm ? &gpitch : (sm ? &gnd_pitch : &pitch);
        float *cd = gm ? &gdist : &dist;
        float pmin = gm ? -1.5f : (sm ? -0.45f : 0.12f);
        float pmax = sm ? 1.25f : 1.5f;
        float dmin = gm ? 130.0f : 25.0f, dmax = gm ? 600.0f : 400.0f;

        Rectangle zo = {view.x + view.width - 262, 6, 256, 30};      /* boutons de taille de zone */
        bool in_view = CheckCollisionPointRec(m, view) && (gm || !CheckCollisionPointRec(m, zo));
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { press = m; press_in_view = in_view; }
        if (press_in_view && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            if (sm) {
                *cy += d.x * 0.005f;
                *cp = Clamp(*cp - d.y * 0.005f, pmin, pmax);
            } else {
                *cy -= d.x * 0.006f;
                *cp = Clamp(*cp + d.y * 0.006f, pmin, pmax);
            }
        }
        if (in_view && !sm) *cd = Clamp(*cd * (1.0f - GetMouseWheelMove() * 0.08f), dmin, dmax);

        if (sm && site.valid) {
            int k = site.y * GW + site.x;
            float eye_h = dem[k] / cell_m + fmaxf(15.0f / cell_m, 0.25f);
            cam.position = (Vector3){site.x - GW / 2.0f, eye_h, site.y - GH / 2.0f};
            Vector3 look = {
                cosf(gnd_pitch) * sinf(gnd_yaw),
                sinf(gnd_pitch),
                -cosf(gnd_pitch) * cosf(gnd_yaw)
            };
            cam.target = Vector3Add(cam.position, look);
            cam.fovy = 55.0f;
        } else {
            cam.fovy = 45.0f;
            cam.target = (Vector3){0, 0, 0};
            cam.position = (Vector3){*cd * cosf(*cp) * sinf(*cy), *cd * sinf(*cp), *cd * cosf(*cp) * cosf(*cy)};
        }

        /* --- clic = sélection --- */
        if (press_in_view && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && Vector2Distance(m, press) < 4.0f) {
            Ray ray = GetScreenToWorldRayEx((Vector2){m.x - view.x, m.y - view.y}, cam, vw, vh);
            if (!gm && !sm) {
                if (site_ready) {
                    RayCollision hit = GetRayCollisionMesh(ray, mesh, MatrixIdentity());
                    if (hit.hit) {
                        site.valid = true;
                        site.x = (int)Clamp(roundf(hit.point.x + GW / 2.0f), 0, GW - 1);
                        site.y = (int)Clamp(roundf(hit.point.z + GH / 2.0f), 0, GH - 1);
                    }
                }
            } else if (gm) {
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

        /* --- calculs azimut / élévation du Soleil et de la Terre pour le site actif --- */
        double cur_sa = sun_az[t], cur_se = sun_el[t], cur_ea = earth_az[t], cur_ee = earth_el[t];
        if (site.valid) {
            int k = site.y * GW + site.x;
            if (have_frame) {
                const float *f = &frame[9 * k];
                double v[3] = {
                    earth_v[t][0] - LUNAR_R_KM * f[0],
                    earth_v[t][1] - LUNAR_R_KM * f[1],
                    earth_v[t][2] - LUNAR_R_KM * f[2]
                };
                cell_azel(k, sun_v[t], &cur_sa, &cur_se);
                cell_azel(k, v, &cur_ea, &cur_ee);
            }
        }

        /* --- mises à jour du terrain si nécessaire --- */
        if (eff_exag != prev_exag) { update_geometry(eff_exag); prev_exag = eff_exag; prev_t = -1; }
        if (t != prev_t || layer != prev_layer || smax != prev_smax || mode != prev_mode) {
            if (sm && site.valid) recolor_ground(t, site.y * GW + site.x);
            else recolor(layer, t, smax);
            prev_t = t; prev_layer = layer; prev_smax = smax; prev_mode = mode;
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
        ClearBackground(sm ? (Color){4, 6, 10, 255} : C_BG);
        BeginMode3D(cam);
        rlDisableBackfaceCulling();
        if (gm) {
            if (have_globe) {
                DrawMesh(gmesh, gmat, MatrixIdentity());
                if (zone_valid) DrawSphere(Vector3Scale(sph(zone_lat, zone_lon), GLOBE_R * 1.02f), 0.8f, C_MUT);
                for (int i = 0; i < npts; i++)
                    DrawSphere(Vector3Scale(sph(pts[i].lat, pts[i].lon), GLOBE_R * 1.02f), i == cur_site ? 1.4f : 0.9f,
                               i == cur_site ? C_SUN : C_EARTH);
                if (gpick_valid) DrawSphere(Vector3Scale(sph(gpick_lat, gpick_lon), GLOBE_R * 1.02f), 0.9f, WHITE);
            }
        } else if (site_ready) {
            DrawMesh(mesh, mat, MatrixIdentity());
            if (sm && site.valid) {
                int k = site.y * GW + site.x;
                float sky_r = 220.0f; /* distance d'affichage sur la voûte céleste */
                Vector3 s_pos = Vector3Add(cam.position, Vector3Scale(azel_to_dir3d(cur_sa, cur_se), sky_r));
                Vector3 e_pos = Vector3Add(cam.position, Vector3Scale(azel_to_dir3d(cur_ea, cur_ee), sky_r));

                bool s_vis = illum[(size_t)t * NCELL + k];
                bool e_vis = earthv[(size_t)t * NCELL + k];

                /* Soleil (halo + disque) */
                DrawSphere(s_pos, s_vis ? 5.5f : 3.5f, s_vis ? (Color){255, 210, 100, 90} : (Color){120, 90, 40, 60});
                DrawSphere(s_pos, 3.0f, s_vis ? (Color){255, 245, 200, 255} : (Color){140, 105, 50, 160});

                /* Terre (halo atmosphérique + globe bleu) */
                DrawSphere(e_pos, e_vis ? 6.5f : 4.5f, e_vis ? (Color){88, 166, 232, 80} : (Color){50, 80, 110, 50});
                DrawSphere(e_pos, 4.2f, e_vis ? C_EARTH : (Color){60, 95, 130, 160});
            } else {
                draw_scale_3d(eff_exag);
                if (site.valid) {
                    int k = site.y * GW + site.x;
                    DrawSphere((Vector3){site.x - GW / 2.0f, dem[k] / cell_m * eff_exag + 1.2f, site.y - GH / 2.0f}, 1.0f, WHITE);
                }
            }
        }
        EndMode3D();
        EndTextureMode();

        /* --- dessin 2D --- */
        BeginDrawing();
        ClearBackground(C_BG);
        DrawTextureRec(rt.texture, (Rectangle){0, 0, (float)vw, -(float)vh}, (Vector2){view.x, view.y}, WHITE);

        if (gm && have_globe) {
            for (int i = 0; i < npts; i++) {
                Vector3 wp = Vector3Scale(sph(pts[i].lat, pts[i].lon), GLOBE_R * 1.02f);
                if (Vector3DotProduct(Vector3Normalize(wp), Vector3Normalize(cam.position)) > 0.15f) {
                    Vector2 sp = GetWorldToScreenEx(wp, cam, vw, vh);
                    DrawText(pts[i].name, (int)(view.x + sp.x) + 8, (int)sp.y - 8, 14, WHITE);
                }
            }
            draw_compass(cam, view, vw, vh);
        }

        if (sm && site_ready && site.valid) {
            /* Repères cardinaux (N, E, S, O) sur l'horizon */
            const char *card[4] = {"N", "E", "S", "O"};
            Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
            for (int c = 0; c < 4; c++) {
                Vector3 dir = azel_to_dir3d(c * 90.0, 0.0);
                if (Vector3DotProduct(dir, fwd) > 0.2f) {
                    Vector2 sp = GetWorldToScreenEx(Vector3Add(cam.position, Vector3Scale(dir, 200.0f)), cam, vw, vh);
                    DrawText(card[c], (int)(view.x + sp.x) - 4, (int)sp.y - 8, 16, C_MUT);
                }
            }
            /* Étiquettes Soleil et Terre dans le ciel */
            int k = site.y * GW + site.x;
            Vector3 sd = azel_to_dir3d(cur_sa, cur_se);
            if (Vector3DotProduct(sd, fwd) > 0.2f) {
                Vector2 sp = GetWorldToScreenEx(Vector3Add(cam.position, Vector3Scale(sd, 220.0f)), cam, vw, vh);
                bool vis = illum[(size_t)t * NCELL + k];
                DrawText(vis ? TextFormat("Soleil (%.1f°)", cur_se) : "Soleil (masqué)",
                         (int)(view.x + sp.x) + 12, (int)sp.y - 8, 14, vis ? C_SUN : C_MUT);
            }
            Vector3 ed = azel_to_dir3d(cur_ea, cur_ee);
            if (Vector3DotProduct(ed, fwd) > 0.2f) {
                Vector2 sp = GetWorldToScreenEx(Vector3Add(cam.position, Vector3Scale(ed, 220.0f)), cam, vw, vh);
                bool vis = earthv[(size_t)t * NCELL + k];
                DrawText(vis ? TextFormat("Terre (%.1f°)", cur_ee) : "Terre (masquée)",
                         (int)(view.x + sp.x) + 14, (int)sp.y - 8, 14, vis ? C_EARTH : C_MUT);
            }
        }

        if (!gm && !sm && site_ready) draw_scale_label(cam, view, vw, vh, eff_exag);
        if (!gm && !site_ready) {
            DrawText("Site non préparé", (int)view.x + vw / 2 - 70, vh / 2 - 10, 20, C_MUT);
        }
        if (!gm) {
            DrawRectangleRec(zo, (Color){16, 21, 28, 215});
            bool can_zoom = cur_site >= 0 || zone_valid;
            const char *zl = site_ready ? TextFormat("Zone : %.1f km", GW * cell_m / 1000.0f) : TextFormat("Zone : x%.1f", zfac[zlev]);
            DrawText(zl, (int)(zo.x + zo.width / 2) - MeasureText(zl, 16) / 2, (int)zo.y + 7, 16, can_zoom ? C_TXT : C_MUT);
            if (ui_button((Rectangle){zo.x + 4, zo.y + 3, 34, 24}, "-", false) && can_zoom && zlev > 0) { zlev--; pending_load = cur_site; }
            if (ui_button((Rectangle){zo.x + zo.width - 38, zo.y + 3, 34, 24}, "+", false) && can_zoom && zlev < 4) { zlev++; pending_load = cur_site; }
        }
        DrawText(gm ? "Glisser = tourner  |  molette = zoom  |  clic = marquer un point"
                    : (sm ? "Glisser = regarder autour (360°)  |  relief réel x1"
                          : "Rouge : pente au-dessus du seuil  |  glisser = tourner, molette = zoom, clic = choisir un site"),
                 (int)view.x + 12, 10, 14, C_MUT);

        /* panneau gauche */
        DrawRectangle(0, 0, PANEL_W, sh, C_PANEL);
        DrawRectangle(PANEL_W - 1, 0, 1, sh, C_LINE);
        float y = 16;
        DrawText("Selene", 18, (int)y, 26, C_TXT); y += 32;
        if (gm) { DrawText("Lune entière  |  rayon 1737 km", 18, (int)y, 14, C_MUT); y += 26; }
        else if (site_ready) {
            float side = GW * cell_m / 1000.0f;
            DrawText(TextFormat("Carré de %.1f x %.1f km (%.0f km²)", side, side, side * side), 18, (int)y, 14, C_MUT); y += 18;
            DrawText(TextFormat("Cellule : %.0f m  |  relief x%.1f", cell_m, eff_exag), 18, (int)y, 14, C_MUT); y += 24;
        } else y += 26;

        /* 3 onglets : Globe | Site | Sol */
        if (ui_button((Rectangle){18, y, 88, 28}, "Globe", gm) && have_globe) mode = 0;
        if (ui_button((Rectangle){111, y, 88, 28}, "Site", mode == 1)) mode = 1;
        if (ui_button((Rectangle){204, y, 88, 28}, "Sol", sm)) {
            mode = 2;
            if (!site.valid) { site.valid = true; site.x = GW / 2; site.y = GH / 2; }
        }
        y += 40;

        if (gm) {
            if (!have_globe) {
                DrawText("Globe indisponible.", 18, (int)y, 16, C_BAD);
                DrawText("Lancez tools/prepare_globe.py", 18, (int)y + 22, 14, C_MUT);
            } else {
                ui_slider(18, y, 274, "Exagération du relief", &gexag, 1, 40, TextFormat("x%.0f", gexag)); y += 50;
                if (ui_button((Rectangle){18, y, 134, 26}, "Pôle Sud", false)) { gpitch = -1.45f; gyaw = 0.0f; }
                if (ui_button((Rectangle){158, y, 134, 26}, "Pôle Nord", false)) { gpitch = 1.45f; gyaw = 0.0f; }
                y += 36;
                DrawRectangle(18, (int)y, 274, 1, C_LINE); y += 10;
                if (gpick_valid) {
                    int gi = (int)Clamp(roundf((gpick_lon + 180.0f) / 360.0f * GLON), 0, GLON);
                    int gj = (int)Clamp(roundf((90.0f - gpick_lat) / 180.0f * GLAT), 0, GLAT);
                    DrawText(TextFormat("Point marqué : %.2f, %.2f", gpick_lat, gpick_lon), 18, (int)y, 16, C_TXT); y += 22;
                    DrawText(TextFormat("Altitude : %.0f m", g_h[gj * (GLON + 1) + gi]), 18, (int)y, 16, C_MUT); y += 26;
                    if (ui_button((Rectangle){18, y, 274, 28}, "Enregistrer ce point", false)) {
                        if (npts < MAX_PTS) {
                            snprintf(pts[npts].id, sizeof pts[npts].id, "P%d", next_id++);
                            snprintf(pts[npts].name, sizeof pts[npts].name, "%s", pts[npts].id);
                            pts[npts].lat = gpick_lat; pts[npts].lon = gpick_lon;
                            npts++;
                            save_points();
                            status_msg[0] = 0;
                        } else snprintf(status_msg, sizeof status_msg, "Maximum de %d points atteint", MAX_PTS);
                    }
                    y += 38;
                } else {
                    DrawText("Cliquez sur la Lune pour marquer un point.", 18, (int)y, 14, C_MUT); y += 28;
                }
                DrawText("Points enregistrés", 18, (int)y, 16, C_TXT); y += 24;
                if (npts == 0) DrawText("Aucun pour l'instant.", 18, (int)y, 14, C_MUT);
                else DrawText("(cliquez sur un nom pour le modifier)", 18, sh - 70, 12, C_MUT);
                int del = -1;
                for (int i = 0; i < npts; i++) {
                    Rectangle nr = {18, y, 150, 24};
                    if (edit_idx == i) {                                  /* champ de saisie, comme un renommage de fichier */
                        edit_rect = nr;
                        DrawRectangleRec(nr, C_BG);
                        DrawRectangleLinesEx(nr, 1, C_SUN);
                        DrawText(TextFormat("%s%s", edit_buf, ((int)(GetTime() * 2.0) % 2) ? "|" : ""), 24, (int)y + 5, 14, C_TXT);
                    } else {
                        bool hot = !g_block && CheckCollisionPointRec(m, nr);
                        int nw = MeasureText(pts[i].name, 14);
                        DrawText(pts[i].name, 18, (int)y + 5, 14, i == cur_site ? C_SUN : C_TXT);
                        if (hot) DrawRectangle(18, (int)y + 21, nw, 1, C_MUT);
                        const char *co = TextFormat("%.1f, %.1f", pts[i].lat, pts[i].lon);
                        if (18 + nw + 8 + MeasureText(co, 12) < 168) DrawText(co, 18 + nw + 8, (int)y + 7, 12, C_MUT);
                        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && edit_idx < 0) {
                            edit_idx = i;
                            snprintf(edit_buf, sizeof edit_buf, "%s", pts[i].name);
                        }
                    }
                    DrawCircle(176, (int)y + 12, 4, site_prepared(i, 1) ? C_OK : C_LINE);
                    if (ui_button((Rectangle){190, y, 56, 24}, "Site", false)) { pending_load = i; zlev = 1; mode = 1; }
                    if (ui_button((Rectangle){252, y, 40, 24}, "X", false)) del = i;
                    y += 28;
                }
                if (del >= 0) {
                    edit_idx = -1;
                    for (int i = del; i < npts - 1; i++) pts[i] = pts[i + 1];
                    npts--;
                    save_points();
                    if (cur_site == del) { pending_load = -1; zlev = 1; }
                    else if (cur_site > del) cur_site--;
                }
                if (zone_valid) {
                    y += 8;
                    DrawText(TextFormat("Zone initiale : %.1f, %.1f (gris)", zone_lat, zone_lon), 18, (int)y, 14, C_MUT);
                }
                if (status_msg[0]) DrawText(TextSubtext(status_msg, 0, 40), 18, sh - 50, 14, C_BAD);
                DrawText("Relief LOLA et couleurs LRO (NASA)", 18, sh - 28, 14, C_MUT);
            }
        } else {
            Rectangle ddr = {18, y, 274, 28};
            y += 36;
            if (cur_site >= 0) {
                DrawText(TextFormat("lat %.3f   lon %.3f", pts[cur_site].lat, pts[cur_site].lon), 18, (int)y, 14, C_MUT);
                y += 22;
            }
            if (!site_ready) {
                float plat = cur_site >= 0 ? pts[cur_site].lat : zone_lat;
                DrawText(TextFormat("Zone x%.1f non préparée.", zfac[zlev]), 18, (int)y, 16, C_TXT); y += 28;
                if (cur_site >= 0 || zone_valid) {
                    if (ui_button((Rectangle){18, y, 274, 30}, "Préparer cette zone", false)) pending_prepare = cur_site;
                    y += 38;
                    DrawText("Hors ligne : nécessite Python et un fichier LDEM.", 18, (int)y, 14, C_MUT); y += 20;
                    DrawText(plat <= -81.0f ? "DEM : LOLA polaire (80 m)" : "DEM : mondial (ldem_64)", 18, (int)y, 14, C_MUT); y += 24;
                } else {
                    DrawText("Zone initiale : taille non modifiable.", 18, (int)y, 14, C_BAD); y += 24;
                }
                if (status_msg[0]) {
                    DrawText(TextSubtext(status_msg, 0, 40), 18, (int)y, 14, C_BAD); y += 18;
                    if (strlen(status_msg) > 40) DrawText(TextSubtext(status_msg, 40, 40), 18, (int)y, 14, C_BAD);
                }
            } else {
                if (!sm) {
                    DrawText("Couche affichée", 18, (int)y, 16, C_TXT); y += 22;
                    for (int i = 0; i < 4; i++) {
                        if (ui_button((Rectangle){18, y, 274, 22}, layer_names[i], layer == i)) layer = i;
                        y += 26;
                    }
                    y += 4;
                } else {
                    DrawText("Orientation rapide", 18, (int)y, 16, C_TXT); y += 22;
                    if (ui_button((Rectangle){18, y, 134, 26}, "Viser Soleil", false)) {
                        gnd_yaw = (float)(cur_sa * DEG2RAD);
                        gnd_pitch = Clamp((float)(cur_se * DEG2RAD), -0.45f, 1.25f);
                    }
                    if (ui_button((Rectangle){158, y, 134, 26}, "Viser Terre", false)) {
                        gnd_yaw = (float)(cur_ea * DEG2RAD);
                        gnd_pitch = Clamp((float)(cur_ee * DEG2RAD), -0.45f, 1.25f);
                    }
                    y += 34;
                }

                if (ui_button((Rectangle){18, y, 274, 24}, eng ? "Mode ingénieur : activé" : "Mode ingénieur : désactivé", eng)) eng = !eng;
                y += 34;

                DrawText(TextFormat("Date : J+%.1f", t * STEP_H / 24.0f), 18, (int)y, 16, C_TXT);
                if (ui_button((Rectangle){190, y - 4, 102, 26}, playing ? "Pause" : "Lecture", playing)) playing = !playing;
                y += 26;
                ui_slider(18, y, 274, "", &time_f, 0, N_STEPS - 1, ""); y += 42;
                ui_slider(18, y, 274, "Pente maximale admise", &smax, 2, 30, TextFormat("%.0f°", smax)); y += 50;
                if (!sm) {
                    ui_slider(18, y, 274, "Exagération verticale", &exag, 1, 6, TextFormat("x%.1f", exag)); y += 54;
                }

                /* fiche du site */
                DrawRectangle(18, (int)y, 274, 1, C_LINE); y += 10;
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
                        DrawText(TextFormat("Soleil az/él : %.0f° / %.1f°", cur_sa, cur_se), 18, (int)y, 14, C_MUT); y += 20;
                        DrawText(TextFormat("Terre az/él : %.0f° / %.1f°", cur_ea, cur_ee), 18, (int)y, 14, C_MUT); y += 26;
                    }
                    bool ok = slope_deg[k] <= smax;
                    DrawRectangle(18, (int)y, 274, 34, ok ? (Color){29, 58, 44, 255} : (Color){65, 35, 31, 255});
                    DrawText(ok ? "Pente compatible avec un atterrissage" : TextFormat("Pente de %.0f° : trop forte", slope_deg[k]),
                             26, (int)y + 9, 15, ok ? C_OK : C_BAD);
                    y += 46;
                }
                if (!real_dem || !real_ephem)
                    DrawText("Données FICTIVES (terrain et/ou éphémérides)", 18, sh - 28, 14, C_BAD);
                else if (!have_frame && (cur_site >= 0 || zone_valid)) {
                    DrawText("Géométrie approx.", 18, sh - 28, 14, C_MUT);
                    if (ui_button((Rectangle){150, sh - 34, 142, 24}, "Re-préparer", false)) pending_prepare = cur_site;
                }

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

            /* menu déroulant des points : dessiné en dernier pour passer par-dessus */
            const char *dd_items[MAX_PTS + 1];
            dd_items[0] = "Zone initiale";
            for (int i = 0; i < npts; i++) dd_items[i + 1] = pts[i].name;
            int chosen = ui_dropdown(ddr, dd_items, npts + 1, cur_site + 1);
            if (chosen >= 0 && chosen - 1 != cur_site) { pending_load = chosen - 1; zlev = 1; }
        }
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
