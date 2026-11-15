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
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif
/* Compression des résultats d'analyse (sdefl / sinfl, domaine public). Les noms sont préfixés : raylib embarque les mêmes fonctions. */
#define sdeflate sln_sdeflate
#define zsdeflate sln_zsdeflate
#define sdefl_bound sln_sdefl_bound
#define sinflate sln_sinflate
#define zsinflate sln_zsinflate
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define SDEFL_IMPLEMENTATION
#include "sdefl.h"
#define SINFL_IMPLEMENTATION
#include "sinfl.h"
#pragma GCC diagnostic pop

/* ===================== Langues et unités de mesure ===================== */
/* Les textes d'origine (français) servent de clés. Une langue = un fichier lang/xx.txt : « texte français <TAB> traduction »,
 * une ligne par texte. Aucun paquet à installer : ajouter une langue = ajouter un fichier. Les textes sans traduction restent en français.
 * TextFormat, DrawText et MeasureText sont redirigés : tout texte affiché passe par le dictionnaire. */
typedef struct { const char *key; const char *val; } TrEntry;
static TrEntry *g_tr = NULL;
static int g_ntr = 0;
static char *g_tr_buf = NULL;
static char g_lang[16] = "fr";
static int g_units = 0;                      /* 0 = métrique (m, km), 1 = impérial (ft, mi) */
static float g_ui_scale = 0.0f;             /* échelle de l'interface choisie : 0 = automatique */
static float g_ui_s = 1.0f;                 /* échelle réellement appliquée (limitée par la taille de l'écran et de la fenêtre) */
static int g_moon_fs = 0; // 0 = mode normal avec panneau latéral, 1 = vue lune en plein écran

static int tr_cmp(const void *a, const void *b) { return strcmp(((const TrEntry *)a)->key, ((const TrEntry *)b)->key); }

static const char *sln_T(const char *s) {
    if (!g_ntr || !s || !s[0]) return s;
    TrEntry k = {s, NULL};
    const TrEntry *r = (const TrEntry *)bsearch(&k, g_tr, (size_t)g_ntr, sizeof(TrEntry), tr_cmp);
    return r ? r->val : s;
}

static void tr_unload(void) {
    free(g_tr); free(g_tr_buf);
    g_tr = NULL; g_tr_buf = NULL; g_ntr = 0;
}

/* Charge lang/<code>.txt (le français est la langue d'origine : rien à charger). Renvoie 1 si la langue est active. */
static int tr_load(const char *code) {
    tr_unload();
    snprintf(g_lang, sizeof g_lang, "%s", code);
    if (strcmp(code, "fr") == 0) return 1;
    char path[96];
    snprintf(path, sizeof path, "lang/%s.txt", code);
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(g_lang, sizeof g_lang, "fr"); return 0; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_tr_buf = (char *)malloc((size_t)sz + 2);
    if (!g_tr_buf) { fclose(f); snprintf(g_lang, sizeof g_lang, "fr"); return 0; }
    size_t rd = fread(g_tr_buf, 1, (size_t)sz, f);
    fclose(f);
    g_tr_buf[rd] = '\n'; g_tr_buf[rd + 1] = 0;
    char *p = g_tr_buf;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;      /* BOM */
    int nlines = 0;
    for (char *q = p; *q; q++) if (*q == '\n') nlines++;
    g_tr = (TrEntry *)malloc((size_t)(nlines + 1) * sizeof(TrEntry));
    if (!g_tr) { tr_unload(); snprintf(g_lang, sizeof g_lang, "fr"); return 0; }
    while (*p) {
        char *eol = strchr(p, '\n');
        if (!eol) break;
        *eol = 0;
        if (eol > p && eol[-1] == '\r') eol[-1] = 0;
        char *tab = strchr(p, '\t');
        if (tab && tab[1]) {                      /* les lignes sans tabulation (#name=...) sont des commentaires */ *tab = 0; g_tr[g_ntr].key = p; g_tr[g_ntr].val = tab + 1; g_ntr++; }
        p = eol + 1;
    }
    qsort(g_tr, (size_t)g_ntr, sizeof(TrEntry), tr_cmp);
    return 1;
}

static const char *sln_textformat(const char *fmt, ...) {
    static char buf[8][1024];
    static int idx = 0;
    char *cur = buf[idx];
    idx = (idx + 1) & 7;
    va_list args;
    va_start(args, fmt);
    vsnprintf(cur, 1024, sln_T(fmt), args);
    va_end(args);
    return cur;
}
/* ----- police Unicode (russe, japonais...) : si lang/NotoSansCJKjp-subset.otf existe, elle sert pour tous les textes.
 * Les caractères chargés = latin + tous ceux des fichiers de langue (dossier lang) (ajouter une langue ne demande aucune autre étape). ----- */
#define LANG_FONT "lang/NotoSansCJKjp-subset.otf"
static Font g_font;
static int g_font_on = 0, g_font_failed = 0, g_font_px = 0;
static int *g_cps = NULL, g_ncps = 0;             /* caractères à charger (latin + fichiers de langue) */

static void cp_push(int **cps, int *n, int *cap, int cp) {
    if (*n >= *cap) { *cap *= 2; *cps = (int *)realloc(*cps, (size_t)*cap * sizeof(int)); }
    (*cps)[(*n)++] = cp;
}
static int cp_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void font_update(void) {
    if (g_font_failed || !IsWindowReady()) return;
    int want = (int)(18.0f * g_ui_s + 0.5f);          /* l'atlas suit l'échelle de l'interface : texte net à toutes les tailles */
    if (want < 16) want = 16;
    if (want > 64) want = 64;
    if (want == g_font_px) return;
    if (!FileExists(LANG_FONT)) { g_font_failed = 1; return; }
    if (!g_cps) {
        int cap = 4096, n = 0;
        int *cps = (int *)malloc((size_t)cap * sizeof(int));
        if (!cps) { g_font_failed = 1; return; }
        for (int c = 32; c <= 0x17F; c++) if (c < 0x7F || c >= 0xA0) cp_push(&cps, &n, &cap, c);
        for (int c = 0x2010; c <= 0x2027; c++) cp_push(&cps, &n, &cap, c);
        for (int c = 0x2190; c <= 0x2193; c++) cp_push(&cps, &n, &cap, c);
        cp_push(&cps, &n, &cap, 0x2212);
        if (DirectoryExists("lang")) {
            FilePathList fl = LoadDirectoryFilesEx("lang", ".txt", false);
            for (unsigned int i = 0; i < fl.count; i++) {
                char *txt = LoadFileText(fl.paths[i]);
                if (!txt) continue;
                for (const char *q = txt; *q;) {
                    int sz = 1;
                    int cp = GetCodepointNext(q, &sz);
                    if (cp >= 32 && cp != 0xFEFF && cp != 0x7F) cp_push(&cps, &n, &cap, cp);
                    q += sz > 0 ? sz : 1;
                }
                UnloadFileText(txt);
            }
            UnloadDirectoryFiles(fl);
        }
        qsort(cps, (size_t)n, sizeof(int), cp_cmp);
        int u = 0;
        for (int i = 0; i < n; i++) if (i == 0 || cps[i] != cps[i - 1]) cps[u++] = cps[i];
        g_cps = cps; g_ncps = u;
    }
    if (g_font_on) { UnloadFont(g_font); g_font_on = 0; }
    g_font = LoadFontEx(LANG_FONT, want, g_cps, g_ncps);
    if (g_font.texture.id == GetFontDefault().texture.id) { g_font_failed = 1; return; }      /* chargement échoué : police par défaut */
    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
    g_font_on = 1;
    g_font_px = want;
}

static void sln_draw_text(const char *t, int x, int y, int size, Color c) {
    t = sln_T(t);
    font_update();
    if (g_font_on) DrawTextEx(g_font, t, (Vector2){(float)x, (float)y}, (float)size, size / 16.0f, c);
    else DrawText(t, x, y, size, c);
}
static int sln_measure_text(const char *t, int size) {
    t = sln_T(t);
    font_update();
    if (g_font_on) return (int)(MeasureTextEx(g_font, t, (float)size, size / 16.0f).x + 0.5f);
    return MeasureText(t, size);
}
#define TR(s) sln_T(s)
#define TextFormat sln_textformat
#define DrawText sln_draw_text
#define MeasureText sln_measure_text

/* ----- affichage des longueurs, altitudes et surfaces selon l'unité choisie ----- */
static const char *fmt_dist_m(double m) {            /* distance quelconque : m ou km / ft ou mi */
    if (g_units == 0) return m < 1000.0 ? TextFormat("%.0f m", m) : TextFormat("%.1f km", m / 1000.0);
    double ft = m * 3.280840;
    return ft < 5280.0 ? TextFormat("%.0f ft", ft) : TextFormat("%.1f mi", m / 1609.344);
}
static const char *fmt_dist_km(double km) { return fmt_dist_m(km * 1000.0); }
static const char *fmt_alt_m(double m) {             /* altitude, rugosité : toujours m / ft */
    return g_units == 0 ? TextFormat("%.0f m", m) : TextFormat("%.0f ft", m * 3.280840);
}
static const char *fmt_area_km2(double km2) {
    return g_units == 0 ? TextFormat("%.0f km²", km2) : TextFormat("%.0f mi²", km2 / 2.589988);
}
static const char *fmt_scale_label(double m) {       /* étiquette « ronde » de la barre d'échelle */
    if (g_units == 0) return m >= 1000.0 ? TextFormat("%g km", m / 1000.0) : TextFormat("%.0f m", m);
    double ft = m * 3.280840;
    return ft < 5280.0 * 0.45 ? TextFormat("%.0f ft", ft) : TextFormat("%g mi", m / 1609.344);
}

/* ----- préférences enregistrées dans data/settings.txt ----- */
static void load_settings(void) {
    FILE *f = fopen("data/settings.txt", "r");
    if (!f) return;
    char line[96];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "lang=", 5) == 0) tr_load(line + 5);
        else if (strncmp(line, "units=", 6) == 0) g_units = strcmp(line + 6, "imperial") == 0;
        else if (strncmp(line, "scale=", 6) == 0) { g_ui_scale = strcmp(line + 6, "auto") == 0 ? 0.0f : (float)atof(line + 6); if (g_ui_scale != 0.0f && (g_ui_scale < 0.75f || g_ui_scale > 4.0f)) g_ui_scale = 0.0f; }
    }
    fclose(f);
}
static void save_settings(void) {
    MKDIR("data");
    FILE *f = fopen("data/settings.txt", "w");
    if (!f) return;
    fprintf(f, "lang=%s\nunits=%s\n", g_lang, g_units ? "imperial" : "metric");
    if (g_ui_scale > 0.0f) fprintf(f, "scale=%.2f\n", g_ui_scale); else fprintf(f, "scale=auto\n");
    fclose(f);
}

/* langues disponibles : le français + chaque fichier lang/xx.txt (la ligne « #name=English » donne le nom affiché) */
static struct { char code[16]; char name[40]; } g_langs[10];
static int g_nlangs = 0;
static void lang_scan(void) {
    snprintf(g_langs[0].code, sizeof g_langs[0].code, "fr");
    snprintf(g_langs[0].name, sizeof g_langs[0].name, "Français");
    g_nlangs = 1;
    if (!DirectoryExists("lang")) return;
    FilePathList fl = LoadDirectoryFilesEx("lang", ".txt", false);
    for (unsigned int i = 0; i < fl.count && g_nlangs < 10; i++) {
        const char *code = GetFileNameWithoutExt(fl.paths[i]);
        if (code[0] == '_' || strcmp(code, "fr") == 0) continue;
        snprintf(g_langs[g_nlangs].code, sizeof g_langs[g_nlangs].code, "%s", code);
        snprintf(g_langs[g_nlangs].name, sizeof g_langs[g_nlangs].name, "%s", code);
        FILE *f = fopen(fl.paths[i], "rb");
        if (f) {
            char line[96];
            for (int k = 0; k < 4 && fgets(line, sizeof line, f); k++) {
                char *nm = strstr(line, "#name=");
                if (nm) { nm += 6; nm[strcspn(nm, "\r\n")] = 0; snprintf(g_langs[g_nlangs].name, sizeof g_langs[g_nlangs].name, "%s", nm); break; }
            }
            fclose(f);
        }
        g_nlangs++;
    }
    UnloadDirectoryFiles(fl);
}

/* Utilitaires Windows (src/winproc.c) : lancement sans fenêtre de console, ouverture de dossiers */
int sln_run_hidden(const char *cmdline);
void sln_open(const char *path);
void sln_hide_console(void);

/* ===================== Paramètres ===================== */
#define GW 128                  /* largeur de la grille (cellules) */
#define GH 128                  /* hauteur de la grille */
static float cell_m = 200.0f;   /* taille d'une cellule (m), lue dans data/input/dem.txt si présent */
#define N_STEPS 240             /* nombre de pas de temps */
#define STEP_H 3.0f             /* durée d'un pas (h) -> 240 x 3 h = 30 jours */
#define NCELL (GW * GH)
#define LUNAR_MONTH_H (29.53 * 24.0)
#define EARTH_PERIOD_H (27.32 * 24.0)

#define SELENE_VERSION "1.0"
#define MENU_H 28                /* hauteur de la barre de menus */
#define TOP_H MENU_H             /* hauteur totale au-dessus du contenu (barre de menus seule) */
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

/* ===================== Classement des sites (année complète) ===================== */
#define NY 1461                  /* une année, pas de 6 h */
#define YSTEP_H 6.0
#define NAZ 180                  /* profil d'horizon : secteurs de 2 degrés */
#define SUN_RADIUS_DEG 0.27
#define EARTH_RADIUS_KM 6371.0
#define EPHEM_START "2026-12-01"   /* même date de départ pour tous les sites : indispensable pour les comparer */

static float horiz[(size_t)NCELL * NAZ];                 /* hauteur du relief (deg) par cellule et par direction */
static uint8_t ysun[(size_t)NCELL * NY];                 /* part du Soleil visible (0..255), par cellule puis par pas de temps */
static uint8_t yearth[(size_t)NCELL * NY];               /* part de la Terre visible (0..255) */
static int8_t ymarg[(size_t)NCELL * NY];                 /* marge angulaire de la Terre au-dessus du relief (0,1 deg) */
static double ysun_v[NY][3], yearth_v[NY][3];
static uint8_t is_psr[NCELL];                            /* 1 = jamais éclairée sur l'année (ombre permanente) */
static float psr_dist[NCELL];                            /* distance à l'ombre permanente la plus proche (m) */
static int year_loaded = 0, year_ready = 0, psr_count = 0;

static int load_year(const char *dir) {
    char p[340];
    snprintf(p, sizeof p, "%s/ephem_year.csv", dir);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char line[256];
    int k = 0;
    while (k < NY && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        double t, a, b, c, d, e, g;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &t, &a, &b, &c, &d, &e, &g) == 7) {
            ysun_v[k][0] = a; ysun_v[k][1] = b; ysun_v[k][2] = c;
            yearth_v[k][0] = d; yearth_v[k][1] = e; yearth_v[k][2] = g;
            k++;
        }
    }
    fclose(f);
    return k == NY;
}

/* Profil d'horizon de chaque cellule : angle maximal du relief dans chaque direction (courbure incluse, plancher = 0) */
static void compute_horizon(void) {
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int k = y * GW + x;
            float h0 = dem[k];
            for (int a = 0; a < NAZ; a++) {
                double az = a * (2.0 * PI / NAZ), dx = sin(az), dy = -cos(az), best = 0.0;
                for (int d = 1;; d++) {
                    double s = d * (double)cell_m, drop = s * s / (2.0 * LUNAR_R_M);
                    if ((dem_max - h0) - drop <= s * best) break;
                    int ix = (int)lround(x + dx * d), iy = (int)lround(y + dy * d);
                    if (ix < 0 || ix >= GW || iy < 0 || iy >= GH) break;
                    double t = (dem[iy * GW + ix] - h0 - drop) / s;
                    if (t > best) best = t;
                }
                horiz[(size_t)k * NAZ + a] = (float)(atan(best) * 180.0 / PI);
            }
        }
}

static double hor_at(int k, double az_deg) {
    double a = az_deg / (360.0 / NAZ);
    a = fmod(a, NAZ);
    if (a < 0) a += NAZ;
    int i0 = (int)a, i1 = (i0 + 1) % NAZ;
    double w = a - i0;
    const float *h = &horiz[(size_t)k * NAZ];
    return h[i0] * (1.0 - w) + h[i1] * w;
}

static uint8_t frac_u8(double margin, double radius) {     /* part visible d'un disque d'un rayon donné, selon sa marge au-dessus du relief */
    double f = (margin + radius) / (2.0 * radius);
    if (f <= 0) return 0;
    if (f >= 1) return 255;
    return (uint8_t)(f * 255.0 + 0.5);
}

/* Distance de chaque case à l'ombre permanente la plus proche (transformée de distance en 2 passes) */
static void compute_psr_dist(void) {
    psr_count = 0;
    for (int k = 0; k < NCELL; k++) { psr_dist[k] = is_psr[k] ? 0.0f : 1e9f; psr_count += is_psr[k]; }
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int k = y * GW + x;
            if (x > 0 && psr_dist[k - 1] + 1.0f < psr_dist[k]) psr_dist[k] = psr_dist[k - 1] + 1.0f;
            if (y > 0) {
                if (psr_dist[k - GW] + 1.0f < psr_dist[k]) psr_dist[k] = psr_dist[k - GW] + 1.0f;
                if (x > 0 && psr_dist[k - GW - 1] + 1.4142f < psr_dist[k]) psr_dist[k] = psr_dist[k - GW - 1] + 1.4142f;
                if (x < GW - 1 && psr_dist[k - GW + 1] + 1.4142f < psr_dist[k]) psr_dist[k] = psr_dist[k - GW + 1] + 1.4142f;
            }
        }
    for (int y = GH - 1; y >= 0; y--)
        for (int x = GW - 1; x >= 0; x--) {
            int k = y * GW + x;
            if (x < GW - 1 && psr_dist[k + 1] + 1.0f < psr_dist[k]) psr_dist[k] = psr_dist[k + 1] + 1.0f;
            if (y < GH - 1) {
                if (psr_dist[k + GW] + 1.0f < psr_dist[k]) psr_dist[k] = psr_dist[k + GW] + 1.0f;
                if (x < GW - 1 && psr_dist[k + GW + 1] + 1.4142f < psr_dist[k]) psr_dist[k] = psr_dist[k + GW + 1] + 1.4142f;
                if (x > 0 && psr_dist[k + GW - 1] + 1.4142f < psr_dist[k]) psr_dist[k] = psr_dist[k + GW - 1] + 1.4142f;
            }
        }
    for (int k = 0; k < NCELL; k++) if (psr_dist[k] < 1e8f) psr_dist[k] *= cell_m;
}

/* Visibilité du Soleil et de la Terre, cellule par cellule, sur l'année entière (disque solaire et terrestre pris en compte) */
static void compute_year(void) {
    for (int k = 0; k < NCELL; k++) {
        const float *f = &frame[9 * k];
        uint8_t *us = &ysun[(size_t)k * NY], *ue = &yearth[(size_t)k * NY];
        int8_t *um = &ymarg[(size_t)k * NY];
        int any = 0;
        for (int t = 0; t < NY; t++) {
            double az, el, m;
            const double *sv = ysun_v[t];
            uint8_t fs = 0;
            if (sv[0] * f[0] + sv[1] * f[1] + sv[2] * f[2] > -0.0209) {     /* au-dessus de -1,2 deg */
                cell_azel(k, sv, &az, &el);
                fs = frac_u8(el - hor_at(k, az), SUN_RADIUS_DEG);
            }
            us[t] = fs;
            any |= fs;
            double v[3] = {yearth_v[t][0] - LUNAR_R_KM * f[0], yearth_v[t][1] - LUNAR_R_KM * f[1], yearth_v[t][2] - LUNAR_R_KM * f[2]};
            double len = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if ((v[0] * f[0] + v[1] * f[1] + v[2] * f[2]) / len > -0.0349) {  /* au-dessus de -2 deg */
                cell_azel(k, v, &az, &el);
                double rE = asin(EARTH_RADIUS_KM / len) * 180.0 / PI;
                m = el - hor_at(k, az);
                ue[t] = frac_u8(m, rE);
                double q = m * 10.0;
                um[t] = (int8_t)(q > 127 ? 127 : (q < -127 ? -127 : q));
            } else { ue[t] = 0; um[t] = -127; }
        }
        is_psr[k] = !any;
    }
    compute_psr_dist();
    year_ready = 1;
}

/* ----- enregistrement des résultats d'analyse (évite de refaire les quelques secondes de calcul) ----- */
#define ANALYSIS_VERSION 3         /* à incrémenter si l'algorithme change : l'ancien fichier est alors ignoré */

static uint64_t analysis_hash(void) {          /* empreinte des entrées : si le site est re-préparé, le fichier est invalidé */
    uint64_t h = 1469598103934665603ULL;
#define HB(ptr, n) { const unsigned char *q_ = (const unsigned char *)(ptr); for (size_t i_ = 0; i_ < (n); i_++) { h ^= q_[i_]; h *= 1099511628211ULL; } }
    HB(&cell_m, sizeof cell_m);
    HB(dem, sizeof dem);
    HB(frame, sizeof frame);
    HB(ysun_v, sizeof ysun_v);
    HB(yearth_v, sizeof yearth_v);
#undef HB
    return h;
}

static int save_analysis(const char *dir) {
    char p[340];
    snprintf(p, sizeof p, "%s/analysis.bin", dir);
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    uint32_t head[5] = {0x314E4C53u, ANALYSIS_VERSION, GW, GH, NY};
    uint64_t h = analysis_hash();
    int ok = fwrite(head, sizeof head, 1, f) == 1 && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(is_psr, 1, sizeof is_psr, f) == sizeof is_psr;
    const void *src[3] = {ysun, yearth, ymarg};
    int n = (int)((size_t)NCELL * NY);
    unsigned char *buf = (unsigned char *)malloc((size_t)sdefl_bound(n));
    static struct sdefl sd;
    ok = ok && buf;
    for (int i = 0; ok && i < 3; i++) {
        memset(&sd, 0, sizeof sd);
        int clen = sdeflate(&sd, buf, src[i], n, 5);
        uint32_t cl = (uint32_t)clen;
        ok = fwrite(&cl, 4, 1, f) == 1 && fwrite(buf, 1, (size_t)clen, f) == (size_t)clen;
    }
    free(buf);
    fclose(f);
    if (!ok) remove(p);
    return ok;
}

static int load_analysis(const char *dir) {
    char p[340];
    snprintf(p, sizeof p, "%s/analysis.bin", dir);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    uint32_t head[5];
    uint64_t h = 0;
    int ok = fread(head, sizeof head, 1, f) == 1 && fread(&h, sizeof h, 1, f) == 1;
    ok = ok && head[0] == 0x314E4C53u && head[1] == ANALYSIS_VERSION && head[2] == GW && head[3] == GH && head[4] == NY && h == analysis_hash();
    ok = ok && fread(is_psr, 1, sizeof is_psr, f) == sizeof is_psr;
    void *dst[3] = {ysun, yearth, ymarg};
    int n = (int)((size_t)NCELL * NY);
    unsigned char *buf = ok ? (unsigned char *)malloc((size_t)sdefl_bound(n)) : NULL;
    ok = ok && buf;
    for (int i = 0; ok && i < 3; i++) {
        uint32_t cl = 0;
        ok = fread(&cl, 4, 1, f) == 1 && cl <= (uint32_t)sdefl_bound(n) && fread(buf, 1, cl, f) == cl && sinflate(dst[i], n, buf, (int)cl) == n;
    }
    free(buf);
    fclose(f);
    if (ok) { compute_psr_dist(); year_ready = 1; }
    return ok;
}

/* ----- paramètres et évaluation d'une cellule ----- */
typedef struct {
    float w[5];                       /* poids : énergie, communication, thermique, terrain, science */
    float smax_deg, sigma_max_m, night_max_h, ellipse_m, d0_m;
    int relay;                        /* 1 = Terre non exigée (un relais assure la liaison) */
    int s0, len;                      /* début et durée de la mission (pas de 6 h) */
} Params;

typedef struct {
    float sub[5];
    float sun_frac, com_frac, night_h, margin_deg;
    int trans, valid;
    int fail;                         /* bits : 1 pente, 2 rugosité, 4 Terre jamais visible, 8 nuit trop longue, 16 non évaluable (bord) */
} CellEval;

typedef struct {
    int k;
    float score;
    CellEval e;
    float slope, rough;
    int best_s0;
    float best_score;
    float robust;                     /* part des perturbations de poids où le site reste dans le top (-1 = non calculé) */
    float lat, lon;                   /* position sur la Lune (deg) */
} Cand;

static void cell_latlon(int k, float *lat, float *lon) {
    const float *f = &frame[9 * k];
    double z = f[2] > 1.0f ? 1.0 : (f[2] < -1.0f ? -1.0 : f[2]);
    *lat = (float)(asin(z) * 180.0 / PI);
    *lon = (float)(atan2(f[1], f[0]) * 180.0 / PI);
}

static float ter_slope[NCELL], ter_rough[NCELL];
static uint8_t ter_ok[NCELL];
static float ter_ellipse = -1.0f;
static float cell_score[NCELL];       /* -1 = hors zone ou éliminé, sinon note 0..100 */
static float cell_pen[NCELL];         /* départage à note égale : petit malus proportionnel à la distance au centre */
static float cell_raw[NCELL];         /* note qu'aurait la cellule sans les filtres (-1 = hors zone) : sert à l'affichage des cases éliminées */
static int st_total = 0, st_eval = 0, st_valid = 0, st_fail[4];   /* bilan des filtres sur la zone de recherche */
static CellEval cell_eval[NCELL];
static uint8_t in_region[NCELL];

/* Pente la plus forte et rugosité (écart-type au plan moyen) dans l'ellipse d'atterrissage de chaque cellule */
static void ensure_terrain(float ellipse_m) {
    if (ter_ellipse == ellipse_m) return;
    ter_ellipse = ellipse_m;
    int kw = (int)lround(ellipse_m / cell_m);
    if (kw < 0) kw = 0;
    int kr = kw < 1 ? 1 : kw;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int k = y * GW + x;
            if (x < kr || y < kr || x >= GW - kr || y >= GH - kr) { ter_ok[k] = 0; continue; }
            double sw = slope_deg[k];
            for (int dy = -kw; dy <= kw; dy++)
                for (int dx = -kw; dx <= kw; dx++)
                    if (dx * dx + dy * dy <= kw * kw && slope_deg[(y + dy) * GW + x + dx] > sw) sw = slope_deg[(y + dy) * GW + x + dx];
            double n = 0, sh = 0, sxh = 0, syh = 0, sh2 = 0, sxx = 0;
            for (int dy = -kr; dy <= kr; dy++)
                for (int dx = -kr; dx <= kr; dx++) {
                    double h = dem[(y + dy) * GW + x + dx];
                    n++; sh += h; sxh += dx * h; syh += dy * h; sh2 += h * h; sxx += dx * dx;
                }
            double c = sh / n, a = sxh / sxx, b = syh / sxx;
            double var = (sh2 - n * c * c - a * a * sxx - b * b * sxx) / n;
            ter_slope[k] = (float)sw;
            ter_rough[k] = (float)sqrt(var > 0 ? var : 0);
            ter_ok[k] = 1;
        }
}

static float aggregate(const float sub[5], const float w[5]) {   /* moyenne géométrique pondérée : une faiblesse pénalise fortement */
    double sw = 0, acc = 0;
    for (int i = 0; i < 5; i++) if (w[i] > 0) { acc += w[i] * log(sub[i] + 0.02); sw += w[i]; }
    if (sw <= 0) return 0.0f;
    double g = (exp(acc / sw) - 0.02);
    return (float)(100.0 * (g < 0 ? 0 : (g > 1 ? 1 : g)));
}

static double clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/* Évalue la cellule (x, y) sur la fenêtre de mission P->s0 .. P->s0 + P->len. Renvoie 1 si elle passe les filtres éliminatoires. */
static int eval_cell(int x, int y, const Params *P, CellEval *E) {
    int k = y * GW + x;
    E->valid = 0;
    E->fail = 0;
    if (!ter_ok[k] || P->s0 < 0 || P->s0 + P->len > NY) { E->fail = 16; return 0; }
    const uint8_t *us = &ysun[(size_t)k * NY + P->s0], *ue = &yearth[(size_t)k * NY + P->s0];
    const int8_t *um = &ymarg[(size_t)k * NY + P->s0];
    double sun = 0, com = 0, mar = 0;
    int nmar = 0, run = 0, best = 0, trans = 0, prev = -1;
    for (int i = 0; i < P->len; i++) {
        sun += us[i];
        com += ue[i];
        int lit = us[i] >= 128;
        if (!lit) { if (++run > best) best = run; } else run = 0;
        if (prev >= 0 && lit != prev) trans++;
        prev = lit;
        if (ue[i] >= 128) { mar += um[i] * 0.1; nmar++; }
    }
    E->sun_frac = (float)(sun / (255.0 * P->len));
    E->com_frac = (float)(com / (255.0 * P->len));
    E->night_h = (float)(best * YSTEP_H);
    E->margin_deg = nmar ? (float)(mar / nmar) : 0.0f;
    E->trans = trans;
    double days = P->len * YSTEP_H / 24.0;
    double sl = ter_slope[k], ro = ter_rough[k];
    double st = 1.0 - (sl / P->smax_deg) * (sl / P->smax_deg), sr = 1.0 - (ro / P->sigma_max_m) * (ro / P->sigma_max_m);
    E->sub[0] = (float)clamp01(0.7 * E->sun_frac + 0.3 * (1.0 - E->night_h / P->night_max_h));
    E->sub[1] = (float)clamp01(0.8 * E->com_frac + 0.2 * clamp01(E->margin_deg / 5.0));
    E->sub[2] = (float)clamp01(1.0 - trans / (2.0 * days));          /* indicateur de substitution : cycles lumière/ombre, PAS une température */
    E->sub[3] = (float)clamp01(0.5 * clamp01(st) + 0.5 * clamp01(sr));
    E->sub[4] = psr_dist[k] < 1e8f ? (float)exp(-psr_dist[k] / P->d0_m) : 0.0f;
    if (sl > P->smax_deg) E->fail |= 1;
    if (ro > P->sigma_max_m) E->fail |= 2;
    if (!P->relay && E->com_frac <= 0.0f) E->fail |= 4;
    if (E->night_h > P->night_max_h) E->fail |= 8;
    E->valid = (E->fail == 0);
    return E->valid;
}

/* Suppression des non-maximums : meilleur site, puis exclusion d'un disque autour de lui, et on recommence */
static int nms(const float *score, const uint8_t *ok, double excl_m, int nmax, int *outk) {
    static uint8_t taken[NCELL];
    memset(taken, 0, sizeof taken);
    int n = 0;
    while (n < nmax) {
        int bk = -1;
        float bs = 0.0f;
        for (int k = 0; k < NCELL; k++) if (ok[k] && !taken[k] && score[k] > bs) { bs = score[k]; bk = k; }
        if (bk < 0) break;
        outk[n++] = bk;
        int bx = bk % GW, by = bk / GW;
        for (int k = 0; k < NCELL; k++) {
            double dx = (k % GW - bx) * (double)cell_m, dy = (k / GW - by) * (double)cell_m;
            if (dx * dx + dy * dy <= excl_m * excl_m) taken[k] = 1;
        }
    }
    return n;
}

static void best_start_for_cell(const Params *P, int k, int *bs0, float *bscore) {
    Params q = *P;
    *bs0 = P->s0; *bscore = -1.0f;
    for (int s0 = 0; s0 + P->len <= NY; s0 += 4) {          /* une date de départ par jour */
        q.s0 = s0;
        CellEval e;
        if (eval_cell(k % GW, k / GW, &q, &e)) {
            float sc = aggregate(e.sub, P->w);
            if (sc > *bscore) { *bscore = sc; *bs0 = s0; }
        }
    }
}

/* Meilleure date de départ pour toute la zone de recherche : celle qui donne le meilleur site */
static int best_start_region(const Params *P, int cx, int cy, double radius_m) {
    Params q = *P;
    int bs0 = -1;
    float best = -1.0f;
    for (int s0 = 0; s0 + P->len <= NY; s0 += 4) {
        q.s0 = s0;
        for (int y = 0; y < GH; y++)
            for (int x = 0; x < GW; x++) {
                double dx = (x - cx) * (double)cell_m, dy = (y - cy) * (double)cell_m;
                if (dx * dx + dy * dy > radius_m * radius_m) continue;
                CellEval e;
                if (eval_cell(x, y, &q, &e)) {
                    float sc = aggregate(e.sub, P->w);
                    if (sc > best) { best = sc; bs0 = s0; }
                }
            }
    }
    return bs0;
}

static unsigned int rk_seed = 12345u;
static float rk_rand(void) { rk_seed = rk_seed * 1664525u + 1013904223u; return (rk_seed >> 8) / 16777216.0f; }

/* Classe les sites autour de (cx, cy) dans un rayon donné. Renvoie le nombre de candidats (jusqu'à nmax). */
static int rank_region(const Params *P, int cx, int cy, double radius_m, double excl_m, int nmax, Cand *out, int with_robust) {
    static uint8_t okmask[NCELL];
    static int ks[8];
    ensure_terrain(P->ellipse_m);
    st_total = st_eval = st_valid = 0;
    for (int b = 0; b < 4; b++) st_fail[b] = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            int k = y * GW + x;
            double dx = (x - cx) * (double)cell_m, dy = (y - cy) * (double)cell_m;
            in_region[k] = dx * dx + dy * dy <= radius_m * radius_m;
            okmask[k] = 0;
            cell_score[k] = -1.0f;
            cell_raw[k] = -1.0f;
            if (!in_region[k]) continue;
            st_total++;
            int ok = eval_cell(x, y, P, &cell_eval[k]);
            if (cell_eval[k].fail & 16) continue;
            st_eval++;
            /* à note égale, on préfère la case la plus proche du centre choisi (écart <= 0,02 point, invisible à l'affichage) */
            cell_pen[k] = 0.02f * (float)(sqrt(dx * dx + dy * dy) / (radius_m > 1.0 ? radius_m : 1.0));
            cell_raw[k] = aggregate(cell_eval[k].sub, P->w) - cell_pen[k];
            if (cell_raw[k] < 0.0f) cell_raw[k] = 0.0f;
            for (int b = 0; b < 4; b++) if (cell_eval[k].fail & (1 << b)) st_fail[b]++;
            if (ok) { cell_score[k] = cell_raw[k]; okmask[k] = 1; st_valid++; }
        }
    int n = nms(cell_score, okmask, excl_m, nmax > 8 ? 8 : nmax, ks);
    int invalid_mode = 0;
    if (n == 0) {                                  /* aucun site valide : on classe quand même les meilleurs sites éliminés (note brute) */
        static uint8_t okraw[NCELL];
        for (int k = 0; k < NCELL; k++) okraw[k] = in_region[k] && cell_raw[k] >= 0.0f;
        n = nms(cell_raw, okraw, excl_m, nmax > 8 ? 8 : nmax, ks);
        invalid_mode = 1;
    }
    for (int i = 0; i < n; i++) {
        Cand *c = &out[i];
        c->k = ks[i]; c->score = invalid_mode ? cell_raw[ks[i]] : cell_score[ks[i]]; c->e = cell_eval[ks[i]];
        cell_latlon(ks[i], &c->lat, &c->lon);
        c->slope = ter_slope[ks[i]]; c->rough = ter_rough[ks[i]];
        best_start_for_cell(P, ks[i], &c->best_s0, &c->best_score);
        c->robust = -1.0f;
    }
    if (with_robust && n > 0 && !invalid_mode) {                                        /* stabilité : 30 jeux de poids perturbés de +-30 % */
        static float vs[NCELL];
        int hits[8] = {0};
        for (int v = 0; v < 30; v++) {
            float w[5];
            for (int i = 0; i < 5; i++) w[i] = P->w[i] * (0.7f + 0.6f * rk_rand());
            for (int k = 0; k < NCELL; k++) vs[k] = okmask[k] ? aggregate(cell_eval[k].sub, w) - cell_pen[k] : 0.0f;
            int vk[5];
            int vn = nms(vs, okmask, excl_m, nmax > 5 ? 5 : nmax, vk);
            for (int i = 0; i < n; i++)
                for (int j = 0; j < vn; j++) {
                    double dx = (ks[i] % GW - vk[j] % GW) * (double)cell_m, dy = (ks[i] / GW - vk[j] / GW) * (double)cell_m;
                    if (dx * dx + dy * dy <= excl_m * excl_m) { hits[i]++; break; }
                }
        }
        for (int i = 0; i < n; i++) out[i].robust = hits[i] / 30.0f;
    }
    return n;
}

/* ===================== Globe : éphémérides (luminosité) et saisie de coordonnées ===================== */
static double gsun_v[NY][3], gearth_v[NY][3];      /* direction du Soleil, vecteur Terre (km) : repère lunaire, ne dépend pas du site */
static int globe_eph_ok = 0;

static int load_globe_eph(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int k = 0;
    while (k < NY && fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        double t, a, b, c, d, e, g;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf", &t, &a, &b, &c, &d, &e, &g) == 7) {
            gsun_v[k][0] = a; gsun_v[k][1] = b; gsun_v[k][2] = c;
            gearth_v[k][0] = d; gearth_v[k][1] = e; gearth_v[k][2] = g;
            k++;
        }
    }
    fclose(f);
    return k == NY;
}

/* Lit une coordonnée saisie : « -89,9 », « 89.9 S », « 45 O », « 12.5° E »... Renvoie 1 si la saisie est valide. */
static int parse_coord(const char *txt, double *out) {
    char b[48];
    size_t n = 0;
    for (const char *p = txt; *p && n < sizeof b - 1; p++) {
        if (*p == ' ') continue;
        b[n++] = (*p == ',') ? '.' : *p;
    }
    b[n] = 0;
    while (n >= 2 && (unsigned char)b[n - 2] == 0xC2 && (unsigned char)b[n - 1] == 0xB0) { b[n - 2] = 0; n -= 2; }   /* signe ° */
    if (n == 0) return 0;
    double sign = 1.0;
    char last = b[n - 1];
    if ((last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z')) {
        char u = (char)(last >= 'a' ? last - 32 : last);
        if (u == 'S' || u == 'W' || u == 'O') sign = -1.0;                  /* O = Ouest */
        else if (u != 'N' && u != 'E') return 0;
        b[--n] = 0;
        while (n >= 2 && (unsigned char)b[n - 2] == 0xC2 && (unsigned char)b[n - 1] == 0xB0) { b[n - 2] = 0; n -= 2; }   /* « 12.5° E » */
        if (n == 0) return 0;
    }
    char *end;
    double v = strtod(b, &end);
    if (end == b || *end) return 0;
    if (sign < 0.0 && v > 0.0) v = -v;
    *out = v;
    return 1;
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
static float g_clip_top = 0.0f;       /* les widgets au-dessus de cette ordonnée ignorent la souris (panneau défilant) */

static bool ui_button(Rectangle r, const char *txt, bool on) {
    bool hot = !g_block && GetMousePosition().y >= g_clip_top && CheckCollisionPointRec(GetMousePosition(), r);
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
    if (!g_block && m.y >= g_clip_top && CheckCollisionPointRec(m, hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_active = v;
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
    DrawText(TextFormat("Point : %s", TR(items[sel])), (int)r.x + 10, (int)r.y + 6, 16, C_TXT);
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
        } else if (layer == 4) {                                  /* classement : vert = valide (plus clair = meilleure note), rouge = éliminé */
            if (year_ready && is_psr[k]) { r = 0.36f; g = 0.26f; b = 0.58f; }
            else if (year_ready && in_region[k]) {
                if (cell_score[k] >= 0.0f) {
                    float v = cell_score[k] / 100.0f;
                    r = 0.06f + 0.14f * v; g = 0.30f + 0.62f * v; b = 0.12f + 0.20f * v;
                } else {
                    float v = cell_raw[k] >= 0.0f ? cell_raw[k] / 100.0f : 0.0f;
                    r = 0.38f + 0.45f * v; g = 0.08f + 0.10f * v; b = 0.08f + 0.08f * v;
                }
            } else {
                float v = (dem[k] - dem_min) / (dem_max - dem_min);
                r = 0.10f + 0.28f * v; g = 0.12f + 0.28f * v; b = 0.16f + 0.26f * v;
            }
        } else {
            float v = Clamp(slope_deg[k] / 30.0f, 0.0f, 1.0f);
            r = 0.2f + 0.7f * v; g = 0.7f - 0.5f * v; b = 0.4f - 0.2f * v;
        }
        if (slope_deg[k] > smax && layer != 3 && layer != 4) { r = r * 0.5f + 0.45f; g = g * 0.5f + 0.10f; b = b * 0.5f + 0.08f; }
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

/* Vue réelle : la Lune comme une caméra la verrait. Soleil à sa vraie position, ombres portées du relief (carte de visibilité du
 * Soleil), régolithe gris avec un grain léger, très faible clair de Terre. Le Soleil rasant donne une scène naturellement sombre :
 * le réglage d'exposition joue le rôle du diaphragme (courbe douce, sans saturation brutale). */
static void recolor_real(int t, int ref_k, float expo) {
    double sa = sun_az[t], se = sun_el[t];
    if (have_frame) cell_azel(ref_k, sun_v[t], &sa, &se);
    Vector3 sd = azel_to_dir3d(sa, se);
    size_t off = (size_t)t * NCELL;
    for (int k = 0; k < NCELL; k++) {
        unsigned h = (unsigned)(k % GW) * 73856093u ^ (unsigned)(k / GW) * 19349663u;
        h = (h ^ (h >> 13)) * 1274126177u;
        float grain = 0.90f + 0.20f * (float)((h >> 8) & 0xFFFF) / 65535.0f;          /* +-10 % d'albédo d'une case à l'autre */
        Vector3 n = {mesh.normals[3 * k], mesh.normals[3 * k + 1], mesh.normals[3 * k + 2]};
        float diff = Vector3DotProduct(n, sd);
        if (diff < 0.0f) diff = 0.0f;
        float lit = illum[off + k] ? diff : 0.0f;                                     /* case à l'ombre du relief : pas de lumière directe */
        float v = (1.0f - expf(-expo * lit)) * 0.95f * grain + 0.012f;                /* + lumière diffuse très faible */
        float earth = earthv[off + k] ? 0.012f : 0.0f;                                /* clair de Terre */
        mesh.colors[4 * k] = (unsigned char)(Clamp(v, 0.0f, 1.0f) * 255.0f);
        mesh.colors[4 * k + 1] = (unsigned char)(Clamp(v * 0.98f + earth * 0.5f, 0.0f, 1.0f) * 255.0f);
        mesh.colors[4 * k + 2] = (unsigned char)(Clamp(v * 0.94f + earth, 0.0f, 1.0f) * 255.0f);
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
static float *g_mf = NULL;       /* directions des sommets dans le repère lunaire (x lon0, y est, z nord) : 3 float par sommet */
static float *g_exp = NULL;      /* valeur 0..1 par sommet pour les filtres de topographie */
static float *g_rough = NULL;    /* écart-type local des altitudes (m), fenêtre de 5 x 5 sommets */
static void globe_topo_stats(void);

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
    g_mf = (float *)MemAlloc(GVERT * 3 * sizeof(float));
    g_exp = (float *)MemAlloc(GVERT * sizeof(float));
    g_rough = (float *)MemAlloc(GVERT * sizeof(float));
    globe_topo_stats();
    for (int j = 0; j <= GLAT; j++)
        for (int i = 0; i <= GLON; i++) {
            int k = j * (GLON + 1) + i;
            float la_deg = 90.0f - 180.0f * j / GLAT, lo_deg = -180.0f + 360.0f * i / GLON;
            g_dir[k] = sph(la_deg, lo_deg);
            g_mf[3 * k] = cosf(la_deg * DEG2RAD) * cosf(lo_deg * DEG2RAD);
            g_mf[3 * k + 1] = cosf(la_deg * DEG2RAD) * sinf(lo_deg * DEG2RAD);
            g_mf[3 * k + 2] = sinf(la_deg * DEG2RAD);
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

/* ----- filtres du globe : dégradés de couleur ----- */
static const Color RAMP_LIGHT[2] = {{20, 22, 30, 255}, {255, 244, 220, 255}};
static const Color RAMP_TOPO[6] = {{22, 32, 120, 255}, {30, 112, 196, 255}, {62, 172, 124, 255}, {208, 204, 92, 255}, {154, 102, 62, 255}, {246, 246, 246, 255}};
static const Color RAMP_DIV[3] = {{40, 76, 190, 255}, {236, 236, 242, 255}, {204, 52, 42, 255}};
static const Color RAMP_ROUGH[4] = {{12, 18, 60, 255}, {96, 48, 140, 255}, {238, 110, 60, 255}, {255, 236, 150, 255}};

static Color ramp_color(const Color *st, int n, float t) {
    if (t <= 0.0f) return st[0];
    if (t >= 1.0f) return st[n - 1];
    float f = t * (n - 1);
    int i = (int)f;
    float u = f - i;
    return (Color){(unsigned char)(st[i].r + (st[i + 1].r - st[i].r) * u), (unsigned char)(st[i].g + (st[i + 1].g - st[i].g) * u),
                   (unsigned char)(st[i].b + (st[i + 1].b - st[i].b) * u), 255};
}

/* Couleur d'un filtre : 1 luminosité, 2 altitude, 3 écart à la moyenne, 4 rugosité (t entre 0 et 1) */
static Color filter_color(int filt, float t) {
    if (filt == 2) return ramp_color(RAMP_TOPO, 6, t);
    if (filt == 3) return ramp_color(RAMP_DIV, 3, t);
    if (filt == 4) return ramp_color(RAMP_ROUGH, 4, t);
    return ramp_color(RAMP_LIGHT, 2, t);
}

/* ----- statistiques de topographie du globe (moyenne pondérée par la surface, écart-type, rugosité locale) ----- */
static double g_topo_mean = 0, g_topo_std = 1, g_topo_lo = 0, g_topo_hi = 1, g_rough_hi = 1;
static float g_topo_min = 0, g_topo_max = 0;

static void globe_topo_stats(void) {
    double sw = 0, sm = 0;
    g_topo_min = g_topo_max = g_h[0];
    for (int j = 0; j <= GLAT; j++) {
        double w = cos((90.0 - 180.0 * j / GLAT) * PI / 180.0);
        if (w < 0.0) w = 0.0;
        for (int i = 0; i < GLON; i++) {
            float h = g_h[j * (GLON + 1) + i];
            sw += w; sm += w * h;
            if (h < g_topo_min) g_topo_min = h;
            if (h > g_topo_max) g_topo_max = h;
        }
    }
    g_topo_mean = sm / sw;
    double sv = 0;
    for (int j = 0; j <= GLAT; j++) {
        double w = cos((90.0 - 180.0 * j / GLAT) * PI / 180.0);
        if (w < 0.0) w = 0.0;
        for (int i = 0; i < GLON; i++) { double d = g_h[j * (GLON + 1) + i] - g_topo_mean; sv += w * d * d; }
    }
    g_topo_std = sqrt(sv / sw);
    if (g_topo_std < 1.0) g_topo_std = 1.0;
    g_topo_lo = fmax(g_topo_min, g_topo_mean - 3.0 * g_topo_std);          /* l'échelle des couleurs ignore les valeurs extrêmes */
    g_topo_hi = fmin(g_topo_max, g_topo_mean + 3.0 * g_topo_std);
    double rs = 0, rs2 = 0, rw = 0;
    for (int j = 0; j <= GLAT; j++) {
        double w = cos((90.0 - 180.0 * j / GLAT) * PI / 180.0);
        if (w < 0.0) w = 0.0;
        for (int i = 0; i <= GLON; i++) {
            double m1 = 0, m2 = 0;
            for (int dj = -2; dj <= 2; dj++) {
                int jj = j + dj < 0 ? 0 : (j + dj > GLAT ? GLAT : j + dj);
                for (int di = -2; di <= 2; di++) {
                    int ii = ((i + di) % GLON + GLON) % GLON;                    /* la longitude boucle */
                    double h = g_h[jj * (GLON + 1) + ii];
                    m1 += h; m2 += h * h;
                }
            }
            m1 /= 25.0; m2 /= 25.0;
            double sd = sqrt(m2 - m1 * m1 > 0.0 ? m2 - m1 * m1 : 0.0);
            g_rough[j * (GLON + 1) + i] = (float)sd;
            if (i < GLON) { rw += w; rs += w * sd; rs2 += w * sd * sd; }
        }
    }
    double rm = rs / rw, rv = rs2 / rw - rm * rm;
    g_rough_hi = rm + 2.0 * sqrt(rv > 0.0 ? rv : 0.0);
    if (g_rough_hi < 1.0) g_rough_hi = 1.0;
}

/* Remplit g_exp (0..1) pour le filtre de topographie choisi */
static void globe_fill_topo(int filt) {
    for (int k = 0; k < GVERT; k++) {
        double v;
        if (filt == 2) v = (g_h[k] - g_topo_lo) / (g_topo_hi - g_topo_lo);
        else if (filt == 3) v = ((g_h[k] - g_topo_mean) / g_topo_std + 3.0) / 6.0;
        else v = g_rough[k] / g_rough_hi;
        g_exp[k] = (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
    }
}

/* Luminosité : le Soleil éclaire la Lune comme à la date choisie (phase et terminateur, relief compris) */
static void globe_light_sun(int step) {
    const double *sv = gsun_v[step];
    Vector3 sdir = {(float)sv[0], (float)sv[2], (float)-sv[1]};            /* repère lunaire -> repère d'affichage */
    for (int k = 0; k < GVERT; k++) {
        float d = Vector3DotProduct(g_nrm[k], sdir);
        float v = 0.04f + 0.96f * Clamp(d * 1.25f, 0.0f, 1.0f);
        gmesh.colors[4 * k] = (unsigned char)(255.0f * v);
        gmesh.colors[4 * k + 1] = (unsigned char)(255.0f * v * 0.97f);
        gmesh.colors[4 * k + 2] = (unsigned char)(255.0f * v * 0.92f);
    }
    UpdateMeshBuffer(gmesh, 3, gmesh.colors, GVERT * 4, 0);
}

/* Topographie : dégradé de couleur selon g_exp (0..1), avec un léger ombrage du relief pour garder le volume */
static void globe_shade_gradient(int filt, Vector3 light) {
    for (int k = 0; k < GVERT; k++) {
        Color c = filter_color(filt, g_exp[k]);
        float d = Vector3DotProduct(g_nrm[k], light);
        float sh = 0.62f + 0.38f * (d > 0 ? d : 0);
        gmesh.colors[4 * k] = (unsigned char)(c.r * sh);
        gmesh.colors[4 * k + 1] = (unsigned char)(c.g * sh);
        gmesh.colors[4 * k + 2] = (unsigned char)(c.b * sh);
    }
    UpdateMeshBuffer(gmesh, 3, gmesh.colors, GVERT * 4, 0);
}

static void draw_gradient_legend(int x, int y, int w, int filt, const char *left, const char *right) {
    for (int i = 0; i < w; i++) DrawRectangle(x + i, y, 1, 10, filter_color(filt, (float)i / (float)(w - 1)));
    DrawText(left, x, y + 13, 12, C_MUT);
    DrawText(right, x + w - MeasureText(right, 12), y + 13, 12, C_MUT);
}

/* ----- points d'intérêt du globe (positions approximatives, au centre des formations) ----- */
typedef struct { const char *name; float lat, lon; int cat; } Poi;     /* cat : 0 missions, 1 reliefs, 2 pôle Sud */
static const Poi POIS[] = {
    {"Apollo 11", 0.674f, 23.473f, 0}, {"Apollo 12", -3.012f, -23.422f, 0}, {"Apollo 14", -3.645f, -17.471f, 0},
    {"Apollo 15", 26.132f, 3.633f, 0}, {"Apollo 16", -8.973f, 15.500f, 0}, {"Apollo 17", 20.191f, 30.772f, 0},
    {"Chang'e 3", 44.12f, -19.51f, 0}, {"Chang'e 4", -45.46f, 177.59f, 0}, {"Chang'e 5", 43.06f, -51.92f, 0},
    {"Chandrayaan-3", -69.37f, 32.32f, 0},
    {"Mer de la Tranquillité", 8.5f, 31.4f, 1}, {"Mer de la Sérénité", 28.0f, 17.5f, 1}, {"Mer des Crises", 17.0f, 59.1f, 1},
    {"Mer de la Fécondité", -7.8f, 51.3f, 1}, {"Mer des Pluies", 32.8f, -15.6f, 1}, {"Mer des Nuées", -21.3f, -16.6f, 1},
    {"Océan des Tempêtes", 18.4f, -57.4f, 1}, {"Mer du Froid", 56.0f, 1.4f, 1}, {"Mer des Humeurs", -24.4f, -38.6f, 1},
    {"Mer Orientale", -19.4f, -92.8f, 1}, {"Tycho", -43.31f, -11.36f, 1}, {"Copernic", 9.62f, -20.08f, 1},
    {"Aristarque", 23.7f, -47.4f, 1}, {"Kepler", 8.1f, -38.0f, 1}, {"Platon", 51.6f, -9.3f, 1}, {"Clavius", -58.8f, -14.4f, 1},
    {"Bassin Pôle Sud-Aitken", -53.0f, -169.0f, 1},
    {"Shackleton", -89.9f, 129.8f, 2}, {"Haworth", -87.5f, -5.0f, 2}, {"de Gerlache", -88.5f, -87.1f, 2}, {"Faustini", -87.3f, 77.0f, 2},
    {"Cabeus", -84.9f, -35.5f, 2}, {"Malapert", -84.9f, 12.9f, 2}, {"Nobile", -85.2f, 53.3f, 2}, {"Amundsen", -84.5f, 82.8f, 2},
};
#define NPOI ((int)(sizeof POIS / sizeof *POIS))
static const char *POI_CAT[3] = {"Missions", "Reliefs", "Pôle Sud"};
static const Color POI_COL[3] = {{242, 184, 75, 255}, {170, 205, 235, 255}, {190, 140, 255, 255}};

/* ===================== Points enregistrés et sites ===================== */
#define MAX_PTS 16

typedef struct { char id[16]; char name[32]; float lat, lon; } SavedPt;   /* id = nom du dossier (stable), name = nom affiché (modifiable) */
static SavedPt pts[MAX_PTS];
static int npts = 0, next_id = 1;
static char status_msg[256] = "";

static void save_points(void) {
    if (npts == 0) next_id = 1;                      /* plus aucun point : la numérotation repart de P1 */
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
    if (!FileExists(p)) return 0;
    if (idx >= 0) {            /* le numéro P1 peut être réutilisé : on vérifie que le dossier correspond bien à CE point */
        float la, lo;
        snprintf(p, sizeof p, "%s/dem.txt", dir);
        if (load_zone_center(p, &la, &lo) && (fabsf(la - pts[idx].lat) > 0.02f || fabsf(lo - pts[idx].lon) > 0.02f)) return 0;
    }
    return 1;
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
    year_loaded = have_frame && load_year(dir);
    year_ready = 0;
    ter_ellipse = -1.0f;
    compute_visibility();
}

/* Message de chargement affiché PAR-DESSUS l'interface (le calcul a lieu à l'image suivante) */
static char g_busy[100] = "";

static void draw_busy(int sw, int sh_total) {
    if (!g_busy[0]) return;
    DrawRectangle(0, MENU_H, sw, sh_total - MENU_H, (Color){0, 0, 0, 110});
    int tw = MeasureText(g_busy, 20) + 64;
    Rectangle b = {(sw - tw) / 2.0f, sh_total / 2.0f - 34, (float)tw, 68};
    DrawRectangleRec(b, C_PANEL);
    DrawRectangleLinesEx(b, 1, C_SUN);
    DrawText(g_busy, (int)b.x + 32, (int)b.y + 24, 20, C_TXT);
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

/* Lance l'outil de préparation pour ce site et ce niveau de zone (DEM + éphémérides). Bloque quelques secondes.
 * Priorité à tools/prepare_data.exe (autonome, sans Python) ; sinon tools/prepare_data.py via Python. */
static void run_prepare(int idx, int z) {
    char dir[300], args[400], cmd[1400];
    site_dir(idx, z, dir, sizeof dir);
    float lat = idx < 0 ? zone_lat : pts[idx].lat, lon = idx < 0 ? zone_lon : pts[idx].lon;
    snprintf(args, sizeof args, "--lat %.4f --lon %.4f --out %s --zoom %.2f --start %s", lat, lon, dir, zfac[z], EPHEM_START);
    int has_exe = FileExists("tools/prepare_data.exe"), has_py = FileExists("tools/prepare_data.py");
    if (!has_exe && !has_py) {
        snprintf(status_msg, sizeof status_msg, "%s", TR("Outil de préparation introuvable (voir Aide > Vérifier l'installation)."));
        return;
    }
    if (has_exe) snprintf(cmd, sizeof cmd, "tools\\prepare_data.exe %s > data\\prepare_log.txt 2>&1", args);
    else snprintf(cmd, sizeof cmd, "python tools/prepare_data.py %s > data\\prepare_log.txt 2>&1 || py tools/prepare_data.py %s > data\\prepare_log.txt 2>&1", args, args);
    sln_run_hidden(cmd);
    if (site_prepared(idx, z)) status_msg[0] = 0; else read_log_tail();
}

/* Boussole : indique où sont le pôle Sud (S) et le pôle Nord (N) par rapport à la vue.
 * Disque plein = pôle visible, anneau = caché derrière la Lune. */
static void draw_compass(Camera3D cam, Rectangle view, int vw, int vh) {
    Vector2 c0 = GetWorldToScreenEx((Vector3){0, 0, 0}, cam, vw, vh);
    Vector3 pole[2] = {{0, -GLOBE_R * 1.02f, 0}, {0, GLOBE_R * 1.02f, 0}};
    const char *lab[2] = {"S", "N"};
    Color col[2] = {C_SUN, C_MUT};
    Vector2 ctr = {view.x + view.width - 124, view.y + view.height - 70};      /* à gauche du bouton plein écran */
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
        Color lc = (i == 0) ? C_SUN : C_TXT;           /* pôle Sud en jaune, pôle Nord en blanc */
        if (front) {   /* repère directement sur la Lune */
            DrawCircleLines((int)(view.x + p.x), (int)p.y, 7, lc);
            DrawText(i == 0 ? "Pôle Sud" : "Pôle Nord", (int)(view.x + p.x) + 10, (int)p.y - 7, 14, lc);
        } else {       /* pôle derrière la Lune : étiquette estompée sur le bord, dans sa direction */
            float dcam = Vector3Length(cam.position);
            float rpx = (vh * 0.5f) / tanf(cam.fovy * 0.5f * DEG2RAD) * tanf(asinf(fminf(GLOBE_R / dcam, 0.999f)));
            Vector2 u = (len > 4.0f) ? (Vector2){d.x / len, d.y / len} : (Vector2){0.0f, i == 0 ? 1.0f : -1.0f};
            Vector2 e = {c0.x + u.x * rpx, c0.y + u.y * rpx};
            Color dim = ColorAlpha(lc, 0.65f);
            const char *nm = i == 0 ? "Pôle Sud (face cachée)" : "Pôle Nord (face cachée)";
            int tw = MeasureText(nm, 14);
            DrawCircleLines((int)(view.x + e.x), (int)e.y, 6, dim);
            DrawText(nm, (int)(view.x + e.x) + (u.x >= 0.0f ? 10 : -10 - tw), (int)e.y - 7, 14, dim);
        }
    }
}

/* ===================== Échelle (onglet Site) ===================== */
/* Longueur "ronde" de la barre d'échelle, environ 1/4 du côté de la zone */
static float nice_len_m(void) {
    static const float steps_m[] = {200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000};
    static const float steps_i[] = {152.4f, 304.8f, 804.672f, 1609.344f, 3218.688f, 8046.72f, 16093.44f, 32186.88f, 80467.2f, 160934.4f};   /* 500 ft, 1000 ft, 0.5 mi, 1, 2, 5, 10, 20, 50, 100 mi */
    const float *steps = g_units ? steps_i : steps_m;
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
    const char *txt = fmt_scale_label(len_m);
    int tw = MeasureText(txt, 16);
    int x = (int)(view.x + sp.x) - tw / 2, y = (int)sp.y - 8;
    DrawRectangle(x - 5, y - 2, tw + 10, 20, (Color){16, 21, 28, 215});
    DrawText(txt, x, y, 16, WHITE);
}

/* ===================== Interface du classement ===================== */
static int days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static void civil_from_days(int z, int *y, int *m, int *d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

/* Date calendaire du jour 'day' après le début des éphémérides (EPHEM_START) */
static const char *date_label(float day) {
    static int base = 0, init = 0;
    if (!init) { int y, m, d; sscanf(EPHEM_START, "%d-%d-%d", &y, &m, &d); base = days_from_civil(y, m, d); init = 1; }
    int y, m, d;
    civil_from_days(base + (int)day, &y, &m, &d);
    static const char *mois[12] = {"janv.", "févr.", "mars", "avr.", "mai", "juin", "juil.", "août", "sept.", "oct.", "nov.", "déc."};
    return TextFormat("%d %s %d", d, TR(mois[m - 1]), y);
}

typedef struct {
    float w[5];
    float rad_km, days, start_day, smax, sigma, night, ell, excl_km;
    int relay, tab, sel, n, init;
    char msg[96];                     /* message sous le bouton « Meilleure date » */
    char note[96];                    /* confirmation après « Garder ce site » */
    int save_req;                     /* indice du candidat à enregistrer comme point (-1 = aucun) */
    int from_cache;                   /* 1 = analyse relue depuis le disque */
    char exp_msg[160];                /* résultat du dernier export CSV */
    int ctr_valid, ctr_fail;          /* évaluation du point central choisi */
    float ctr_raw, ctr_lat, ctr_lon;
    Cand c[5];
    float sig[16];                    /* signature des entrées : sert à détecter un changement */
    int cx, cy;                       /* centre de la zone de recherche (cellule) */
} Rank;

static Params rk_params(const Rank *r) {
    Params P;
    memcpy(P.w, r->w, sizeof P.w);
    if (psr_count == 0) P.w[4] = 0.0f;        /* aucune ombre permanente ici : le critère science n'a pas de sens, on l'ignore */
    P.smax_deg = r->smax; P.sigma_max_m = r->sigma; P.night_max_h = r->night; P.ellipse_m = r->ell; P.d0_m = 2000.0f;
    P.relay = r->relay;
    P.len = (int)(r->days * 4.0f);
    if (P.len < 4) P.len = 4;
    P.s0 = (int)(r->start_day * 4.0f);
    if (P.s0 > NY - P.len) P.s0 = NY - P.len;
    if (P.s0 < 0) P.s0 = 0;
    return P;
}

static void rk_signature(const Rank *r, float *s) {
    for (int i = 0; i < 5; i++) s[i] = r->w[i];
    s[5] = r->rad_km; s[6] = r->days; s[7] = r->start_day; s[8] = r->smax; s[9] = r->sigma; s[10] = r->night;
    s[11] = r->ell; s[12] = r->excl_km; s[13] = (float)r->relay; s[14] = (float)r->cx; s[15] = (float)r->cy;
}

static void rk_update(Rank *r, int with_robust) {
    Params P = rk_params(r);
    r->n = rank_region(&P, r->cx, r->cy, r->rad_km * 1000.0, r->excl_km * 1000.0, 5, r->c, with_robust);
    if (r->sel < 0 || r->sel >= r->n) r->sel = r->n ? 0 : -1;
    int ck = r->cy * GW + r->cx;                                   /* le point central choisi par l'utilisateur */
    r->ctr_valid = cell_eval[ck].valid;
    r->ctr_fail = cell_eval[ck].fail;
    r->ctr_raw = cell_raw[ck];
    cell_latlon(ck, &r->ctr_lat, &r->ctr_lon);
}

/* Cercle de recherche posé sur le relief + sphères sur les sites candidats */
static void draw_rank_3d(const Rank *r, float exag) {
    float R = r->rad_km * 1000.0f / cell_m;
    Vector3 prev = {0};
    int have = 0;
    for (int a = 0; a <= 96; a++) {
        float th = a * 2.0f * PI / 96.0f;
        float fx = r->cx + R * cosf(th), fy = r->cy + R * sinf(th);
        int ix = (int)lroundf(fx), iy = (int)lroundf(fy);
        if (ix < 0 || ix >= GW || iy < 0 || iy >= GH) { have = 0; continue; }
        Vector3 p = {fx - GW / 2.0f, dem[iy * GW + ix] / cell_m * exag + 0.8f, fy - GH / 2.0f};
        if (have) DrawLine3D(prev, p, C_SUN);
        prev = p;
        have = 1;
    }
    for (int i = 0; i < r->n; i++) {
        int k = r->c[i].k;
        DrawSphere((Vector3){k % GW - GW / 2.0f, dem[k] / cell_m * exag + 1.8f, k / GW - GH / 2.0f}, 1.3f,
                   !r->c[i].e.valid ? C_BAD : (i == r->sel ? C_SUN : WHITE));
    }
}

static void draw_rank_labels(const Rank *r, Camera3D cam, Rectangle view, int vw, int vh, float exag) {
    for (int i = 0; i < r->n; i++) {
        int k = r->c[i].k;
        Vector3 wp = {k % GW - GW / 2.0f, dem[k] / cell_m * exag + 4.5f, k / GW - GH / 2.0f};
        Vector2 sp = GetWorldToScreenEx(wp, cam, vw, vh);
        int cx = (int)(view.x + sp.x), cy = (int)sp.y;
        DrawCircle(cx, cy, 10, (Color){16, 21, 28, 230});
        DrawCircleLines(cx, cy, 10, !r->c[i].e.valid ? C_BAD : (i == r->sel ? C_SUN : WHITE));
        DrawText(TextFormat("%d", i + 1), cx - 4, cy - 7, 14, WHITE);
    }
}

/* ----- tableau de classement posé sur la vue 3D ----- */
static const char *score_word(float v) { return TR(v >= 80 ? "excellent" : (v >= 60 ? "bon" : (v >= 40 ? "moyen" : "faible"))); }
static Color score_color(float v) { return v >= 80 ? (Color){70, 225, 115, 255} : (v >= 60 ? C_OK : (v >= 40 ? C_SUN : (Color){230, 150, 70, 255})); }

static const char *fail_text(int f) {
    static char buf[160];
    buf[0] = 0;
    if (f & 16) return TR("hors zone étudiée");
    const char *parts[4];
    int n = 0;
    if (f & 8) parts[n++] = TR("nuit trop longue");
    if (f & 4) parts[n++] = TR("Terre jamais visible");
    if (f & 1) parts[n++] = TR("pente trop forte");
    if (f & 2) parts[n++] = TR("terrain rugueux");
    for (int k = 0; k < n; k++) { if (k) strncat(buf, ", ", sizeof buf - strlen(buf) - 1); strncat(buf, parts[k], sizeof buf - strlen(buf) - 1); }
    return buf;
}

static float rank_table_height(const Rank *r) {
    return 30 + 54 + (r->n ? r->n * 44.0f : 26.0f) + ((r->sel >= 0 && r->sel < r->n) ? 180.0f : 0.0f) + 50;
}

static void draw_rank_table(Rank *r, Rectangle ro) {
    Vector2 m = GetMousePosition();
    int x = (int)ro.x, y = (int)ro.y, w = (int)ro.width;
    Params P = rk_params(r);
    float sw = 0;
    for (int i = 0; i < 5; i++) sw += P.w[i];
    DrawRectangleRec(ro, (Color){16, 21, 28, 238});
    DrawRectangleLinesEx(ro, 1, C_LINE);
    DrawText("Classement des sites", x + 10, y + 7, 18, C_TXT);
    int cy = y + 30;

    /* le point choisi sur le terrain */
    DrawRectangle(x + 6, cy, w - 12, 48, (Color){24, 32, 42, 255});
    DrawText(TextFormat("Point choisi (clic sur le terrain) : %.2f, %.2f", r->ctr_lat, r->ctr_lon), x + 12, cy + 5, 12, C_MUT);
    if (r->ctr_raw < 0.0f) DrawText("hors zone étudiée", x + 12, cy + 24, 16, C_BAD);
    else if (r->ctr_valid) DrawText(TextFormat("Note %.0f / 100   (%s)", r->ctr_raw, score_word(r->ctr_raw)), x + 12, cy + 23, 18, score_color(r->ctr_raw));
    else DrawText(TextFormat("Éliminé : %s", fail_text(r->ctr_fail)), x + 12, cy + 24, 15, C_BAD);
    cy += 54;

    if (r->n == 0) { DrawText("Aucun site à classer dans cette zone.", x + 12, cy + 4, 14, C_BAD); cy += 26; }
    for (int i = 0; i < r->n; i++) {
        const Cand *c = &r->c[i];
        Rectangle rr = {(float)x + 6, (float)cy, (float)w - 12, 42};
        int sel = (i == r->sel), ok = c->e.valid;
        DrawRectangleRec(rr, sel ? (Color){44, 58, 76, 255} : (Color){24, 32, 42, 255});
        if (sel) DrawRectangleLinesEx(rr, 1, ok ? C_SUN : C_BAD);
        if (!g_block && CheckCollisionPointRec(m, rr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) r->sel = i;
        if (ok) {
            DrawText(TextFormat("#%d    %.0f / 100    %s", i + 1, c->score, score_word(c->score)), x + 12, cy + 4, 17, score_color(c->score));
            DrawText(TextFormat("%.2f, %.2f   Soleil %.0f %%   Terre %.0f %%   nuit %.0f h", c->lat, c->lon, 100 * c->e.sun_frac,
                                100 * c->e.com_frac, c->e.night_h), x + 12, cy + 25, 12, C_MUT);
        } else {
            DrawText(TextFormat("#%d    %.0f / 100    ÉLIMINÉ", i + 1, c->score), x + 12, cy + 4, 17, C_BAD);
            DrawText(TextFormat("%s   (%.2f, %.2f)", fail_text(c->e.fail), c->lat, c->lon), x + 12, cy + 25, 12, C_MUT);
        }
        cy += 44;
        if (sel) {                                                    /* détail du site sélectionné */
            DrawRectangle(x + 6, cy, w - 12, 176, (Color){20, 27, 36, 255});
            DrawText(c->best_score >= 0 ? TextFormat("Meilleure date : %s  (note %.0f)", date_label(c->best_s0 / 4.0f), c->best_score)
                                        : "Meilleure date : aucune date valide pour ce site", x + 12, cy + 4, 13, C_TXT);
            DrawText(c->robust >= 0 ? TextFormat("Stabilité : %.0f %%  (30 jeux de poids perturbés de 30 %%)", 100 * c->robust)
                                    : (ok ? "Stabilité : calculée au relâchement du curseur" : "Stabilité : sans objet (site éliminé)"), x + 12, cy + 21, 13, C_TXT);
            DrawText("critère", x + 12, cy + 40, 11, C_MUT);
            DrawText("sous-note (0 à 1)", x + 150, cy + 40, 11, C_MUT);
            DrawText("poids", x + 335, cy + 40, 11, C_MUT);
            static const char *names[5] = {"Énergie", "Communication", "Thermique*", "Terrain", "Science"};
            for (int b = 0; b < 5; b++) {
                int by = cy + 54 + b * 16;
                DrawText(b == 4 && psr_count == 0 ? "Science (ignorée)" : names[b], x + 12, by, 13, C_MUT);
                DrawRectangle(x + 150, by + 2, 140, 10, C_LINE);
                DrawRectangle(x + 150, by + 2, (int)(140 * c->e.sub[b]), 10, C_SUN);
                DrawText(TextFormat("%.2f", c->e.sub[b]), x + 296, by, 13, C_TXT);
                DrawText(TextFormat("%.0f %%", sw > 0 ? 100.0f * P.w[b] / sw : 0.0f), x + 335, by, 13, C_MUT);
            }
            DrawText(TextFormat("pente %.1f°   rugosité %s   marge Terre %.1f°", c->slope, fmt_alt_m(c->rough), c->e.margin_deg), x + 12, cy + 137, 12, C_MUT);
            if (ui_button((Rectangle){(float)x + 12, (float)cy + 153, (float)w - 24, 22}, "Garder ce site comme point enregistré", false)) r->save_req = i;
            cy += 180;
        }
    }
    cy += 4;
    if (r->note[0]) DrawText(TextSubtext(r->note, 0, 56), x + 10, cy, 12, C_OK);
    DrawText("Note = moyenne géométrique pondérée des 5 critères (poids = curseurs).", x + 10, cy + 14, 11, C_MUT);
    DrawText("* Thermique : cycles lumière/ombre, ce n'est pas une température.", x + 10, cy + 28, 11, C_MUT);
}

/* ===================== Export CSV ===================== */
static int g_csv_fr = 1;           /* 1 = Excel français (séparateur ; et virgule décimale), 0 = international (, et .) */

static const char *csv_n(double v, int dec) {
    static char buf[4][48];
    static int bi = 0;
    char *b = buf[bi = (bi + 1) & 3];
    snprintf(b, 48, "%.*f", dec, v);
    if (g_csv_fr) for (char *c = b; *c; c++) if (*c == '.') *c = ',';
    return b;
}

static void csv_txt(FILE *f, const char *t) {
    fputc('"', f);
    for (; *t; t++) { if (*t == '"') fputc('"', f); fputc(*t, f); }
    fputc('"', f);
}

static const char *fail_csv(int fl) {
    static char buf[160];
    buf[0] = 0;
    if (fl & 16) return TR("hors zone etudiee");
    const char *parts[4];
    int n = 0;
    if (fl & 8) parts[n++] = TR("nuit trop longue");
    if (fl & 4) parts[n++] = TR("Terre jamais visible");
    if (fl & 1) parts[n++] = TR("pente trop forte");
    if (fl & 2) parts[n++] = TR("terrain rugueux");
    for (int k = 0; k < n; k++) { if (k) strncat(buf, " + ", sizeof buf - strlen(buf) - 1); strncat(buf, parts[k], sizeof buf - strlen(buf) - 1); }
    return buf;
}

/* Date ISO (AAAA-MM-JJ) du jour 'day' après le début des éphémérides */
static void iso_date(int step6, char *out, size_t n) {
    int y, m, d, by, bm, bd;
    sscanf(EPHEM_START, "%d-%d-%d", &by, &bm, &bd);
    civil_from_days(days_from_civil(by, bm, bd) + step6 / 4, &y, &m, &d);
    snprintf(out, n, "%04d-%02d-%02d %02d:00", y, m, d, (step6 % 4) * 6);
}

static void safe_name(const char *in, char *out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-') out[j++] = (char)c;
        else if (j > 0 && out[j - 1] != '_') out[j++] = '_';
    }
    out[j] = 0;
    if (!j) snprintf(out, n, "site");
}

/* Une ligne de CSV : les champs sont ajoutés un par un (le séparateur est géré ici) */
typedef struct { FILE *f; int n; } Row;
static void row_raw(Row *r, const char *s) { if (r->n++) fputs(g_csv_fr ? ";" : ",", r->f); fputs(s, r->f); }
static void row_n(Row *r, double v, int dec) { row_raw(r, csv_n(v, dec)); }
static void row_i(Row *r, int v) { char b[24]; snprintf(b, sizeof b, "%d", v); row_raw(r, b); }
static void row_s(Row *r, const char *s) { if (r->n++) fputs(g_csv_fr ? ";" : ",", r->f); csv_txt(r->f, s); }
static void row_end(Row *r) { fputc('\n', r->f); r->n = 0; }
static void row_hdr(Row *r, const char *const *names, int n) { for (int i = 0; i < n; i++) row_raw(r, TR(names[i])); row_end(r); }

static FILE *csv_open(const char *path) {
    FILE *f = fopen(path, "w");
    if (f && g_csv_fr) fputs("\xEF\xBB\xBF", f);          /* BOM UTF-8 : Excel affiche correctement les accents */
    return f;
}

/* Écrit 4 fichiers dans exports/ : classement, cases, série annuelle, paramètres. Renvoie le nombre de fichiers écrits. */
static int export_csv(Rank *r, int cur_site, int zlev, char *msg, size_t msgn) {
    MKDIR("exports");
    char nm[48], base[160], p[220], d1[32];
    safe_name(cur_site >= 0 ? pts[cur_site].name : "zone_initiale", nm, sizeof nm);
    if (zlev != 1) snprintf(base, sizeof base, "exports/%s_%s", nm, zname[zlev]);
    else snprintf(base, sizeof base, "exports/%s", nm);
    Params P = rk_params(r);
    int files = 0;

    /* 1. classement des sites candidats */
    snprintf(p, sizeof p, "%s_classement.csv", base);
    FILE *f = csv_open(p);
    if (!f) { snprintf(msg, msgn, "%s", TR("Impossible d'écrire dans le dossier exports/")); return 0; }
    Row R = {f, 0};
    static const char *const h1[] = {"rang", "note_sur_100", "valide", "raison_elimination", "latitude", "longitude", "altitude_m", "pente_deg", "rugosite_m",
        "soleil_pct", "terre_pct", "nuit_max_h", "marge_terre_deg", "note_energie", "note_communication", "note_thermique_indicateur", "note_terrain",
        "note_science", "meilleur_debut", "note_au_meilleur_debut", "stabilite_pct"};
    row_hdr(&R, h1, 21);
    for (int i = 0; i < r->n; i++) {
        const Cand *c = &r->c[i];
        row_i(&R, i + 1);
        row_n(&R, c->score, 1);
        row_i(&R, c->e.valid);
        row_s(&R, c->e.valid ? "" : fail_csv(c->e.fail));
        row_n(&R, c->lat, 4); row_n(&R, c->lon, 4); row_n(&R, dem[c->k], 0);
        row_n(&R, c->slope, 2); row_n(&R, c->rough, 1);
        row_n(&R, 100 * c->e.sun_frac, 1); row_n(&R, 100 * c->e.com_frac, 1);
        row_n(&R, c->e.night_h, 0); row_n(&R, c->e.margin_deg, 2);
        for (int b = 0; b < 5; b++) row_n(&R, c->e.sub[b], 3);
        if (c->best_score >= 0) { iso_date(c->best_s0, d1, sizeof d1); row_s(&R, d1); row_n(&R, c->best_score, 1); }
        else { row_raw(&R, ""); row_raw(&R, ""); }
        if (c->robust >= 0) row_n(&R, 100 * c->robust, 0); else row_raw(&R, "");
        row_end(&R);
    }
    fclose(f);
    files++;

    /* 2. toutes les cases de la grille (celles de la zone de recherche portent une note) */
    snprintf(p, sizeof p, "%s_cases.csv", base);
    f = csv_open(p);
    if (f) {
        R.f = f; R.n = 0;
        static const char *const h2[] = {"x", "y", "latitude", "longitude", "altitude_m", "pente_deg", "pente_pire_cas_ellipse_deg", "rugosite_m",
            "ombre_permanente", "dans_zone_recherche", "valide", "note_sur_100", "soleil_pct", "terre_pct", "nuit_max_h", "raison_elimination"};
        row_hdr(&R, h2, 16);
        for (int k = 0; k < NCELL; k++) {
            float la, lo;
            cell_latlon(k, &la, &lo);
            int inr = in_region[k], ev = inr && cell_raw[k] >= 0.0f;
            row_i(&R, k % GW); row_i(&R, k / GW);
            row_n(&R, la, 4); row_n(&R, lo, 4); row_n(&R, dem[k], 0);
            row_n(&R, slope_deg[k], 2); row_n(&R, ter_slope[k], 2); row_n(&R, ter_rough[k], 1);
            row_i(&R, is_psr[k]); row_i(&R, inr); row_i(&R, inr && cell_eval[k].valid);
            if (ev) { row_n(&R, cell_raw[k], 1); row_n(&R, 100 * cell_eval[k].sun_frac, 1); row_n(&R, 100 * cell_eval[k].com_frac, 1); row_n(&R, cell_eval[k].night_h, 0); }
            else { row_raw(&R, ""); row_raw(&R, ""); row_raw(&R, ""); row_raw(&R, ""); }
            row_s(&R, ev && !cell_eval[k].valid ? fail_csv(cell_eval[k].fail) : "");
            row_end(&R);
        }
        fclose(f);
        files++;
    }

    /* 3. série annuelle du site sélectionné (ou du point central) */
    int kk = (r->sel >= 0 && r->sel < r->n) ? r->c[r->sel].k : r->cy * GW + r->cx;
    snprintf(p, sizeof p, "%s_serie_annuelle.csv", base);
    f = csv_open(p);
    if (f) {
        R.f = f; R.n = 0;
        static const char *const h3[] = {"pas_6h", "date_heure_utc", "soleil_visible_pct", "terre_visible_pct", "marge_terre_deg"};
        row_hdr(&R, h3, 5);
        for (int t = 0; t < NY; t++) {
            iso_date(t, d1, sizeof d1);
            row_i(&R, t); row_s(&R, d1);
            row_n(&R, 100.0 * ysun[(size_t)kk * NY + t] / 255.0, 1);
            row_n(&R, 100.0 * yearth[(size_t)kk * NY + t] / 255.0, 1);
            if (yearth[(size_t)kk * NY + t] >= 128) row_n(&R, ymarg[(size_t)kk * NY + t] * 0.1, 1); else row_raw(&R, "");
            row_end(&R);
        }
        fclose(f);
        files++;
    }

    /* 4. paramètres utilisés pour ce classement */
    snprintf(p, sizeof p, "%s_parametres.csv", base);
    f = csv_open(p);
    if (f) {
        R.f = f; R.n = 0;
        char when[40], d0[32];
        time_t now = time(NULL);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
        iso_date(P.s0, d0, sizeof d0);
        #define PRM_S(key, txt) { row_raw(&R, TR(key)); row_s(&R, txt); row_end(&R); }
        #define PRM_N(key, v, dec) { row_raw(&R, TR(key)); row_n(&R, v, dec); row_end(&R); }
        row_raw(&R, "parametre"); row_raw(&R, "valeur"); row_end(&R);
        PRM_S("logiciel", "Selene")
        PRM_S("date_export", when)
        PRM_S("site", cur_site >= 0 ? pts[cur_site].name : TR("zone initiale"))
        PRM_N("latitude_point_central", r->ctr_lat, 4)
        PRM_N("longitude_point_central", r->ctr_lon, 4)
        PRM_N("taille_zone_km", GW * cell_m / 1000.0, 1)
        PRM_N("taille_cellule_m", cell_m, 0)
        PRM_N("rayon_recherche_km", r->rad_km, 1)
        PRM_N("duree_mission_jours", P.len / 4.0, 1)
        PRM_S("debut_mission", d0)
        PRM_N("poids_energie", P.w[0], 0)
        PRM_N("poids_communication", P.w[1], 0)
        PRM_N("poids_thermique", P.w[2], 0)
        PRM_N("poids_terrain", P.w[3], 0)
        PRM_N("poids_science", P.w[4], 0)
        PRM_N("critere_science_ignore", psr_count == 0, 0)
        PRM_N("pente_max_deg", P.smax_deg, 0)
        PRM_N("rugosite_max_m", P.sigma_max_m, 0)
        PRM_N("nuit_continue_max_h", P.night_max_h, 0)
        PRM_N("ellipse_atterrissage_m", P.ellipse_m, 0)
        PRM_N("distance_min_entre_sites_km", r->excl_km, 1)
        PRM_N("relais_disponible", P.relay, 0)
        PRM_N("cases_etudiees", st_eval, 0)
        PRM_N("cases_valides", st_valid, 0)
        PRM_N("elimination_pente_pct", st_eval ? 100.0 * st_fail[0] / st_eval : 0, 1)
        PRM_N("elimination_rugosite_pct", st_eval ? 100.0 * st_fail[1] / st_eval : 0, 1)
        PRM_N("elimination_terre_pct", st_eval ? 100.0 * st_fail[2] / st_eval : 0, 1)
        PRM_N("elimination_nuit_pct", st_eval ? 100.0 * st_fail[3] / st_eval : 0, 1)
        PRM_N("cases_ombre_permanente", psr_count, 0)
        #undef PRM_S
        #undef PRM_N
        fclose(f);
        files++;
    }
    snprintf(msg, msgn, TR("%d fichiers écrits dans exports/ (%s_...)"), files, nm);
    return files;
}

/* ===================== Barre de menus, boîtes de dialogue, notifications ===================== */
enum { A_NONE = 0, A_NEWPT, A_EXPORT, A_OPEN_EXPORTS, A_OPEN_DATA, A_QUIT, A_RENAME, A_DELETE, A_RESET_RK,
       A_TAB0, A_TAB1, A_TAB2, A_TAB3, A_ZOOMIN, A_ZOOMOUT, A_FULLSCREEN, A_PREPARE, A_ANALYZE, A_CLEAR_CACHE,
       A_CSV_FMT, A_DIAG, A_HELP, A_SHORTCUTS, A_ABOUT, A_CLEAR_ALL, A_PREFS, A_MOONFS, A_UI_BIGGER, A_UI_SMALLER, A_UI_AUTO };
typedef struct { const char *label; const char *key; int act; int on; } MItem;
typedef struct { const char *title; MItem it[18]; int n; } MMenu;

static int g_menu_open = -1;           /* menu déroulé (-1 = aucun) */
static int g_dialog = 0;               /* 0 aucune, 1 aide, 2 raccourcis, 3 à propos, 4 vérification */
static int g_dialog_page = 0;
static int g_clear_confirmed = 0;      /* mis à 1 par la boîte de confirmation « Tout supprimer » */
static char g_ci[3][40];               /* saisie : latitude, longitude, nom (boîte « Nouveau point ») */
static int g_ci_focus = 0;
static char g_ci_err[120] = "";
static int g_newpt_req = 0;            /* 1 = marquer sur le globe, 2 = enregistrer comme point */
static float g_newpt_lat = 0, g_newpt_lon = 0;
static char g_newpt_name[32] = "";

/* Valide la saisie ; en cas de succès, demande à la boucle principale de marquer (req 1) ou d'enregistrer (req 2) le point */
static int ci_validate(int req) {
    double la, lo;
    if (!parse_coord(g_ci[0], &la) || la < -90.0 || la > 90.0) { snprintf(g_ci_err, sizeof g_ci_err, "%s", TR("Latitude invalide : un nombre entre -90 et 90 (ex. -89,9 ou 89.9 S).")); return 0; }
    if (!parse_coord(g_ci[1], &lo) || lo < -360.0 || lo > 360.0) { snprintf(g_ci_err, sizeof g_ci_err, "%s", TR("Longitude invalide : un nombre entre -180 et 360 (ex. 45 ou 120 O).")); return 0; }
    while (lo > 180.0) lo -= 360.0;
    while (lo <= -180.0) lo += 360.0;
    g_newpt_lat = (float)la; g_newpt_lon = (float)lo;
    snprintf(g_newpt_name, sizeof g_newpt_name, "%s", g_ci[2]);
    g_newpt_req = req;
    g_ci_err[0] = 0;
    return 1;
}
static char g_toast[200] = "";
static double g_toast_until = 0.0;

static void toast(const char *msg) { snprintf(g_toast, sizeof g_toast, "%s", msg); g_toast_until = GetTime() + 4.5; }

static void mi(MMenu *m, const char *label, const char *key, int act, int on) {
    if (m->n < 18) m->it[m->n++] = (MItem){label, key, act, on};
}

/* Dessine la barre et renvoie l'action choisie (A_NONE sinon). Coordonnées d'écran réelles (pas de décalage). */
static int menu_bar(int sw, MMenu *menus, int nm, int live) {
    Vector2 m = GetMousePosition();
    bool pressed = live && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (!live) g_menu_open = -1;
    int act = A_NONE, consumed = 0;
    DrawRectangle(0, 0, sw, MENU_H, C_PANEL);
    DrawRectangle(0, MENU_H - 1, sw, 1, C_LINE);
    int tx[8] = {0}, tw[8] = {0}, x = 6;
    for (int i = 0; i < nm; i++) {
        tw[i] = MeasureText(menus[i].title, 16) + 22;
        tx[i] = x;
        Rectangle tr = {(float)x, 1, (float)tw[i], MENU_H - 2};
        bool hot = live && CheckCollisionPointRec(m, tr);
        if (g_menu_open == i || hot) DrawRectangleRec(tr, g_menu_open == i ? C_HOT : C_BTN);
        DrawText(menus[i].title, x + 11, 6, 16, C_TXT);
        if (hot && pressed) { g_menu_open = (g_menu_open == i) ? -1 : i; consumed = 1; }
        else if (hot && g_menu_open >= 0 && g_menu_open != i) g_menu_open = i;      /* survol : on change de menu */
        x += tw[i];
    }
    if (g_menu_open >= 0) {
        MMenu *mm = &menus[g_menu_open];
        int w = 220;
        for (int i = 0; i < mm->n; i++) {
            int need = MeasureText(mm->it[i].label, 16) + (mm->it[i].key ? MeasureText(mm->it[i].key, 14) + 40 : 24);
            if (need > w) w = need;
        }
        int h = 8;
        for (int i = 0; i < mm->n; i++) h += (mm->it[i].label[0] == '-' && mm->it[i].label[1] == 0) ? 9 : 26;
        Rectangle box = {(float)tx[g_menu_open], (float)MENU_H, (float)w, (float)h};
        DrawRectangleRec((Rectangle){box.x + 3, box.y + 3, box.width, box.height}, (Color){0, 0, 0, 90});
        DrawRectangleRec(box, C_PANEL);
        DrawRectangleLinesEx(box, 1, C_LINE);
        float yy = box.y + 4;
        for (int i = 0; i < mm->n; i++) {
            const MItem *it = &mm->it[i];
            if (it->label[0] == '-' && it->label[1] == 0) { DrawRectangle((int)box.x + 8, (int)yy + 4, w - 16, 1, C_LINE); yy += 9; continue; }
            Rectangle r = {box.x + 2, yy, (float)w - 4, 24};
            bool hot = CheckCollisionPointRec(m, r);
            if (hot && it->on) DrawRectangleRec(r, C_HOT);
            DrawText(it->label, (int)r.x + 10, (int)yy + 4, 16, it->on ? C_TXT : C_MUT);
            if (it->key) DrawText(it->key, (int)(r.x + r.width - MeasureText(it->key, 14) - 10), (int)yy + 6, 14, C_MUT);
            if (hot && pressed && it->on) { act = it->act; g_menu_open = -1; consumed = 1; }
            else if (hot && pressed) consumed = 1;
            yy += 26;
        }
        if (pressed && !consumed && !CheckCollisionPointRec(m, box)) g_menu_open = -1;     /* clic ailleurs : on referme */
        if (IsKeyPressed(KEY_ESCAPE)) g_menu_open = -1;
    }
    return act;
}

/* ----- textes d'aide ----- */
static const char *HELP_P1[] = {
    "PRISE EN MAIN",
    "",
    "1.  Onglet GLOBE : tournez la Lune (glisser), zoomez (molette), cliquez pour marquer un point,",
    "     puis « Enregistrer ce point », ou saisissez des coordonnées (Fichier > Nouveau point, Ctrl+N).",
    "     Filtres : luminosité à une date, topographie (altitude, écart à la moyenne, rugosité), points d'intérêt.",
    "     Cliquez sur le nom d'un point pour le renommer.",
    "2.  Bouton « Site » à côté d'un point : ouvre le relief détaillé autour de ce point.",
    "     Si la zone n'est pas encore préparée, cliquez sur « Préparer cette zone » (quelques secondes).",
    "3.  Onglet SITE : le Soleil et la Terre sont-ils visibles ? sur 30 jours, avec la pente du terrain.",
    "     Les boutons - et + (en haut à droite) agrandissent ou réduisent la zone étudiée.",
    "4.  Onglet SOL : la vue à 360° depuis le point choisi, avec le Soleil et la Terre dans le ciel.",
    "5.  Onglet ANALYSE : classe automatiquement des sites d'atterrissage autour d'un point.",
    "",
    "TERMES UTILES",
    "",
    "-  Nuit lunaire : environ 14,8 jours d'obscurité (350 h) en dehors des pôles.",
    "-  Ombre permanente : zone où le Soleil ne se lève jamais, là où l'on peut trouver de la glace.",
    "-  Terre visible : permet une liaison radio directe. Au fond d'un cratère, elle est souvent cachée.",
    "",
    "Les données viennent du laser altimètre LOLA et de la caméra LROC (sonde NASA LRO).",
};
static const char *HELP_P2[] = {
    "L'ONGLET ANALYSE",
    "",
    "1.  Cliquez sur « Analyser l'année » (une seule fois par site : le résultat est enregistré).",
    "2.  Cliquez sur le terrain pour choisir le centre de la zone de recherche, réglez le rayon.",
    "3.  Réglez la durée de la mission, sa date de début (ou « Meilleure date »), les priorités.",
    "4.  Le tableau en haut à gauche de la vue 3D donne les 5 meilleurs sites avec leur note sur 100.",
    "5.  Sur la carte : VERT = site valide (plus clair = meilleure note), ROUGE = éliminé, VIOLET = ombre.",
    "",
    "COMMENT LA NOTE EST CALCULÉE",
    "",
    "Filtres éliminatoires : pente, rugosité du terrain, Terre jamais visible, nuit trop longue.",
    "Puis une moyenne pondérée de 5 critères : Énergie, Communication, Thermique*, Terrain, Science.",
    "Les poids sont les curseurs « Priorités » ; les profils Survie, Glace et Relais les règlent d'un clic.",
    "* Thermique : indicateur de cycles lumière/ombre, ce n'est pas une température.",
    "",
    "EXPORT : menu Fichier > Exporter en CSV (4 fichiers dans le dossier exports).",
    "Limites : relief à 200 m par case (blocs et petits cratères invisibles), un seul mois ou un an modélisé.",
};
static const char *KEYS[] = {
    "RACCOURCIS CLAVIER",
    "",
    "Ctrl + 1 / 2 / 3 / 4      Globe / Site / Sol / Analyse",
    "Ctrl + E                  Exporter le classement en CSV",
    "Ctrl + N                  Nouveau point (saisie de coordonnées)",
    "Ctrl + ,                  Préférences (langue, unités)",
    "Ctrl + =  /  Ctrl + -     Interface plus grande / plus petite",
    "Ctrl + 0                  Échelle automatique de l'interface",
    "F1                        Guide d'utilisation",
    "F2                        Renommer le point actuel",
    "F11                       Plein écran",
    "Échap                     Fermer un menu ou une fenêtre, annuler une saisie",
    "",
    "SOURIS",
    "",
    "Glisser                   Tourner la caméra (Globe, Site, Analyse) ou regarder autour (Sol)",
    "Molette                   Zoom (Globe, Site, Analyse) ; défilement du panneau (Analyse)",
    "Clic                      Marquer un point (Globe), choisir un site (Site, Analyse)",
};
static const char *ABOUT[] = {
    "SELENE  -  planificateur de site d'atterrissage lunaire",
    "Version " SELENE_VERSION,
    "",
    "Projet réalisé pour le NASA Space Apps Challenge 2026.",
    "",
    "Données : NASA / LRO (altimètre LOLA, caméra LROC), domaine public.",
    "Éphémérides : modèle analytique de Jean Meeus (Astronomical Algorithms), hors ligne.",
    "Interface : raylib (licence zlib). Compression : sdefl / sinfl (domaine public).",
    "",
    "Les résultats sont des estimations pour la démonstration :",
    "ils ne remplacent pas une étude de mission.",
};

/* ----- vérification de l'installation ----- */
static struct { int globe, tool_exe, tool_py, python_ok, nsites, ntif; char polar[80], global[80], cwd[260], seen[200]; } g_diag;

static void diag_scan(void) {
    memset(&g_diag, 0, sizeof g_diag);
    g_diag.globe = FileExists("data/globe/dem_globe.f32") && FileExists("data/globe/moon_color.png");
    g_diag.tool_exe = FileExists("tools/prepare_data.exe");
    g_diag.tool_py = FileExists("tools/prepare_data.py");
    if (!g_diag.tool_exe && g_diag.tool_py)
        g_diag.python_ok = sln_run_hidden("python --version > nul 2>&1") == 0 || sln_run_hidden("py --version > nul 2>&1") == 0;
    snprintf(g_diag.cwd, sizeof g_diag.cwd, "%s", GetWorkingDirectory());
    const char *dirs[2] = {".", "data"};               /* "data" est parcouru avec tous ses sous-dossiers (source, sources, ...), comme tools/prepare_data.py */
    for (int d = 0; d < 2; d++) {
        if (!DirectoryExists(dirs[d])) continue;
        FilePathList fl = LoadDirectoryFilesEx(dirs[d], ".tif;.tiff;.TIF;.TIFF", d == 1);
        for (unsigned int i = 0; i < fl.count; i++) {
            const char *nm = GetFileName(fl.paths[i]);
            char low[96];
            size_t j = 0;
            for (; nm[j] && j < sizeof low - 1; j++) low[j] = (char)((nm[j] >= 'A' && nm[j] <= 'Z') ? nm[j] + 32 : nm[j]);
            low[j] = 0;
            g_diag.ntif++;
            if (strlen(g_diag.seen) + strlen(nm) + 3 < sizeof g_diag.seen && g_diag.ntif <= 4)
                strcat(strcat(g_diag.seen, g_diag.seen[0] ? ", " : ""), nm);
            if (strstr(low, "ldem") == NULL) continue;
            if (strstr(low, "80s") != NULL) { if (!g_diag.polar[0]) snprintf(g_diag.polar, sizeof g_diag.polar, "%s", nm); }
            else if (strstr(low, "_err") == NULL && strstr(low, "effres") == NULL) { if (!g_diag.global[0]) snprintf(g_diag.global, sizeof g_diag.global, "%s", nm); }
        }
        UnloadDirectoryFiles(fl);
    }
    for (int i = 0; i < npts; i++) g_diag.nsites += site_prepared(i, 1);
}

static void diag_line(int x, int *y, int ok, const char *label, const char *detail) {
    DrawCircle(x + 8, *y + 9, 6, ok ? C_OK : C_BAD);
    DrawText(label, x + 24, *y, 16, C_TXT);
    if (detail && detail[0]) DrawText(detail, x + 24, *y + 20, 13, C_MUT);
    *y += detail && detail[0] ? 46 : 28;
}

/* Boîte de dialogue centrée. Renvoie 1 quand elle vient d'être fermée. */
static int draw_dialog(int sw, int sh_total) {
    bool saved = g_block;
    g_block = false;
    int closed = 0;
    int bw = sw - 120 < 940 ? sw - 120 : 940, bh = sh_total - 120 < 640 ? sh_total - 120 : 640;
    if (g_dialog == 5) { bw = 560; bh = 250; }
    if (g_dialog == 6) { bw = 640; bh = 430; }
    if (g_dialog == 7) { bw = 700; bh = 560; }
    Rectangle box = {(float)(sw - bw) / 2, (float)MENU_H + (float)(sh_total - MENU_H - bh) / 2, (float)bw, (float)bh};
    DrawRectangle(0, MENU_H, sw, sh_total - MENU_H, (Color){0, 0, 0, 150});
    DrawRectangleRec(box, C_PANEL);
    DrawRectangleLinesEx(box, 1, C_LINE);
    const char *const *lines = NULL;
    int n = 0;
    const char *title = "";
    if (g_dialog == 1) { title = "Guide d'utilisation"; lines = g_dialog_page ? HELP_P2 : HELP_P1; n = g_dialog_page ? (int)(sizeof HELP_P2 / sizeof *HELP_P2) : (int)(sizeof HELP_P1 / sizeof *HELP_P1); }
    else if (g_dialog == 2) { title = "Raccourcis clavier"; lines = KEYS; n = (int)(sizeof KEYS / sizeof *KEYS); }
    else if (g_dialog == 3) { title = "À propos de Selene"; lines = ABOUT; n = (int)(sizeof ABOUT / sizeof *ABOUT); }
    else if (g_dialog == 4) title = "Vérification de l'installation";
    else if (g_dialog == 5) title = "Supprimer tous les points ?";
    else if (g_dialog == 6) title = "Nouveau point : saisie des coordonnées";
    else if (g_dialog == 7) title = "Préférences";
    DrawText(title, (int)box.x + 24, (int)box.y + 18, 22, C_SUN);
    int x = (int)box.x + 28, y = (int)box.y + 64;
    if (g_dialog == 4) {
        diag_line(x, &y, g_diag.globe, "Globe lunaire (relief et couleurs)", g_diag.globe ? "" : "Manquant : lancez tools/prepare_globe.py une fois (voir LISEZMOI.txt).");
        int tool = g_diag.tool_exe || g_diag.python_ok;
        diag_line(x, &y, tool, "Outil de préparation des sites",
                  g_diag.tool_exe ? "tools/prepare_data.exe (autonome)" : (g_diag.python_ok ? "Python détecté : tools/prepare_data.py" : "Manquant : placez tools/prepare_data.exe, ou installez Python + rasterio."));
        diag_line(x, &y, g_diag.polar[0] != 0, "Relief LOLA du pôle Sud (80 m par pixel)", g_diag.polar[0] ? g_diag.polar : "Absent : placez LDEM_80S_80MPP_ADJ.tif dans data/source (ou un sous-dossier de data).");
        diag_line(x, &y, g_diag.global[0] != 0, "Relief LOLA mondial (hors pôle Sud)", g_diag.global[0] ? g_diag.global : "Absent : placez ldem_64 ou LDEM128 (.tif) dans data/source (ou un sous-dossier de data).");
        {
            char sb[420];
            snprintf(sb, sizeof sb, TR("Dossier de travail : %s"), g_diag.cwd);
            DrawText(TextSubtext(sb, 0, 120), x, y, 13, C_MUT); y += 18;
            snprintf(sb, sizeof sb, TR("Fichiers .tif/.tiff vus dans ce dossier et dans data/ : %d%s%s"), g_diag.ntif, g_diag.ntif ? "  (" : "", g_diag.ntif ? g_diag.seen : "");
            if (g_diag.ntif) strcat(sb, ")");
            DrawText(TextSubtext(sb, 0, 120), x, y, 13, C_MUT); y += 26;
        }
        char b[96];
        snprintf(b, sizeof b, "%d site(s) déjà préparé(s) sur %d point(s) enregistré(s)", g_diag.nsites, npts);
        diag_line(x, &y, g_diag.nsites > 0, b, g_diag.nsites ? "" : "Les sites préparés s'ouvrent sans aucune installation supplémentaire.");
        DrawText("Sans les fichiers de relief, vous pouvez quand même utiliser tous les sites déjà préparés.", x, y + 6, 14, C_MUT);
        if (ui_button((Rectangle){(float)x, box.y + box.height - 112, 230, 30}, "Ouvrir le dossier des données", false)) { MKDIR("data"); MKDIR("data/source"); sln_open("data"); }
    } else if (g_dialog == 6) {
        static const char *lab[3] = {"Latitude  (de -90 à 90, ex. -89,9  ou  89.9 S)", "Longitude  (Est positif, ex. 45  ou  120 O)", "Nom du point  (facultatif)"};
        int fw[3] = {240, 240, 380};
        Vector2 mp = GetMousePosition();
        for (int i = 0; i < 3; i++) {
            int fy = y + i * 70;
            DrawText(lab[i], x, fy, 14, C_MUT);
            Rectangle fr = {(float)x, (float)fy + 22, (float)fw[i], 32};
            DrawRectangleRec(fr, C_BG);
            DrawRectangleLinesEx(fr, 1, g_ci_focus == i ? C_SUN : C_LINE);
            DrawText(TextFormat("%s%s", g_ci[i], (g_ci_focus == i && ((int)(GetTime() * 2.0) % 2)) ? "|" : ""), x + 8, fy + 30, 16, C_TXT);
            if (CheckCollisionPointRec(mp, fr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_ci_focus = i;
        }
        int ch;
        while ((ch = GetCharPressed()) > 0) {                       /* saisie au clavier dans le champ actif */
            char *buf = g_ci[g_ci_focus];
            int len = (int)strlen(buf), maxlen = g_ci_focus == 2 ? 24 : 18;
            int okc = g_ci_focus == 2 ? (ch >= 32 && ch != 127) : ((ch < 128 && strchr("0123456789+-.,NSEWOnsewo", ch) != NULL) || ch == 176);
            if (okc && len < maxlen) {
                int sz = 0;
                const char *u = CodepointToUTF8(ch, &sz);
                memcpy(buf + len, u, (size_t)sz);
                buf[len + sz] = 0;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            char *buf = g_ci[g_ci_focus];
            int len = (int)strlen(buf);
            while (len > 0 && ((unsigned char)buf[len - 1] & 0xC0) == 0x80) len--;
            if (len > 0) buf[len - 1] = 0;
        }
        if (IsKeyPressed(KEY_TAB)) g_ci_focus = (g_ci_focus + 1) % 3;
        DrawText("Entrée : enregistrer   |   Tab : champ suivant   |   Échap : annuler", x, y + 3 * 70 - 4, 13, C_MUT);
        if (g_ci_err[0]) DrawText(g_ci_err, x, y + 3 * 70 + 16, 14, C_BAD);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) { if (ci_validate(2)) closed = 1; }
    } else if (g_dialog == 7) {
        DrawText("Langue de l'interface", x, y, 16, C_TXT); y += 30;
        for (int i = 0; i < g_nlangs; i++) {
            Rectangle lr = {(float)(x + (i % 4) * 160), (float)(y + (i / 4) * 38), 150, 30};
            if (ui_button(lr, g_langs[i].name, strcmp(g_lang, g_langs[i].code) == 0)) { tr_load(g_langs[i].code); save_settings(); }
        }
        y += ((g_nlangs + 3) / 4) * 38 + 18;
        DrawText("Unités de mesure", x, y, 16, C_TXT); y += 30;
        if (ui_button((Rectangle){(float)x, (float)y, 230, 30}, "Métrique (m, km)", g_units == 0)) { g_units = 0; save_settings(); }
        if (ui_button((Rectangle){(float)x + 240, (float)y, 230, 30}, "Impérial (ft, mi)", g_units == 1)) { g_units = 1; save_settings(); }
        y += 52;
        DrawText("Échelle de l'interface", x, y, 16, C_TXT); y += 30;
        {
            static const float opt[8] = {0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};       /* 0 = automatique */
            for (int i = 0; i < 8; i++) {
                Rectangle sr = {(float)(x + (i % 4) * 160), (float)(y + (i / 4) * 36), 150, 28};
                const char *lb = i == 0 ? "Auto" : TextFormat("%.0f %%", opt[i] * 100.0f);
                if (ui_button(sr, lb, fabsf(g_ui_scale - opt[i]) < 0.001f)) { g_ui_scale = opt[i]; save_settings(); }
            }
        }
        y += 84;
        DrawText("Les exports CSV restent toujours en unités métriques (m, km).", x, y, 14, C_MUT); y += 26;
        DrawText("Ajouter une langue : copiez lang/_modele.txt en lang/xx.txt, traduisez la colonne de droite,", x, y, 14, C_MUT); y += 20;
        DrawText("puis ajoutez en première ligne  #name=NomDeLaLangue.  Aucune installation n'est nécessaire.", x, y, 14, C_MUT);
    } else if (g_dialog == 5) {
        DrawText(TextFormat("%d point(s) enregistré(s) vont être retirés de la liste et du globe.", npts), x, y, 16, C_TXT);
        DrawText("Les sites déjà préparés restent sur le disque (aucun fichier n'est effacé).", x, y + 30, 14, C_MUT);
        DrawText("Cette action est définitive.", x, y + 60, 14, C_BAD);
    } else {
        for (int i = 0; i < n; i++) {
            bool head = lines[i][0] && lines[i][0] >= 'A' && lines[i][0] <= 'Z' && lines[i][1] >= 'A' && lines[i][1] <= 'Z';
            DrawText(lines[i], x, y, head ? 17 : 15, head ? C_SUN : (g_dialog == 3 && i == 0 ? C_TXT : C_TXT));
            y += lines[i][0] ? 22 : 12;
        }
    }
    if (g_dialog == 1) {
        DrawText(TextFormat("Page %d / 2", g_dialog_page + 1), (int)box.x + 28, (int)(box.y + box.height - 34), 14, C_MUT);
        if (ui_button((Rectangle){box.x + box.width - 380, box.y + box.height - 46, 110, 30}, g_dialog_page ? "Précédent" : "Suivant", false)) g_dialog_page = !g_dialog_page;
    }
    if (g_dialog == 5) {
        if (ui_button((Rectangle){box.x + box.width - 300, box.y + box.height - 46, 130, 30}, "Annuler", false) || IsKeyPressed(KEY_ESCAPE)) closed = 1;
        if (ui_button((Rectangle){box.x + box.width - 160, box.y + box.height - 46, 130, 30}, "Tout supprimer", true)) { g_clear_confirmed = 1; closed = 1; }
    } else if (g_dialog == 6) {
        if (ui_button((Rectangle){box.x + box.width - 470, box.y + box.height - 46, 100, 30}, "Annuler", false) || IsKeyPressed(KEY_ESCAPE)) closed = 1;
        if (ui_button((Rectangle){box.x + box.width - 360, box.y + box.height - 46, 170, 30}, "Marquer seulement", false)) { if (ci_validate(1)) closed = 1; }
        if (ui_button((Rectangle){box.x + box.width - 180, box.y + box.height - 46, 150, 30}, "Enregistrer", true)) { if (ci_validate(2)) closed = 1; }
    } else if (ui_button((Rectangle){box.x + box.width - 140, box.y + box.height - 46, 110, 30}, "Fermer", true) || IsKeyPressed(KEY_ESCAPE)) closed = 1;
    g_block = saved;
    return closed;
}

/* Défilement du panneau de gauche : une position par onglet (Globe et Analyse défilent) */
#define panel_scroll pscroll[mode]
#define panel_max pmax_[mode]

/* Les noms des points d'intérêt n'apparaissent que quand on a assez zoomé (sinon la sphère serait illisible) */
static int dist_ok_labels(float d) { return d < 235.0f; }

/* Échelle d'interface voulue. Automatique = le plus grand de (échelle d'affichage de Windows, hauteur du moniteur / 1080), arrondi au quart.
 * Jamais plus grande que ce que le moniteur peut afficher (l'interface garde une taille logique minimale de 900 x 728). */
static float ui_scale_wanted(void) {
    int cm = GetCurrentMonitor();
    float mw = (float)GetMonitorWidth(cm), mh = (float)GetMonitorHeight(cm);
    float s = g_ui_scale;
    if (s <= 0.0f) {
        Vector2 d = GetWindowScaleDPI();
        s = fmaxf(fmaxf(d.x, d.y), mh / 1080.0f);
        s = roundf(s * 4.0f) / 4.0f;
    }
    if (s < 1.0f) s = 1.0f;
    if (s > 3.0f) s = 3.0f;
    float cap = fminf(mw * 0.98f / 900.0f, mh * 0.92f / (float)(700 + TOP_H));
    if (s > cap) s = fmaxf(cap, 0.75f);
    return s;
}

/* ===================== Programme principal ===================== */
typedef struct { bool valid; int x, y; } Site;

int main(void) {
    sln_hide_console();                             /* pas de fenêtre de commande à l'arrière-plan */
    ChangeDirectory(GetApplicationDirectory());     /* les données sont cherchées à côté de l'exécutable, quel que soit le mode de lancement */
    /* --- données initiales : zone préparée dans data/input (ou fictive) --- */
    load_points();
    load_site_data("data/input");
    globe_eph_ok = load_globe_eph("data/input/ephem_year.csv");           /* les directions du Soleil et de la Terre ne dépendent pas du site */
    for (int i = 0; i < npts && !globe_eph_ok; i++) {
        char pe[300];
        snprintf(pe, sizeof pe, "data/sites/%s/ephem_year.csv", pts[i].id);
        globe_eph_ok = load_globe_eph(pe);
    }
    zone_valid = real_dem && load_zone_center("data/input/dem.txt", &zone_lat, &zone_lon);

    /* --- fenêtre --- */
    load_settings();                                    /* langue et unités choisies à la dernière session */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1440, 860 + TOP_H, TextFormat("Selene %s - %s", SELENE_VERSION, TR("Planificateur de site lunaire")));
    /* --- Icône de la fenêtre et logo du panneau (data/logo.png, optionnel) --- */
    Texture2D logo_tex = {0};
    if (FileExists("data/logo.png")) {
        Image icon = LoadImage("data/logo.png");
        SetWindowIcon(icon);
        UnloadImage(icon);
        logo_tex = LoadTexture("data/logo.png");
        SetTextureFilter(logo_tex, TEXTURE_FILTER_BILINEAR);
    }
    {                                                   /* taille de départ proportionnelle à l'échelle : grand écran = grande fenêtre */
        int cm = GetCurrentMonitor(), mw = GetMonitorWidth(cm), mh = GetMonitorHeight(cm);
        Vector2 mp = GetMonitorPosition(cm);
        float s0 = ui_scale_wanted();
        int w = (int)fminf(1440.0f * s0, mw * 0.96f), h = (int)fminf((860.0f + TOP_H) * s0, mh * 0.90f);
        SetWindowSize(w, h);
        SetWindowPosition((int)mp.x + (mw - w) / 2, (int)mp.y + (mh - h) / 4);
    }
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);                        /* Échap sert à annuler une saisie, il ne ferme plus la fenêtre */
    if (!DirectoryExists("data")) {
        toast("Dossier « data » introuvable à côté de selene.exe : données fictives. Placez selene.exe dans le dossier du projet.");
        g_toast_until = GetTime() + 12.0;
    }

    build_mesh();
    UploadMesh(&mesh, true);
    Material mat = LoadMaterialDefault();

    int have_globe = load_globe();
    Material gmat = LoadMaterialDefault();
    if (have_globe) SetMaterialTexture(&gmat, MATERIAL_MAP_DIFFUSE, g_tex);
    Material gmat_plain = LoadMaterialDefault();                 /* sans texture : pour les dégradés de couleur */

    int mode = have_globe ? 0 : 1;               /* 0 = globe, 1 = site, 2 = sol, 3 = analyse */
    int cur_site = -1;                           /* -1 = zone initiale, sinon indice dans pts[] */
    int site_ready = 1;
    int edit_idx = -1;                           /* point en cours de renommage (-1 = aucun) */
    char edit_buf[32] = "";
    Rectangle edit_rect = {0, 0, 0, 0};
    int pending_load = -2, pending_prepare = -2;
    int zlev = 1;                                /* niveau de taille de zone (index dans zfac) */
    Rank rk = {.w = {6, 6, 4, 6, 3}, .rad_km = 6.4f, .days = 7.0f, .smax = 10.0f, .sigma = 20.0f, .night = 120.0f,
               .ell = 300.0f, .excl_km = 1.0f, .sel = -1, .save_req = -1};
    int pending_year = 0, force_recolor = 0, rk_force = 1, cache_tried = 0;
    int quit_req = 0, menu_act_prev = A_NONE, busy_armed = 0;
    float pscroll[4] = {0}, pmax_[4] = {0};
    const void *prev_active = NULL;
    float gyaw = 0.0f, gpitch = 0.3f, gdist = 260.0f, gexag = 1.0f, prev_gexag = -1.0f;
    int gpick_valid = 0;
    float gpick_lat = 0, gpick_lon = 0;
    char gpick_name[40] = "";                    /* nom du point d'intérêt marqué (vide si point libre) */
    int gfilter = 0, gplay = 0, gdirty = 1, prev_gfilter = -1, prev_gstep = -1;   /* filtres du globe : 0 aucun, 1 luminosité, 2 altitude, 3 écart à la moyenne, 4 rugosité */
    float gday = 0.0f;
    int poi_on[3] = {0, 0, 0};

    float exag = 1.0f, smax = 15.0f, time_f = 0.0f;
    float rexp = 10.0f, prev_rexp = -1.0f;            /* vue réelle : exposition */
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
    const char *layer_names[5] = {"Éclairement solaire", "Visibilité de la Terre", "Altitude", "Pente", "Vue réelle"};

    while (!WindowShouldClose() && !quit_req) {
        {                                             /* échelle de l'interface */
            static float min_s = 0.0f;
            float wnt = ui_scale_wanted();
            if (fabsf(wnt - min_s) > 0.001f) {
                int nw = (int)(900.0f * wnt), nh = (int)((700.0f + TOP_H) * wnt);
                SetWindowMinSize(nw, nh);
                if (min_s > 0.0f && wnt > min_s && !IsWindowMaximized()) {          /* plus grand : on agrandit la fenêtre si besoin, sans sortir de l'écran */
                    int cm = GetCurrentMonitor(), mw = GetMonitorWidth(cm), mh = GetMonitorHeight(cm);
                    int ww = GetScreenWidth() < nw ? nw : GetScreenWidth(), hh = GetScreenHeight() < nh ? nh : GetScreenHeight();
                    Vector2 wp = GetWindowPosition(), mp = GetMonitorPosition(cm);
                    if (wp.x + ww > mp.x + mw) wp.x = fmaxf(mp.x, mp.x + mw - ww);
                    if (wp.y + hh > mp.y + mh) wp.y = fmaxf(mp.y, mp.y + mh - hh);
                    SetWindowSize(ww, hh);
                    SetWindowPosition((int)wp.x, (int)wp.y);
                }
                min_s = wnt;
            }
            float lim = fminf(GetScreenWidth() / 900.0f, GetScreenHeight() / (float)(700 + TOP_H));   /* garde-fou : l'interface doit tenir dans la fenêtre */
            g_ui_s = fmaxf(fminf(wnt, lim), 0.6f);
            font_update();                            /* avant tout dessin : la police est reconstruite à la bonne taille */
        }
        SetMouseScale(1.0f / g_ui_s, 1.0f / g_ui_s);
        SetMouseOffset(0, (int)(-TOP_H * g_ui_s));      /* le contenu est dessiné sous les barres */

        /* --- actions des menus et raccourcis clavier --- */
        int act = menu_act_prev;
        menu_act_prev = A_NONE;
        if (edit_idx < 0 && g_dialog == 0) {
            bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (ctrl && IsKeyPressed(KEY_ONE)) act = A_TAB0;
            if (ctrl && IsKeyPressed(KEY_TWO)) act = A_TAB1;
            if (ctrl && IsKeyPressed(KEY_THREE)) act = A_TAB2;
            if (ctrl && IsKeyPressed(KEY_FOUR)) act = A_TAB3;
            if (ctrl && IsKeyPressed(KEY_E)) act = A_EXPORT;
            if (ctrl && IsKeyPressed(KEY_N)) act = A_NEWPT;
            if (ctrl && IsKeyPressed(KEY_COMMA)) act = A_PREFS;
            if (ctrl && (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))) act = A_UI_BIGGER;
            if (ctrl && (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT) || IsKeyPressed(KEY_SIX))) act = A_UI_SMALLER;     /* KEY_SIX : le « - » d'un clavier AZERTY */
            if (ctrl && (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_KP_0))) act = A_UI_AUTO;
            if (IsKeyPressed(KEY_F1)) act = A_HELP;
            if (IsKeyPressed(KEY_F2)) act = A_RENAME;
            if (IsKeyPressed(KEY_F11)) act = A_FULLSCREEN;
            if (g_moon_fs && IsKeyPressed(KEY_ESCAPE)) g_moon_fs = 0;
        }
        switch (act) {
            case A_NEWPT:
                if (have_globe) { g_ci[0][0] = g_ci[1][0] = g_ci[2][0] = 0; g_ci_focus = 0; g_ci_err[0] = 0; g_dialog = 6; }
                else toast("Globe indisponible : voir Aide > Vérifier l'installation.");
                break;
            case A_EXPORT:
                if (mode == 3 && site_ready && year_ready) { export_csv(&rk, cur_site, zlev, rk.exp_msg, sizeof rk.exp_msg); toast(rk.exp_msg); }
                else toast("Ouvrez l'onglet Analyse et lancez l'analyse avant d'exporter.");
                break;
            case A_OPEN_EXPORTS: MKDIR("exports"); sln_open("exports"); break;
            case A_OPEN_DATA: MKDIR("data"); sln_open("data"); break;
            case A_QUIT: quit_req = 1; break;
            case A_RENAME:
                if (cur_site >= 0 && have_globe) { mode = 0; edit_idx = cur_site; snprintf(edit_buf, sizeof edit_buf, "%s", pts[cur_site].name); }
                else toast("Sélectionnez d'abord un point enregistré (onglet Globe, bouton « Site »).");
                break;
            case A_DELETE:
                if (cur_site >= 0) {
                    for (int i = cur_site; i < npts - 1; i++) pts[i] = pts[i + 1];
                    npts--;
                    save_points();
                    cur_site = -1; pending_load = -1; zlev = 1; edit_idx = -1;
                    toast("Point supprimé.");
                } else toast("Aucun point enregistré n'est sélectionné.");
                break;
            case A_RESET_RK: {
                float dw[5] = {6, 6, 4, 6, 3};
                memcpy(rk.w, dw, sizeof dw);
                rk.days = 7.0f; rk.smax = 10.0f; rk.sigma = 20.0f; rk.night = 120.0f; rk.ell = 300.0f; rk.excl_km = 1.0f; rk.relay = 0;
                rk.rad_km = 0.25f * GW * cell_m / 1000.0f;
                rk_force = 1;
                toast("Réglages du classement réinitialisés.");
                break; }
            case A_TAB0: if (have_globe) mode = 0; break;
            case A_TAB1: mode = 1; break;
            case A_TAB2: mode = 2; if (!site.valid) { site.valid = true; site.x = GW / 2; site.y = GH / 2; } break;
            case A_TAB3: mode = 3; break;
            case A_ZOOMIN: if (zlev < 4 && (cur_site >= 0 || zone_valid)) { zlev++; pending_load = cur_site; } break;
            case A_ZOOMOUT: if (zlev > 0 && (cur_site >= 0 || zone_valid)) { zlev--; pending_load = cur_site; } break;
            case A_FULLSCREEN: ToggleBorderlessWindowed(); break;
            case A_MOONFS: g_moon_fs = !g_moon_fs; break;
            case A_UI_BIGGER: case A_UI_SMALLER: {
                static const float st[7] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
                float cw = g_ui_scale > 0.0f ? g_ui_scale : ui_scale_wanted(), nv = cw;
                if (act == A_UI_BIGGER) { nv = st[6]; for (int i = 6; i >= 0; i--) if (st[i] > cw + 0.001f) nv = st[i]; }
                else { nv = st[0]; for (int i = 0; i < 7; i++) if (st[i] < cw - 0.001f) nv = st[i]; }
                g_ui_scale = nv;
                save_settings();
                toast(TextFormat("Échelle de l'interface : %.0f %%", nv * 100.0f));
                break; }
            case A_UI_AUTO:
                g_ui_scale = 0.0f;
                save_settings();
                toast("Échelle de l'interface : automatique");
                break;
            case A_PREPARE:
                if (cur_site >= 0 || zone_valid) pending_prepare = cur_site;
                else toast("Choisissez d'abord un point enregistré (onglet Globe).");
                break;
            case A_ANALYZE:
                mode = 3;
                if (have_frame && year_loaded && !year_ready) pending_year = 1;
                else if (!have_frame || !year_loaded) toast("Ce site doit être re-préparé (Outils > Préparer / re-préparer).");
                break;
            case A_CLEAR_CACHE: {
                char dc[300], pc[340];
                site_dir(cur_site, zlev, dc, sizeof dc);
                snprintf(pc, sizeof pc, "%s/analysis.bin", dc);
                if (FileExists(pc)) { remove(pc); toast("Analyse enregistrée supprimée pour ce site."); }
                else toast("Aucune analyse enregistrée pour ce site.");
                break; }
            case A_CSV_FMT:
                g_csv_fr = !g_csv_fr;
                toast(g_csv_fr ? "Export CSV : format Excel français (; et virgule)." : "Export CSV : format international (, et point).");
                break;
            case A_DIAG: diag_scan(); g_dialog = 4; break;
            case A_PREFS: lang_scan(); g_dialog = 7; break;
            case A_HELP: g_dialog = 1; g_dialog_page = 0; break;
            case A_SHORTCUTS: g_dialog = 2; break;
            case A_ABOUT: g_dialog = 3; break;
            case A_CLEAR_ALL:
                if (npts > 0) g_dialog = 5;
                else toast("Aucun point à supprimer.");
                break;
            default: break;
        }

        if (g_newpt_req) {                          /* coordonnées saisies : on centre le globe dessus, et on enregistre si demandé */
            int req = g_newpt_req;
            g_newpt_req = 0;
            if (have_globe) mode = 0;
            gpick_lat = g_newpt_lat; gpick_lon = g_newpt_lon; gpick_valid = 1;
            snprintf(gpick_name, sizeof gpick_name, "%s", g_newpt_name);
            gyaw = PI / 2.0f + g_newpt_lon * DEG2RAD;
            gpitch = Clamp(g_newpt_lat * DEG2RAD, -1.45f, 1.45f);
            if (gdist > 190.0f) gdist = 190.0f;
            if (req == 2) {
                if (npts < MAX_PTS) {
                    snprintf(pts[npts].id, sizeof pts[npts].id, "P%d", next_id++);
                    if (g_newpt_name[0]) snprintf(pts[npts].name, sizeof pts[npts].name, "%s", g_newpt_name);
                    else snprintf(pts[npts].name, sizeof pts[npts].name, "%s", pts[npts].id);
                    pts[npts].lat = g_newpt_lat; pts[npts].lon = g_newpt_lon;
                    npts++;
                    save_points();
                    toast(TextFormat("Point « %s » enregistré (%.3f, %.3f).", pts[npts - 1].name, g_newpt_lat, g_newpt_lon));
                } else toast("Maximum de points atteint : supprimez-en un d'abord.");
            } else toast(TextFormat("Point marqué : %.3f, %.3f.", g_newpt_lat, g_newpt_lon));
        }
        if (g_clear_confirmed) {                    /* confirmation reçue : on efface tous les points */
            g_clear_confirmed = 0;
            int n_del = npts;
            npts = 0;
            save_points();
            edit_idx = -1;
            if (cur_site >= 0) { cur_site = -1; pending_load = -1; zlev = 1; }     /* rechargement seulement si le site affiché venait d'un point supprimé */
            toast(TextFormat("%d point(s) supprimé(s).", n_del));
        }

        /* --- tâches lourdes : le message s'affiche d'abord par-dessus l'écran, le calcul a lieu à l'image suivante --- */
        /* à l'ouverture de l'onglet Analyse, une analyse déjà enregistrée sur le disque est rechargée automatiquement */
        if (mode == 3 && site_ready && have_frame && year_loaded && !year_ready && !cache_tried) {
            cache_tried = 1;
            char dc[300], pc[340];
            site_dir(cur_site, zlev, dc, sizeof dc);
            snprintf(pc, sizeof pc, "%s/analysis.bin", dc);
            if (FileExists(pc)) pending_year = 1;
        }
        int run_tasks = 1;
        {
            int heavy_load = pending_load != -2 && ((pending_load < 0 && zlev == 1) || site_prepared(pending_load, zlev));
            if ((pending_prepare != -2 || heavy_load || pending_year) && !busy_armed) {
                const char *bm;
                if (pending_prepare != -2) bm = "Préparation du site (quelques secondes)...";
                else if (heavy_load) bm = "Chargement du site (calcul de l'éclairement)...";
                else {
                    char dy2[300], pc2[340];
                    site_dir(cur_site, zlev, dy2, sizeof dy2);
                    snprintf(pc2, sizeof pc2, "%s/analysis.bin", dy2);
                    bm = FileExists(pc2) ? "Chargement de l'analyse enregistrée..." : "Analyse de l'année complète (quelques secondes)...";
                }
                snprintf(g_busy, sizeof g_busy, "%s", bm);
                busy_armed = 1;
                run_tasks = 0;
            } else if (busy_armed) { busy_armed = 0; g_busy[0] = 0; }
        }
        if (run_tasks) {
            if (pending_prepare != -2) {
                int idx = pending_prepare; pending_prepare = -2;
                run_prepare(idx, zlev);
                if (site_prepared(idx, zlev)) pending_load = idx;
            }
            if (pending_load != -2) {
                int idx = pending_load; pending_load = -2;
                cur_site = idx;
                if (!(idx < 0 && zlev == 1) && !site_prepared(idx, zlev)) {
                    site_ready = 0;
                } else {
                    char dir[300];
                    site_dir(idx, zlev, dir, sizeof dir);
                    load_site_data(dir);
                    if (!globe_eph_ok) {
                        char pe[340];
                        snprintf(pe, sizeof pe, "%s/ephem_year.csv", dir);
                        globe_eph_ok = load_globe_eph(pe);
                    }
                    site_ready = 1;
                    site.valid = true; site.x = GW / 2; site.y = GH / 2;
                    prev_exag = -1.0f; prev_t = -1;
                    rk.n = 0; rk.sel = -1; rk.start_day = 0.0f; rk.rad_km = 0.25f * GW * cell_m / 1000.0f;
                    rk_force = 1; memset(pscroll, 0, sizeof pscroll); rk.note[0] = 0; rk.exp_msg[0] = 0; rk.from_cache = 0; cache_tried = 0;
                }
            }
            if (pending_year) {
                pending_year = 0;
                char dy[300], pcache[340];
                site_dir(cur_site, zlev, dy, sizeof dy);
                snprintf(pcache, sizeof pcache, "%s/analysis.bin", dy);
                rk.from_cache = 0;
                if (FileExists(pcache)) {
                    rk.from_cache = load_analysis(dy);
                }
                if (!rk.from_cache) {
                    compute_horizon();
                    compute_year();
                    save_analysis(dy);
                }
                rk.cx = site.valid ? site.x : GW / 2; rk.cy = site.valid ? site.y : GH / 2;
                ensure_terrain(rk.ell);
                Params P0 = rk_params(&rk);
                int bd0 = best_start_region(&P0, rk.cx, rk.cy, rk.rad_km * 1000.0);                 /* on démarre à la meilleure date */
                rk.start_day = bd0 >= 0 ? bd0 / 4.0f : 0.0f;
                rk_force = 1; force_recolor = 1;
            }
        }
        g_block = g_dd_open || g_menu_open >= 0 || g_dialog != 0;
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

        int sw = (int)(GetScreenWidth() / g_ui_s), sh_total = (int)(GetScreenHeight() / g_ui_s), sh = sh_total - TOP_H;
        int gm = (mode == 0);
        int sm = (mode == 2);                    /* mode vue du sol */
        int real_view = (mode == 1 && layer == 4);        /* onglet Site, couche « Vue réelle » */
        float eff_exag = (sm || real_view) ? 1.0f : exag;       /* en vue du sol, relief réel (x1) indispensable pour un horizon exact */
        int panel_w = g_moon_fs ? 0 : PANEL_W;                 /* plein écran de la vue Lune : ni panneau ni frise */
        Rectangle view = {(float)panel_w, 0, (float)(sw - panel_w), (float)((gm || g_moon_fs) ? sh : sh - TL_H)};
        Rectangle tl = {PANEL_W, (float)(sh - TL_H), (float)(sw - PANEL_W), TL_H};
        Vector2 m = GetMousePosition();
        int vw = (int)view.width, vh = (int)view.height;
        int rtw = (int)(vw * g_ui_s + 0.5f), rth = (int)(vh * g_ui_s + 0.5f);       /* la vue 3D est rendue en résolution réelle (nette quelle que soit l'échelle) */
        if (rtw != rt_w || rth != rt_h) {
            if (rt.id) UnloadRenderTexture(rt);
            rt = LoadRenderTexture(rtw, rth);
            SetTextureFilter(rt.texture, TEXTURE_FILTER_BILINEAR);
            rt_w = rtw; rt_h = rth;
        }

        /* --- caméra (orbitale pour Globe/Site/Classer, première personne pour Sol) --- */
        float *cy = gm ? &gyaw : (sm ? &gnd_yaw : &yaw);
        float *cp = gm ? &gpitch : (sm ? &gnd_pitch : &pitch);
        float *cd = gm ? &gdist : &dist;
        float pmin = gm ? -1.5f : (sm ? -0.45f : 0.12f);
        float pmax = sm ? 1.25f : 1.5f;
        float dmin = gm ? 130.0f : 25.0f, dmax = gm ? 600.0f : 400.0f;

        Rectangle zo = {view.x + view.width - 262, 6, 256, 30};      /* boutons de taille de zone */
        Rectangle ro = {view.x + 12, 34, 392, (mode == 3 && site_ready && year_ready) ? rank_table_height(&rk) : 0.0f};   /* tableau de classement */
        Rectangle fsb = {view.x + view.width - 44, view.y + view.height - 44, 34, 34};   /* bouton plein écran, angle inférieur droit de la vue Lune */
        bool in_view = !g_block && CheckCollisionPointRec(m, view) && !CheckCollisionPointRec(m, fsb)
                       && (gm || (!CheckCollisionPointRec(m, zo) && !CheckCollisionPointRec(m, ro)));
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
        if (!g_moon_fs && (mode == 3 || mode == 0) && m.x < PANEL_W) panel_scroll = Clamp(panel_scroll - GetMouseWheelMove() * 40.0f, 0.0f, panel_max);

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
                int snapped = -1;                                      /* un point d'intérêt tout proche du clic ? */
                float bestd = 14.0f;
                for (int i = 0; i < NPOI; i++) {
                    if (!poi_on[POIS[i].cat]) continue;
                    Vector3 wp = Vector3Scale(sph(POIS[i].lat, POIS[i].lon), GLOBE_R * 1.01f);
                    if (Vector3DotProduct(Vector3Normalize(wp), Vector3Normalize(cam.position)) < 0.15f) continue;
                    Vector2 sp = GetWorldToScreenEx(wp, cam, vw, vh);
                    float dd = Vector2Distance(sp, (Vector2){m.x - view.x, m.y - view.y});
                    if (dd < bestd) { bestd = dd; snapped = i; }
                }
                if (snapped >= 0) {
                    gpick_lat = POIS[snapped].lat; gpick_lon = POIS[snapped].lon; gpick_valid = 1;
                    snprintf(gpick_name, sizeof gpick_name, "%s", TR(POIS[snapped].name));
                } else {
                    RayCollision hit = GetRayCollisionSphere(ray, (Vector3){0, 0, 0}, GLOBE_R);
                    if (hit.hit) {
                        Vector3 n = Vector3Normalize(hit.point);
                        gpick_lat = asinf(n.y) * RAD2DEG;
                        gpick_lon = atan2f(-n.z, n.x) * RAD2DEG;
                        gpick_valid = 1;
                        gpick_name[0] = 0;
                    }
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

        /* --- azimut / élévation du Soleil et de la Terre pour le site actif --- */
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
        int eff_layer = (mode == 3) ? 4 : (layer == 4 ? 5 : layer);
        if (t != prev_t || eff_layer != prev_layer || smax != prev_smax || force_recolor || mode != prev_mode || rexp != prev_rexp) {
            if (sm && site.valid) recolor_ground(t, site.y * GW + site.x);
            else if (eff_layer == 5) recolor_real(t, site.valid ? site.y * GW + site.x : (GH / 2) * GW + GW / 2, rexp);
            else recolor(eff_layer, t, smax);
            prev_rexp = rexp;
            prev_t = t; prev_layer = eff_layer; prev_smax = smax; prev_mode = mode; force_recolor = 0;
        }
        if (gm && have_globe) {
            if (gexag != prev_gexag) { globe_update_geometry(gexag); prev_gexag = gexag; gdirty = 1; }
            if (!globe_eph_ok && gfilter == 1) gfilter = 0;                   /* la luminosité demande les éphémérides d'un site préparé */
            if (gplay && gfilter == 1) { gday += GetFrameTime() * 6.0f; if (gday > 365.0f) gday = 0.0f; }        /* lecture : 6 jours par seconde */
            int gstep = (int)Clamp(gday * 4.0f, 0.0f, (float)(NY - 1));
            Vector3 to_cam = Vector3Normalize(cam.position);
            Vector3 right = Vector3Normalize(Vector3CrossProduct(Vector3Negate(to_cam), (Vector3){0, 1, 0}));
            Vector3 up = Vector3CrossProduct(right, Vector3Negate(to_cam));
            Vector3 light = Vector3Normalize(Vector3Add(to_cam, Vector3Add(Vector3Scale(right, -0.55f), Vector3Scale(up, 0.45f))));
            if (gfilter == 0) globe_shade(light);                                                           /* éclairage rasant depuis la caméra */
            else if (gfilter == 1) {                                                                        /* luminosité à la date choisie */
                if (gdirty || prev_gfilter != 1 || gstep != prev_gstep) globe_light_sun(gstep);
            } else {                                                                                        /* topographie */
                if (prev_gfilter != gfilter) globe_fill_topo(gfilter);
                globe_shade_gradient(gfilter, light);
            }
            prev_gfilter = gfilter; prev_gstep = gstep; gdirty = 0;
        }

        /* --- rendu 3D dans la texture --- */
        BeginTextureMode(rt);
        ClearBackground((sm || real_view) ? (Color){4, 6, 10, 255} : C_BG);
        BeginMode3D(cam);
        rlDisableBackfaceCulling();
        if (gm) {
            if (have_globe) {
                DrawMesh(gmesh, gfilter >= 2 ? gmat_plain : gmat, MatrixIdentity());
                for (int i = 0; i < NPOI; i++)
                    if (poi_on[POIS[i].cat]) DrawSphere(Vector3Scale(sph(POIS[i].lat, POIS[i].lon), GLOBE_R * 1.01f), 0.65f, POI_COL[POIS[i].cat]);
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
                if (mode == 3 && year_ready) draw_rank_3d(&rk, eff_exag);
            }
        }
        EndMode3D();
        EndTextureMode();

        /* --- dessin 2D --- */
        BeginDrawing();
        ClearBackground(C_BG);
        rlPushMatrix();
        rlScalef(g_ui_s, g_ui_s, 1.0f);                   /* toute l'interface (textes, boutons, menus) est agrandie d'un coup */
        rlPushMatrix();
        rlTranslatef(0.0f, (float)TOP_H, 0.0f);           /* tout le contenu est décalé sous les barres */
        DrawTexturePro(rt.texture, (Rectangle){0, 0, (float)rt_w, -(float)rt_h}, (Rectangle){view.x, view.y, (float)vw, (float)vh}, (Vector2){0, 0}, 0.0f, WHITE);
        if (gm && have_globe) {
            for (int i = 0; i < npts; i++) {
                Vector3 wp = Vector3Scale(sph(pts[i].lat, pts[i].lon), GLOBE_R * 1.02f);
                if (Vector3DotProduct(Vector3Normalize(wp), Vector3Normalize(cam.position)) > 0.15f) {
                    Vector2 sp = GetWorldToScreenEx(wp, cam, vw, vh);
                    DrawText(pts[i].name, (int)(view.x + sp.x) + 8, (int)sp.y - 8, 14, WHITE);
                }
            }
            if (dist_ok_labels(gdist))
                for (int i = 0; i < NPOI; i++) {
                    if (!poi_on[POIS[i].cat]) continue;
                    Vector3 wp = Vector3Scale(sph(POIS[i].lat, POIS[i].lon), GLOBE_R * 1.01f);
                    if (Vector3DotProduct(Vector3Normalize(wp), Vector3Normalize(cam.position)) < 0.15f) continue;
                    Vector2 sp = GetWorldToScreenEx(wp, cam, vw, vh);
                    int tw = MeasureText(POIS[i].name, 13);
                    DrawRectangle((int)(view.x + sp.x) + 7, (int)sp.y - 8, tw + 6, 16, (Color){16, 21, 28, 190});
                    DrawText(POIS[i].name, (int)(view.x + sp.x) + 10, (int)sp.y - 6, 13, POI_COL[POIS[i].cat]);
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
        if (mode == 3 && site_ready && year_ready) {
            draw_rank_labels(&rk, cam, view, vw, vh, exag);
            draw_rank_table(&rk, ro);
            DrawText("Vert : site valide (plus clair = meilleure note)  |  Rouge : éliminé par un filtre  |  Violet : ombre permanente",
                     (int)view.x + 12, vh - 20, 13, C_MUT);
        }
        if (!gm && !site_ready) {
            DrawText("Site non préparé", (int)view.x + vw / 2 - 70, vh / 2 - 10, 20, C_MUT);
        }
        if (!gm) {
            DrawRectangleRec(zo, (Color){16, 21, 28, 215});
            bool can_zoom = cur_site >= 0 || zone_valid;
            const char *zl = site_ready ? TextFormat("Zone : %s", fmt_dist_m(GW * cell_m)) : TextFormat("Zone : x%.1f", zfac[zlev]);
            DrawText(zl, (int)(zo.x + zo.width / 2) - MeasureText(zl, 16) / 2, (int)zo.y + 7, 16, can_zoom ? C_TXT : C_MUT);
            if (ui_button((Rectangle){zo.x + 4, zo.y + 3, 34, 24}, "-", false) && can_zoom && zlev > 0) { zlev--; pending_load = cur_site; }
            if (ui_button((Rectangle){zo.x + zo.width - 38, zo.y + 3, 34, 24}, "+", false) && can_zoom && zlev < 4) { zlev++; pending_load = cur_site; }
        }
        DrawText(gm ? "Glisser = tourner  |  molette = zoom  |  clic = marquer un point"
                    : (sm ? "Glisser = regarder autour (360°)  |  relief réel x1"
                          : (mode == 3 ? "Clic = centre de la zone de recherche  |  cercle jaune = rayon  |  chiffres = sites candidats"
                                       : "Rouge : pente au-dessus du seuil  |  glisser = tourner, molette = zoom, clic = choisir un site")),
                 (int)view.x + 12, 10, 14, C_MUT);

        /* bouton plein écran (angle inférieur droit de la vue Lune) */
        {
            bool hot = !g_block && CheckCollisionPointRec(m, fsb);
            DrawRectangleRec(fsb, hot ? C_HOT : (Color){16, 21, 28, 215});
            DrawRectangleLinesEx(fsb, 1, hot ? C_SUN : C_LINE);
            Color ic = hot ? C_SUN : C_TXT;
            float cx0 = fsb.x + fsb.width / 2, cy0 = fsb.y + fsb.height / 2;
            for (int a = -1; a <= 1; a += 2)
                for (int b = -1; b <= 1; b += 2) {
                    float px = cx0 + a * (g_moon_fs ? 4.0f : 9.0f), py = cy0 + b * (g_moon_fs ? 4.0f : 9.0f);
                    float dir = g_moon_fs ? 1.0f : -1.0f;                 /* plein écran : bras vers le centre ; réduire : bras vers l'extérieur */
                    DrawLineEx((Vector2){px, py}, (Vector2){px + a * dir * 6.0f, py}, 2.0f, ic);
                    DrawLineEx((Vector2){px, py}, (Vector2){px, py + b * dir * 6.0f}, 2.0f, ic);
                }
            if (hot) {
                const char *tip = g_moon_fs ? "Quitter le plein écran (Échap)" : "Lune en plein écran";
                int tw = MeasureText(tip, 14);
                DrawRectangle((int)(fsb.x - tw - 22), (int)(fsb.y + 8), tw + 12, 20, (Color){16, 21, 28, 235});
                DrawText(tip, (int)(fsb.x - tw - 16), (int)(fsb.y + 11), 14, C_TXT);
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && g_dialog == 0) g_moon_fs = !g_moon_fs;
            }
        }
        /* en plein écran, la frise n'est plus visible : on garde les commandes de temps du Site / Sol */
        if (g_moon_fs && (mode == 1 || mode == 2) && site_ready) {
            float bw = fminf(440.0f, view.width - 160.0f);
            Rectangle bar = {view.x + 12, view.y + view.height - 74, bw, 62};
            DrawRectangleRec(bar, (Color){16, 21, 28, 215});
            DrawText(TextFormat("Date : J+%.1f", t * STEP_H / 24.0f), (int)bar.x + 10, (int)bar.y + 8, 16, C_TXT);
            if (ui_button((Rectangle){bar.x + bar.width - 98, bar.y + 5, 88, 24}, playing ? "Pause" : "Lecture", playing)) playing = !playing;
            ui_slider(bar.x + 10, bar.y + 22, bar.width - 20, "", &time_f, 0, N_STEPS - 1, "");
        }

        /* panneau gauche (masqué quand la vue Lune est en plein écran) */
        if (!g_moon_fs) {
        DrawRectangle(0, 0, PANEL_W, sh, C_PANEL);
        DrawRectangle(PANEL_W - 1, 0, 1, sh, C_LINE);
        float y = 16;
        if (logo_tex.id > 0) {
            float size = 28.0f;                                  /* hauteur du logo en pixels */
            float scale = size / logo_tex.height;
            DrawTextureEx(logo_tex, (Vector2){18, y}, 0.0f, scale, WHITE);
            DrawText("Selene", 18 + (int)(logo_tex.width * scale) + 10, (int)y + 2, 26, C_TXT);
        } else {
            DrawText("Selene", 18, (int)y, 26, C_TXT);
        }
        y += 34;
        if (gm) { DrawText(TextFormat("Lune entière  |  rayon %s", fmt_dist_km(1737.4)), 18, (int)y, 14, C_MUT); y += 26; }
        else if (site_ready) {
            float side = GW * cell_m / 1000.0f;
            DrawText(TextFormat("Carré de %s x %s (%s)", fmt_dist_km(side), fmt_dist_km(side), fmt_area_km2(side * side)), 18, (int)y, 14, C_MUT); y += 18;
            DrawText(TextFormat("Cellule : %s  |  relief x%.1f", fmt_dist_m(cell_m), eff_exag), 18, (int)y, 14, C_MUT); y += 24;
        } else y += 26;

        /* 4 onglets : Globe | Site | Sol | Analyse */
        if (ui_button((Rectangle){18, y, 64, 28}, "Globe", mode == 0) && have_globe) mode = 0;
        if (ui_button((Rectangle){88, y, 64, 28}, "Site", mode == 1)) mode = 1;
        if (ui_button((Rectangle){158, y, 64, 28}, "Sol", sm)) {
            mode = 2;
            if (!site.valid) { site.valid = true; site.x = GW / 2; site.y = GH / 2; }
        }
        if (ui_button((Rectangle){228, y, 64, 28}, "Analyse", mode == 3)) mode = 3;
        y += 40;

        if (gm) {
            if (!have_globe) {
                DrawText("Globe indisponible.", 18, (int)y, 16, C_BAD);
                DrawText("Lancez tools/prepare_globe.py", 18, (int)y + 22, 14, C_MUT);
            } else {
                float y0 = y;                                         /* le panneau défile avec la molette (la liste peut être longue) */
                y -= panel_scroll;
                BeginScissorMode(0, (int)((y0 + TOP_H) * g_ui_s), (int)(PANEL_W * g_ui_s) + 1, GetScreenHeight() - (int)((y0 + TOP_H) * g_ui_s));
                g_clip_top = y0;
                ui_slider(18, y, 274, "Exagération du relief", &gexag, 1, 40, TextFormat("x%.0f", gexag)); y += 50;
                if (ui_button((Rectangle){18, y, 134, 26}, "Pôle Sud", false)) { gpitch = -1.45f; gyaw = 0.0f; }
                if (ui_button((Rectangle){158, y, 134, 26}, "Pôle Nord", false)) { gpitch = 1.45f; gyaw = 0.0f; }
                y += 36;
                DrawRectangle(18, (int)y, 274, 1, C_LINE); y += 10;

                /* --- filtres : luminosité à une date, topographie (altitude, écart à la moyenne, rugosité) --- */
                DrawText("Filtres du globe", 18, (int)y, 16, C_TXT); y += 24;
                if (ui_button((Rectangle){18, y, 134, 26}, "Aucun", gfilter == 0)) gfilter = 0;
                if (globe_eph_ok) { if (ui_button((Rectangle){158, y, 134, 26}, "Luminosité", gfilter == 1)) gfilter = 1; }
                else {                                                /* grisé : il faut un site préparé (éphémérides) */
                    DrawRectangleRec((Rectangle){158, y, 134, 26}, C_BTN);
                    DrawText("Luminosité", 158 + (134 - MeasureText("Luminosité", 16)) / 2, (int)y + 5, 16, C_MUT);
                }
                y += 30;
                if (ui_button((Rectangle){18, y, 134, 26}, "Altitude", gfilter == 2)) gfilter = 2;
                if (ui_button((Rectangle){158, y, 134, 26}, "Écart moyenne", gfilter == 3)) gfilter = 3;
                y += 30;
                if (ui_button((Rectangle){18, y, 134, 26}, "Rugosité", gfilter == 4)) gfilter = 4;
                y += 34;
                if (gfilter == 1) {
                    ui_slider(18, y, 274, "Date", &gday, 0.0f, 365.0f, date_label(gday)); y += 40;
                    if (ui_button((Rectangle){18, y, 134, 24}, gplay ? "Pause" : "Lecture", gplay)) gplay = !gplay;
                    y += 32;
                    DrawText("Phase de la Lune à cette date", 18, (int)y, 13, C_MUT); y += 18;
                    draw_gradient_legend(18, (int)y, 274, 1, "nuit", "plein jour"); y += 36;
                } else if (gfilter >= 2) {
                    DrawText(TextFormat("Moyenne : %s   |   écart-type : %s", fmt_alt_m(g_topo_mean), fmt_alt_m(g_topo_std)), 18, (int)y, 13, C_TXT); y += 18;
                    DrawText(TextFormat("Minimum : %s   |   maximum : %s", fmt_alt_m(g_topo_min), fmt_alt_m(g_topo_max)), 18, (int)y, 13, C_MUT); y += 22;
                    if (gfilter == 2) {
                        DrawText("Altitude (échelle : moyenne ± 3 écarts-types)", 18, (int)y, 13, C_MUT); y += 18;
                        draw_gradient_legend(18, (int)y, 274, 2, fmt_alt_m(g_topo_lo), fmt_alt_m(g_topo_hi));
                    } else if (gfilter == 3) {
                        DrawText("Écart à l'altitude moyenne, en écarts-types", 18, (int)y, 13, C_MUT); y += 18;
                        draw_gradient_legend(18, (int)y, 274, 3, "-3 (bas)", "+3 (haut)");
                        DrawText("moyenne", 18 + 137 - MeasureText("moyenne", 12) / 2, (int)y + 13, 12, C_MUT);
                    } else {
                        DrawText("Rugosité : écart-type local des altitudes", 18, (int)y, 13, C_MUT); y += 18;
                        draw_gradient_legend(18, (int)y, 274, 4, "lisse", fmt_alt_m(g_rough_hi));
                    }
                    y += 36;
                }

                /* --- points d'intérêt --- */
                DrawText("Points d'intérêt", 18, (int)y, 16, C_TXT); y += 24;
                for (int c = 0; c < 3; c++)
                    if (ui_button((Rectangle){18 + c * 94.0f, y, 86, 26}, POI_CAT[c], poi_on[c])) poi_on[c] = !poi_on[c];
                y += 30;
                if (poi_on[0] || poi_on[1] || poi_on[2]) { DrawText("Zoomez pour voir les noms, cliquez pour marquer.", 18, (int)y, 12, C_MUT); y += 18; }
                y += 6;
                DrawRectangle(18, (int)y, 274, 1, C_LINE); y += 10;

                /* --- point marqué --- */
                if (gpick_valid) {
                    int gi = (int)Clamp(roundf((gpick_lon + 180.0f) / 360.0f * GLON), 0, GLON);
                    int gj = (int)Clamp(roundf((90.0f - gpick_lat) / 180.0f * GLAT), 0, GLAT);
                    DrawText(gpick_name[0] ? TextFormat("Point marqué : %s", gpick_name) : "Point marqué", 18, (int)y, 16, C_TXT); y += 22;
                    DrawText(TextFormat("lat %.3f   lon %.3f", gpick_lat, gpick_lon), 18, (int)y, 14, C_MUT); y += 20;
                    DrawText(TextFormat("Altitude : %s", fmt_alt_m(g_h[gj * (GLON + 1) + gi])), 18, (int)y, 14, C_MUT); y += 24;
                    if (ui_button((Rectangle){18, y, 274, 28}, "Enregistrer ce point", false)) {
                        if (npts < MAX_PTS) {
                            snprintf(pts[npts].id, sizeof pts[npts].id, "P%d", next_id++);
                            if (gpick_name[0]) snprintf(pts[npts].name, sizeof pts[npts].name, "%s", gpick_name);
                            else snprintf(pts[npts].name, sizeof pts[npts].name, "%s", pts[npts].id);
                            pts[npts].lat = gpick_lat; pts[npts].lon = gpick_lon;
                            npts++;
                            save_points();
                            status_msg[0] = 0;
                        } else snprintf(status_msg, sizeof status_msg, TR("Maximum de %d points atteint"), MAX_PTS);
                    }
                    y += 34;
                } else {
                    DrawText("Cliquez sur la Lune pour marquer un point.", 18, (int)y, 14, C_MUT); y += 24;
                }
                if (ui_button((Rectangle){18, y, 274, 26}, "Saisir des coordonnées...", false)) {
                    g_ci[0][0] = g_ci[1][0] = g_ci[2][0] = 0; g_ci_focus = 0; g_ci_err[0] = 0; g_dialog = 6;
                }
                y += 36;

                /* --- points enregistrés --- */
                DrawText("Points enregistrés", 18, (int)y, 16, C_TXT);
                {
                    Rectangle cb = {188, y - 3, 104, 22};
                    if (npts > 0) { if (ui_button(cb, "Tout effacer", false)) g_dialog = 5; }
                    else {                                           /* grisé : rien à effacer */
                        DrawRectangleRec(cb, C_BTN);
                        DrawText("Tout effacer", (int)(cb.x + (cb.width - MeasureText("Tout effacer", 16)) / 2), (int)y, 16, C_MUT);
                    }
                }
                y += 24;
                if (npts == 0) { DrawText("Aucun pour l'instant.", 18, (int)y, 14, C_MUT); y += 22; }
                else { DrawText("(cliquez sur un nom pour le modifier)", 18, (int)y, 12, C_MUT); y += 18; }
                int del = -1;
                for (int i = 0; i < npts; i++) {
                    Rectangle nr = {18, y, 150, 24};
                    if (edit_idx == i) {                                  /* champ de saisie, comme un renommage de fichier */
                        edit_rect = nr;
                        DrawRectangleRec(nr, C_BG);
                        DrawRectangleLinesEx(nr, 1, C_SUN);
                        DrawText(TextFormat("%s%s", edit_buf, ((int)(GetTime() * 2.0) % 2) ? "|" : ""), 24, (int)y + 5, 14, C_TXT);
                    } else {
                        bool hot = !g_block && m.y >= g_clip_top && CheckCollisionPointRec(m, nr);
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
                    y += 22;
                }
                if (status_msg[0]) { DrawText(TextSubtext(status_msg, 0, 40), 18, (int)y, 14, C_BAD); y += 20; }
                y += 6;
                DrawText("Relief LOLA et couleurs LRO (NASA)", 18, (int)y, 13, C_MUT); y += 22;
                EndScissorMode();
                g_clip_top = 0.0f;
                panel_max = fmaxf(0.0f, (y + panel_scroll) - (sh - 12));
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
            } else if (mode == 3) {
                if (!have_frame || !year_loaded) {
                    DrawText("Classement indisponible.", 18, (int)y, 16, C_TXT); y += 26;
                    DrawText("Ce site a une ancienne préparation.", 18, (int)y, 14, C_MUT); y += 20;
                    DrawText("Il faut les fichiers année + repères.", 18, (int)y, 14, C_MUT); y += 28;
                    if ((cur_site >= 0 || zone_valid) && ui_button((Rectangle){18, y, 274, 30}, "Re-préparer ce site", false)) pending_prepare = cur_site;
                } else if (!year_ready) {
                    DrawText("Analyse annuelle requise (~ 5 s).", 18, (int)y, 16, C_TXT); y += 28;
                    DrawText("Soleil et Terre vus de chaque case,", 18, (int)y, 14, C_MUT); y += 18;
                    DrawText("sur une année entière, relief inclus.", 18, (int)y, 14, C_MUT); y += 30;
                    if (ui_button((Rectangle){18, y, 274, 30}, "Analyser l'année", false)) pending_year = 1;
                } else {
                    float y0 = y;
                    y -= panel_scroll;
                    BeginScissorMode(0, (int)((y0 + TOP_H) * g_ui_s), (int)(PANEL_W * g_ui_s) + 1, GetScreenHeight() - (int)((y0 + TOP_H) * g_ui_s));
                    g_clip_top = y0;
                    float side = GW * cell_m / 1000.0f;
                    if (rk.start_day > 365.0f - rk.days) rk.start_day = 365.0f - rk.days;
                    if (rk.start_day < 0.0f) rk.start_day = 0.0f;
                    if (rk.rad_km > 0.5f * side) rk.rad_km = 0.5f * side;
                    ui_slider(18, y, 274, "Rayon de recherche", &rk.rad_km, 0.5f, 0.5f * side, fmt_dist_km(rk.rad_km)); y += 40;
                    ui_slider(18, y, 274, "Durée de la mission", &rk.days, 2.0f, 30.0f, TextFormat("%.0f j", rk.days)); y += 40;
                    ui_slider(18, y, 274, "Début de la mission", &rk.start_day, 0.0f, 365.0f - rk.days, date_label(rk.start_day)); y += 42;
                    if (ui_button((Rectangle){18, y, 86, 26}, "Survie", false)) { float v[5] = {10, 3, 10, 4, 1}; memcpy(rk.w, v, sizeof v); }
                    if (ui_button((Rectangle){112, y, 86, 26}, "Glace", false)) { float v[5] = {3, 3, 2, 9, 10}; memcpy(rk.w, v, sizeof v); }
                    if (ui_button((Rectangle){206, y, 86, 26}, "Relais", false)) { float v[5] = {4, 10, 2, 5, 1}; memcpy(rk.w, v, sizeof v); }
                    y += 32;
                    if (ui_button((Rectangle){18, y, 274, 24}, rk.relay ? "Relais disponible : oui" : "Relais disponible : non", rk.relay)) rk.relay = !rk.relay;
                    y += 32;
                    if (ui_button((Rectangle){18, y, 134, 24}, "Priorités", rk.tab == 0)) rk.tab = 0;
                    if (ui_button((Rectangle){158, y, 134, 24}, "Contraintes", rk.tab == 1)) rk.tab = 1;
                    y += 32;
                    if (rk.tab == 0) {
                        ui_slider(18, y, 274, "Énergie (Soleil)", &rk.w[0], 0, 10, TextFormat("%.0f", rk.w[0])); y += 38;
                        ui_slider(18, y, 274, "Communication (Terre)", &rk.w[1], 0, 10, TextFormat("%.0f", rk.w[1])); y += 38;
                        ui_slider(18, y, 274, "Stabilité thermique*", &rk.w[2], 0, 10, TextFormat("%.0f", rk.w[2])); y += 38;
                        ui_slider(18, y, 274, "Sécurité du terrain", &rk.w[3], 0, 10, TextFormat("%.0f", rk.w[3])); y += 38;
                        ui_slider(18, y, 274, "Intérêt scientifique (glace)", &rk.w[4], 0, 10, psr_count ? TextFormat("%.0f", rk.w[4]) : "ignoré"); y += 38;
                        if (psr_count == 0) { DrawText("Aucune ombre permanente dans cette zone.", 18, (int)y - 6, 12, C_MUT); y += 12; }
                    } else {
                        ui_slider(18, y, 274, "Pente maximale", &rk.smax, 2, 25, TextFormat("%.0f°", rk.smax)); y += 38;
                        ui_slider(18, y, 274, "Rugosité maximale", &rk.sigma, 2, 60, fmt_alt_m(rk.sigma)); y += 38;
                        ui_slider(18, y, 274, "Nuit continue maximale", &rk.night, 24, 400, TextFormat("%.0f h", rk.night)); y += 38;
                        ui_slider(18, y, 274, "Ellipse d'atterrissage", &rk.ell, 0, 1000, fmt_dist_m(rk.ell)); y += 38;
                        ui_slider(18, y, 274, "Distance min. entre sites", &rk.excl_km, 0.5f, 5, fmt_dist_km(rk.excl_km)); y += 38;
                    }
                    if (ui_button((Rectangle){18, y, 274, 28}, "Meilleure date pour cette zone", false)) {
                        ensure_terrain(rk.ell);
                        Params Pb = rk_params(&rk);
                        int bd = best_start_region(&Pb, rk.cx, rk.cy, rk.rad_km * 1000.0);
                        if (bd >= 0) { rk.start_day = bd / 4.0f; rk.msg[0] = 0; }
                        else snprintf(rk.msg, sizeof rk.msg, "%s", TR("Aucune date valide avec ces contraintes (voir ci-dessous)."));
                    }
                    y += 34;
                    if (rk.msg[0]) { DrawText(TextSubtext(rk.msg, 0, 42), 18, (int)y, 13, C_BAD); y += 16; }
                    y += 4;
                    DrawText("Résultats", 18, (int)y, 16, C_TXT); y += 22;
                    if (st_eval > 0) {
                        DrawText(TextFormat("Cases valides à cette date : %.0f %%", 100.0f * st_valid / st_eval), 18, (int)y, 13, st_valid ? C_OK : C_BAD);
                        y += 18;
                    }
                    if (st_eval > 0 && st_valid == 0) {
                        DrawText("Aucun site valide à cette date.", 18, (int)y, 14, C_BAD); y += 18;
                        DrawText("(le tableau montre les meilleurs sites éliminés)", 18, (int)y, 12, C_MUT); y += 18;
                        static const char *why[4] = {"pente trop forte", "terrain trop rugueux", "Terre jamais visible", "nuit trop longue"};
                        int worst = -1;
                        float wmax = 0.0f;
                        for (int b = 0; b < 4; b++) {
                            float pc = st_eval ? 100.0f * st_fail[b] / st_eval : 0.0f;
                            if (pc >= 0.5f) { DrawText(TextFormat("%s : %.0f %% des cases", TR(why[b]), pc), 18, (int)y, 13, C_MUT); y += 16; }
                            if (pc > wmax) { wmax = pc; worst = b; }
                        }
                        if (worst == 3 && rk.days > 14.0f) { DrawText("Au-delà de 15 j, une nuit lunaire (~350 h)", 18, (int)y, 13, C_MUT); y += 15;
                                                             DrawText("est inévitable : réduisez la durée ou la limite.", 18, (int)y, 13, C_MUT); y += 18; }
                        y += 4;
                        if (worst == 0 && ui_button((Rectangle){18, y, 274, 26}, "Autoriser pente jusqu'à 25°", false)) rk.smax = 25.0f;
                        if (worst == 1 && ui_button((Rectangle){18, y, 274, 26}, TextFormat("Autoriser rugosité jusqu'à %s", fmt_alt_m(60)), false)) rk.sigma = 60.0f;
                        if (worst == 2 && ui_button((Rectangle){18, y, 274, 26}, "Activer le relais de communication", false)) rk.relay = 1;
                        if (worst == 3 && ui_button((Rectangle){18, y, 274, 26}, "Autoriser nuit jusqu'à 400 h", false)) rk.night = 400.0f;
                        if (worst >= 0) y += 34;
                    }
                    DrawText("Le classement et les notes sont dans le", 18, (int)y + 2, 12, C_MUT); y += 14;
                    DrawText("tableau en haut à gauche de la vue 3D.", 18, (int)y + 2, 12, C_MUT); y += 18;
                    DrawText(rk.from_cache ? "Analyse relue depuis le disque (rapide)." : "Analyse calculée puis enregistrée sur le disque.", 18, (int)y, 12, C_MUT); y += 22;
                    if (ui_button((Rectangle){18, y, 274, 28}, "Exporter en CSV", false)) export_csv(&rk, cur_site, zlev, rk.exp_msg, sizeof rk.exp_msg);
                    y += 32;
                    if (ui_button((Rectangle){18, y, 164, 24}, g_csv_fr ? "Format : Excel FR (;)" : "Format : international (,)", false)) g_csv_fr = !g_csv_fr;
                    if (ui_button((Rectangle){188, y, 104, 24}, "Ouvrir dossier", false)) { MKDIR("exports"); sln_open("exports"); }
                    y += 30;
                    if (rk.exp_msg[0]) { DrawText(TextSubtext(rk.exp_msg, 0, 42), 18, (int)y, 12, C_OK); y += 16; }
                    EndScissorMode();
                    g_clip_top = 0.0f;
                    panel_max = fmaxf(0.0f, (y + panel_scroll) - (sh - 12));
                }
            } else {
                if (!sm) {
                    DrawText("Couche affichée", 18, (int)y, 16, C_TXT); y += 22;
                    for (int i = 0; i < 5; i++) {
                        if (ui_button((Rectangle){18, y, 274, 22}, layer_names[i], layer == i)) layer = i;
                        y += 26;
                    }
                    if (layer == 4) {
                        ui_slider(18, y + 4, 274, "Exposition", &rexp, 2.0f, 40.0f, TextFormat("x%.0f", rexp)); y += 44;
                        DrawText("Soleil à sa position réelle, ombres portées", 18, (int)y, 12, C_MUT); y += 15;
                        DrawText("du relief, relief réel x1 (sans exagération).", 18, (int)y, 12, C_MUT); y += 18;
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
                if (!sm && layer != 4) {
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
                        DrawText(TextFormat("Altitude : %s   Pente : %.1f°", fmt_alt_m(dem[k]), slope_deg[k]), 18, (int)y, 14, C_MUT); y += 20;
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
        } /* fin du panneau gauche */
        /* frise annuelle du site sélectionné (onglet Classer) */
        if (!g_moon_fs && mode == 3 && site_ready && year_ready) {
            DrawRectangleRec(tl, C_PANEL);
            DrawRectangle((int)tl.x, (int)tl.y, (int)tl.width, 1, C_LINE);
            int kk = (rk.sel >= 0 && rk.sel < rk.n) ? rk.c[rk.sel].k : rk.cy * GW + rk.cx;
            float L = tl.x + 70, plotW = tl.width - 70 - 14, laneH = (tl.height - 16 - 30 - 10) / 2;
            float ly1 = tl.y + 16, ly2 = ly1 + laneH + 10;
            DrawRectangle((int)L, (int)ly1, (int)plotW, (int)laneH, C_SHADE);
            DrawRectangle((int)L, (int)ly2, (int)plotW, (int)laneH, C_SHADE);
            DrawText("Soleil", (int)tl.x + 12, (int)(ly1 + laneH / 2 - 8), 16, C_MUT);
            DrawText("Terre", (int)tl.x + 12, (int)(ly2 + laneH / 2 - 8), 16, C_MUT);
            for (int i = 0; i < NY; i++) {
                float x0 = L + plotW * i / NY, w = plotW / NY + 1.0f;
                uint8_t su = ysun[(size_t)kk * NY + i], ea = yearth[(size_t)kk * NY + i];
                if (su) DrawRectangleRec((Rectangle){x0, ly1, w, laneH}, ColorAlpha(C_SUN, su / 255.0f));
                if (ea) DrawRectangleRec((Rectangle){x0, ly2, w, laneH}, ColorAlpha(C_EARTH, ea / 255.0f));
            }
            for (int d = 0; d <= 365; d += 61) {
                float x = L + plotW * d / 365.25f;
                DrawRectangle((int)x, (int)(tl.y + tl.height - 30), 1, 5, C_MUT);
                DrawText(date_label((float)d), (int)x - 28, (int)(tl.y + tl.height - 22), 12, C_MUT);
            }
            float xs = L + plotW * rk.start_day / 365.25f, ww = plotW * rk.days / 365.25f;
            DrawRectangleLinesEx((Rectangle){xs, ly1 - 4, ww < 3 ? 3 : ww, 2 * laneH + 18}, 2, WHITE);
            DrawText(TextFormat("Année complète, site %d  |  fenêtre de mission en blanc  |  clic = choisir le début", rk.sel >= 0 ? rk.sel + 1 : 0),
                     (int)L, (int)tl.y + 1, 12, C_MUT);
            if (CheckCollisionPointRec(m, tl) && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && g_active == NULL && g_block == false) {
                float f = (m.x - L) / plotW * 365.25f - rk.days / 2.0f;
                if (m.x >= L && m.x <= L + plotW) rk.start_day = Clamp(f, 0.0f, 365.0f - rk.days);
            }
        }

        /* recalcul du classement si une entrée a changé (la stabilité n'est recalculée qu'au relâchement du curseur) */
        if (mode == 3 && site_ready && year_ready) {
            rk.cx = site.valid ? site.x : GW / 2;
            rk.cy = site.valid ? site.y : GH / 2;
            float sg[16];
            rk_signature(&rk, sg);
            int released = (prev_active != NULL && g_active == NULL);
            if (rk_force || released || memcmp(sg, rk.sig, sizeof sg) != 0) {
                if (memcmp(sg, rk.sig, sizeof sg) != 0) rk.msg[0] = 0;
                memcpy(rk.sig, sg, sizeof sg);
                rk_update(&rk, g_active == NULL);
                rk_force = 0;
                force_recolor = 1;
            }
        }
        prev_active = g_active;
        if (rk.save_req >= 0) {
            int i = rk.save_req;
            rk.save_req = -1;
            if (i < rk.n && npts < MAX_PTS) {
                snprintf(pts[npts].id, sizeof pts[npts].id, "P%d", next_id++);
                snprintf(pts[npts].name, sizeof pts[npts].name, "Cand%d", i + 1);
                pts[npts].lat = rk.c[i].lat; pts[npts].lon = rk.c[i].lon;
                npts++;
                save_points();
                snprintf(rk.note, sizeof rk.note, TR("Site %d enregistré : point %s (voir l'onglet Globe)."), i + 1, pts[npts - 1].id);
            } else snprintf(rk.note, sizeof rk.note, TR("Impossible : %d points enregistrés au maximum."), MAX_PTS);
        }
        rlPopMatrix();
        SetMouseOffset(0, 0);                              /* barre de menus, notifications et dialogues : coordonnées d'écran réelles */

        MMenu menus[5];
        memset(menus, 0, sizeof menus);
        int can_site = cur_site >= 0, can_prep = cur_site >= 0 || zone_valid, can_year = have_frame && year_loaded;
        menus[0].title = "Fichier";
        mi(&menus[0], "Nouveau point (coordonnées)...", "Ctrl+N", A_NEWPT, have_globe);
        mi(&menus[0], "-", NULL, A_NONE, 0);
        mi(&menus[0], "Exporter le classement en CSV", "Ctrl+E", A_EXPORT, mode == 3 && site_ready && year_ready);
        mi(&menus[0], "Ouvrir le dossier d'export", NULL, A_OPEN_EXPORTS, 1);
        mi(&menus[0], "Ouvrir le dossier des données", NULL, A_OPEN_DATA, 1);
        mi(&menus[0], "-", NULL, A_NONE, 0);
        mi(&menus[0], "Quitter", "Alt+F4", A_QUIT, 1);
        menus[1].title = "Édition";
        mi(&menus[1], "Renommer le point actuel", "F2", A_RENAME, can_site);
        mi(&menus[1], "Supprimer le point actuel", NULL, A_DELETE, can_site);
        mi(&menus[1], "Supprimer tous les points...", NULL, A_CLEAR_ALL, npts > 0);
        mi(&menus[1], "-", NULL, A_NONE, 0);
        mi(&menus[1], "Réinitialiser les réglages du classement", NULL, A_RESET_RK, 1);
        menus[2].title = "Affichage";
        mi(&menus[2], "Globe", "Ctrl+1", A_TAB0, have_globe);
        mi(&menus[2], "Site", "Ctrl+2", A_TAB1, 1);
        mi(&menus[2], "Sol (vue à 360°)", "Ctrl+3", A_TAB2, 1);
        mi(&menus[2], "Analyse", "Ctrl+4", A_TAB3, 1);
        mi(&menus[2], "-", NULL, A_NONE, 0);
        mi(&menus[2], "Zone plus grande", NULL, A_ZOOMIN, can_prep && zlev < 4);
        mi(&menus[2], "Zone plus petite", NULL, A_ZOOMOUT, can_prep && zlev > 0);
        mi(&menus[2], "-", NULL, A_NONE, 0);
        mi(&menus[2], "Plein écran", "F11", A_FULLSCREEN, 1);
        mi(&menus[2], g_moon_fs ? "Quitter la Lune en plein écran" : "Lune en plein écran", "Échap", A_MOONFS, 1);
        mi(&menus[2], "-", NULL, A_NONE, 0);
        mi(&menus[2], "Interface plus grande", "Ctrl++", A_UI_BIGGER, 1);
        mi(&menus[2], "Interface plus petite", "Ctrl+-", A_UI_SMALLER, 1);
        mi(&menus[2], "Échelle automatique", "Ctrl+0", A_UI_AUTO, 1);
        menus[3].title = "Outils";
        mi(&menus[3], "Préparer / re-préparer le site actuel", NULL, A_PREPARE, can_prep);
        mi(&menus[3], "Analyser l'année (onglet Analyse)", NULL, A_ANALYZE, can_year);
        mi(&menus[3], "Oublier l'analyse enregistrée du site", NULL, A_CLEAR_CACHE, 1);
        mi(&menus[3], "-", NULL, A_NONE, 0);
        mi(&menus[3], g_csv_fr ? "Format CSV : Excel FR (passer en international)" : "Format CSV : international (passer en Excel FR)", NULL, A_CSV_FMT, 1);
        mi(&menus[3], "-", NULL, A_NONE, 0);
        mi(&menus[3], "Préférences (langue, unités)...", "Ctrl+,", A_PREFS, 1);
        mi(&menus[3], "Vérifier l'installation...", NULL, A_DIAG, 1);
        menus[4].title = "Aide";
        mi(&menus[4], "Guide d'utilisation", "F1", A_HELP, 1);
        mi(&menus[4], "Raccourcis clavier", NULL, A_SHORTCUTS, 1);
        mi(&menus[4], "-", NULL, A_NONE, 0);
        mi(&menus[4], "À propos de Selene", NULL, A_ABOUT, 1);
        int chosen_act = menu_bar(sw, menus, 5, g_dialog == 0);
        if (chosen_act != A_NONE) menu_act_prev = chosen_act;

        if (GetTime() < g_toast_until) {                    /* notification temporaire en bas de la fenêtre */
            int tw = MeasureText(g_toast, 16) + 36;
            Rectangle tb = {(float)(sw - tw) / 2 + (g_moon_fs ? 0 : PANEL_W) / 2.0f, (float)sh_total - 52, (float)tw, 34};
            DrawRectangleRec(tb, (Color){16, 21, 28, 240});
            DrawRectangleLinesEx(tb, 1, C_SUN);
            DrawText(g_toast, (int)tb.x + 18, (int)tb.y + 9, 16, C_TXT);
        }
        if (g_dialog && draw_dialog(sw, sh_total)) g_dialog = 0;
        draw_busy(sw, sh_total);
        rlPopMatrix();
        EndDrawing();
    }
    if (logo_tex.id > 0) UnloadTexture(logo_tex);
    CloseWindow();
    return 0;
}