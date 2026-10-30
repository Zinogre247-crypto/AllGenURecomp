// [AllGenU-Android] Mandos tactiles: dibujo, editor y ajustes en pantalla.
//
// Este archivo NO conoce ImGui (el resto del runtime es C, no C++). Expone datos
// planos y el overlay C++ (runtime/overlay.cpp) los pinta con ImGui.
//
//   android_touch_zones()      zonas activas  -> se dibujan sobre el juego
//   android_touch_label()      texto de cada zona
//   android_touch_edit_draw()  panel del editor (mover / tamano / opacidad)
//
// Todo se guarda en:  files/touch_layout.txt  (se relee al arrancar)
// Con ALLGENU_TOUCH=0 o el interruptor del menu, no se dibuja ni se toca nada.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <SDL3/SDL.h>

enum { Z_NONE = 0, Z_BUTTON, Z_STICK, Z_DPAD, Z_CAMERA };
#define MAX_ZONES 48

// Bits de boton, en el orden de host_input.c (BTN_*)
enum { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LS, BTN_RS, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN };
#define BTN(b) (1ull << (b))

typedef struct {
    int kind;
    int btn;
    int stick;         // 0 = izquierdo, 1 = derecho
    float x, y, r, h;  // normalizado a la pantalla (0..1)
    char label[12];
} Zone;

static Zone zones[MAX_ZONES];
static int nzones;

static bool t_visible = true;      // dibujar y usar
static bool t_edit;                // editor abierto
static float t_opacity = 0.42f;    // opacidad del dibujo (0.1 .. 1.0)
static int t_sel = -1;             // zona seleccionada en el editor
static float t_scale_all = 1.0f;   // multiplicador de radio de todas las zonas
static bool t_snap_grid = true;    // imantar al 1 % de la pantalla

static uint64_t t_buttons;         // botones pulsados
static int t_lx, t_ly, t_rx, t_ry; // sticks (-32767..32767)

static const char *const k_names[16] = {"A",  "B",   "X",     "Y",     "LSTICK", "RSTICK", "L",     "R",
                                        "ZL", "ZR",  "PLUS",  "MINUS", "LEFT",   "UP",     "RIGHT", "DOWN"};

static int button_bit(const char *name) {
    for (int i = 0; i < 16; i++)
        if (!strcmp(name, k_names[i])) return i;
    return -1;
}

// ------------------------------------------------------------------ archivo
// Busca el layout: ALLGENU_TOUCH_LAYOUT, la carpeta de datos de la app, la del
// ejecutable y assets/.
static bool writable_path(char *out, size_t n) {
    const char *env = getenv("ALLGENU_TOUCH_LAYOUT");
    if (env && *env) { snprintf(out, n, "%s", env); return true; }
    // Android: /data/data/<pkg>/files/touch_layout.txt  (cwd suele ser la carpeta de la app)
    snprintf(out, n, "touch_layout.txt");
    return true;
}

static FILE *open_layout(void) {
    char p[512];
    const char *env = getenv("ALLGENU_TOUCH_LAYOUT");
    if (env && *env) {
        FILE *f = fopen(env, "r");
        if (f) return f;
    }
    const char *files[] = {"touch_layout.txt", "files/touch_layout.txt", "assets/touch_layout.txt",
                           "../assets/touch_layout.txt"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        FILE *f = fopen(files[i], "r");
        if (f) return f;
    }
    snprintf(p, sizeof p, "touch_layout.txt");
    return NULL;
}

static void add_zone(int kind, int btn, int stick, float x, float y, float r, float h, const char *label) {
    if (nzones >= MAX_ZONES) return;
    Zone *z = &zones[nzones++];
    z->kind = kind, z->btn = btn, z->stick = stick, z->x = x, z->y = y, z->r = r, z->h = h;
    snprintf(z->label, sizeof z->label, "%s", label ? label : "");
}

// Layout por defecto (mando Switch girado): se usa si no hay archivo.
static void defaults(void) {
    add_zone(Z_BUTTON, BTN_A, 0, 0.880f, 0.800f, 0.055f, 0, "A");
    add_zone(Z_BUTTON, BTN_B, 0, 0.790f, 0.885f, 0.055f, 0, "B");
    add_zone(Z_BUTTON, BTN_X, 0, 0.790f, 0.715f, 0.055f, 0, "X");
    add_zone(Z_BUTTON, BTN_Y, 0, 0.700f, 0.800f, 0.055f, 0, "Y");
    add_zone(Z_BUTTON, BTN_L, 0, 0.070f, 0.120f, 0.050f, 0, "L");
    add_zone(Z_BUTTON, BTN_R, 0, 0.190f, 0.120f, 0.050f, 0, "R");
    add_zone(Z_BUTTON, BTN_ZL, 0, 0.070f, 0.260f, 0.050f, 0, "ZL");
    add_zone(Z_BUTTON, BTN_ZR, 0, 0.190f, 0.260f, 0.050f, 0, "ZR");
    add_zone(Z_BUTTON, BTN_PLUS, 0, 0.955f, 0.550f, 0.035f, 0, "PLUS");
    add_zone(Z_BUTTON, BTN_MINUS, 0, 0.045f, 0.550f, 0.035f, 0, "MINUS");
    add_zone(Z_STICK, 0, 0, 0.150f, 0.760f, 0.110f, 0, "L");
    add_zone(Z_STICK, 0, 1, 0.620f, 0.830f, 0.090f, 0, "R");
    add_zone(Z_DPAD, 0, 0, 0.300f, 0.860f, 0.075f, 0, "D");
}

// Lee "Enabled = On/Off" de la seccion [Touch] del ini (lo escribe el menu).
static bool ini_touch_enabled(void) {
    const char *paths[] = {"allgenurecomp.ini", "files/allgenurecomp.ini", "../allgenurecomp.ini"};
    for (unsigned i = 0; i < sizeof paths / sizeof *paths; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        char line[256];
        bool in = false, val = true;
        while (fgets(line, sizeof line, f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;
            if (*s == '[') { in = !strncmp(s + 1, "Touch", 5); continue; }
            if (!in) continue;
            if (!strncasecmp(s, "Enabled", 7)) {
                char *e = strchr(s, '=');
                if (e) val = !(strstr(e, "Off") || strstr(e, "off") || strstr(e, "0"));
            }
        }
        fclose(f);
        return val;
    }
    return true;
}

void android_touch_init(void) {
    const char *off = getenv("ALLGENU_TOUCH");
    if (off && (!strcmp(off, "0") || !strcmp(off, "off"))) t_visible = false;
    else if (!ini_touch_enabled()) t_visible = false;

    nzones = 0;
    FILE *f = open_layout();
    char line[160];
    if (!f) {
        defaults();
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || !*p) continue;
        char tipo[16], id[16];
        float x, y, r, h = 0;
        int n = sscanf(p, "%15s %15s %f %f %f %f", tipo, id, &x, &y, &r, &h);
        if (n < 5) continue;
        if (!strcmp(tipo, "button"))
            add_zone(Z_BUTTON, button_bit(id), 0, x, y, r, 0, id);
        else if (!strcmp(tipo, "stick"))
            add_zone(Z_STICK, 0, !strcmp(id, "R"), x, y, r, 0, id);
        else if (!strcmp(tipo, "dpad"))
            add_zone(Z_DPAD, 0, 0, x, y, r, h > 0 ? h : r, id);
        else if (!strcmp(tipo, "camera"))
            add_zone(Z_CAMERA, 0, 1, x, y, r, h > 0 ? h : r, id);
    }
    fclose(f);
    if (!nzones) defaults();
}

// Guarda el layout actual (lo llama el boton "GUARDAR" del editor).
bool android_touch_save(void) {
    char p[512];
    writable_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return false;
    fprintf(f, "# AllGenURecomp: mandos tactiles (generado por el editor)\n");
    fprintf(f, "# tipo  id  x  y  r  [h]   ->  coordenadas 0..1 sobre la pantalla\n");
    for (int i = 0; i < nzones; i++) {
        Zone *z = &zones[i];
        switch (z->kind) {
        case Z_BUTTON: fprintf(f, "button %-6s %.4f %.4f %.4f\n", z->label, z->x, z->y, z->r); break;
        case Z_STICK: fprintf(f, "stick  %-6s %.4f %.4f %.4f\n", z->stick ? "R" : "L", z->x, z->y, z->r); break;
        case Z_DPAD: fprintf(f, "dpad   D      %.4f %.4f %.4f %.4f\n", z->x, z->y, z->r, z->h); break;
        case Z_CAMERA: fprintf(f, "camera R      %.4f %.4f %.4f %.4f\n", z->x, z->y, z->r, z->h); break;
        }
    }
    fclose(f);
    fprintf(stderr, "[touch] layout guardado en %s\n", p);
    return true;
}

// ------------------------------------------------------------------ entrada
uint64_t android_touch_buttons(void) { return t_visible ? t_buttons : 0; }

void android_touch_sticks(int *lx, int *ly, int *rx, int *ry) {
    if (!t_visible) { *lx = *ly = *rx = *ry = 0; return; }
    *lx = t_lx, *ly = t_ly, *rx = t_rx, *ry = t_ry;
}

bool android_touch_visible(void) { return t_visible; }
void android_touch_set_visible(bool v) { t_visible = v; }
bool android_touch_edit(void) { return t_edit; }
void android_touch_set_edit(bool v) { t_edit = v; }

typedef struct { SDL_FingerID id; float x, y; } Finger;
static Finger prev[16];
static int nprev;

static const Finger *find_prev(SDL_FingerID id) {
    for (int i = 0; i < nprev; i++)
        if (prev[i].id == id) return &prev[i];
    return NULL;
}

// Zona bajo (x,y) en coordenadas normalizadas. -1 si ninguna.
static int zone_at(float x, float y) {
    for (int i = 0; i < nzones; i++) {
        Zone *z = &zones[i];
        if (z->kind == Z_BUTTON || z->kind == Z_STICK) {
            float dx = x - z->x, dy = y - z->y;
            if (dx * dx + dy * dy <= z->r * z->r) return i;
        } else {  // dpad / camera: rectangulo
            float hw = z->r, hh = z->h > 0 ? z->h : z->r;
            if (fabsf(x - z->x) <= hw && fabsf(y - z->y) <= hh) return i;
        }
    }
    return -1;
}

// Una vez por frame: lee los dedos de SDL y actualiza botones/sticks.
void ainput_poll(void) {
    if (!t_visible || t_edit) {  // (en el editor los dedos mueven zonas, no el juego)
        t_buttons = 0, t_lx = t_ly = t_rx = t_ry = 0;
        return;
    }
    SDL_Finger **fingers = NULL;
    int n = SDL_GetTouchFingers(SDL_GetPrimaryDisplay(), &fingers);
    uint64_t bits = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    int nstick = 0;
    for (int i = 0; i < n; i++) {
        SDL_Finger *fi = fingers[i];
        int zi = zone_at(fi->x, fi->y);
        if (zi < 0) continue;
        Zone *z = &zones[zi];
        if (z->kind == Z_BUTTON) {
            bits |= BTN(z->btn);
        } else if (z->kind == Z_STICK) {
            const Finger *p = find_prev(fi->id);
            float ox = z->x, oy = z->y;
            float dx = (p ? p->x : fi->x) - ox, dy = (p ? p->y : fi->y) - oy;
            // si el dedo empezo dentro, se sigue usando aunque salga del circulo
            if (!p) { dx = 0, dy = 0; }
            float mag = sqrtf(dx * dx + dy * dy);
            if (mag > z->r) { dx *= z->r / mag; dy *= z->r / mag; mag = z->r; }
            float dead = z->r * 0.18f;
            float nx = mag > dead ? (dx / z->r) : 0.0f;
            float ny = mag > dead ? (dy / z->r) : 0.0f;
            if (z->stick) rx = (int)(nx * 32767.0f), ry = (int)(ny * 32767.0f);
            else lx = (int)(nx * 32767.0f), ly = (int)(ny * 32767.0f);
            nstick++;
        } else if (z->kind == Z_DPAD) {
            float dx = fi->x - z->x, dy = fi->y - z->y;
            if (fabsf(dx) > fabsf(dy)) {
                if (dx < -z->r * 0.15f) bits |= BTN(BTN_LEFT);
                if (dx > z->r * 0.15f) bits |= BTN(BTN_RIGHT);
            } else {
                if (dy < -z->r * 0.15f) bits |= BTN(BTN_UP);
                if (dy > z->r * 0.15f) bits |= BTN(BTN_DOWN);
            }
        }
    }
    (void)nstick;
    // memoriza los dedos para el frame siguiente
    nprev = 0;
    for (int i = 0; i < n && nprev < 16; i++) prev[nprev].id = fingers[i]->id, prev[nprev].x = fingers[i]->x,
                                          prev[nprev].y = fingers[i]->y, nprev++;
    t_buttons = bits, t_lx = lx, t_ly = ly, t_rx = rx, t_ry = ry;
}

// ------------------------------------------------------------------ dibujo
// Lista plana para el overlay (ImGui): por zona 7 floats
//   x, y, r, h, pulsada(0/1), kind, etiqueta_indice
int android_touch_zones(float *out, int max_zones) {
    if (!t_visible) return 0;
    int n = 0;
    for (int i = 0; i < nzones && n < max_zones; i++) {
        Zone *z = &zones[i];
        float *o = out + n * 7;
        o[0] = z->x, o[1] = z->y, o[2] = z->r * t_scale_all, o[3] = z->h * t_scale_all;
        o[4] = (z->kind == Z_BUTTON && (t_buttons & BTN(z->btn))) ? 1.0f : 0.0f;
        o[5] = (float)z->kind, o[6] = (float)i;
        n++;
    }
    return n;
}

const char *android_touch_label(int i) {
    if (i < 0 || i >= nzones) return "";
    Zone *z = &zones[i];
    if (z->kind == Z_BUTTON) return z->label;
    if (z->kind == Z_STICK) return z->stick ? "R" : "L";
    if (z->kind == Z_DPAD) return "D";
    if (z->kind == Z_CAMERA) return "C";
    return "";
}

float android_touch_opacity(void) { return t_opacity; }
int android_touch_zone_count(void) { return nzones; }
int android_touch_selected(void) { return t_sel; }

// El overlay pasa aqui los cambios que hace el usuario en el editor.
void android_touch_set_zone(int i, float x, float y, float r) {
    if (i < 0 || i >= nzones) return;
    if (t_snap_grid) x = roundf(x * 100.0f) / 100.0f, y = roundf(y * 100.0f) / 100.0f;
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    if (y < 0) y = 0;
    if (y > 1) y = 1;
    zones[i].x = x, zones[i].y = y;
    if (r > 0.005f) zones[i].r = r;
}

void android_touch_select(int i) { t_sel = (i >= 0 && i < nzones) ? i : -1; }

// -------------------------------------------------------- panel del editor
// Datos para que el overlay dibuje el panel de ajustes tactiles:
//   [0] hay dedo (0/1)  [1] dedo x  [2] dedo y  [3] zona bajo el dedo (-1)
//   [4] opacidad        [5] escala global       [6] iman (0/1)
void android_touch_edit_state(float *o) {
    o[0] = 0;
    SDL_Finger **fingers = NULL;
    int n = SDL_GetTouchFingers(SDL_GetPrimaryDisplay(), &fingers);
    if (n > 0) {
        o[0] = 1, o[1] = fingers[0]->x, o[2] = fingers[0]->y, o[3] = (float)zone_at(o[1], o[2]);
    } else {
        o[1] = o[2] = 0, o[3] = -1;
    }
    o[4] = t_opacity, o[5] = t_scale_all, o[6] = t_snap_grid ? 1.0f : 0.0f;
}

void android_touch_set_opacity(float v) {
    if (v < 0.05f) v = 0.05f;
    if (v > 1.0f) v = 1.0f;
    t_opacity = v;
}
void android_touch_set_scale_all(float v) {
    if (v < 0.4f) v = 0.4f;
    if (v > 2.5f) v = 2.5f;
    t_scale_all = v;
}
void android_touch_set_snap(bool on) { t_snap_grid = on; }
void android_touch_restore_defaults(void) { nzones = 0; defaults(); t_sel = -1; }
