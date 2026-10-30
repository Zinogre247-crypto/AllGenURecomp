// [AllGenU-Android] Mandos tactiles (botones en pantalla).
//
// El port de PC decidio "sin controles tactiles" (docs/research/android.md). Esto
// lo cambia: una capa de botones dibujados encima del juego que produce el MISMO
// estado de mando que un gamepad fisico. No toca input.c ni el juego: escribe
// donde escriben el teclado y el mando SDL (host_input.c -> input_set).
//
// Como funciona:
//   1. Las zonas de los botones se definen en el layout (archivo de texto, ver
//      abajo), en coordenadas normalizadas 0..1 sobre la PANTALLA.
//   2. Cada frame, host_input_update() llama a ainput_poll(), que lee los dedos
//      con SDL_GetTouchFingers() (multitouch real: se puede pulsar A + stick a la
//      vez), aplica histeresis a los sticks y devuelve botones + ejes.
//   3. El overlay (ImGui, runtime/overlay.cpp) dibuja las zonas con ainput_draw().
//   4. Si un gamepad fisico esta conectado, se puede ocultar con el boton de la
//      esquina o con la variable de entorno ALLGENU_TOUCH=0.
//
// Layout (assets/touch_layout.txt, override en /data/data/<pkg>/files/touch_layout.txt):
//      # tipo   id        x      y      r      [radio o w/h]
//      button  A         0.88   0.80   0.055
//      button  B         0.79   0.88   0.055
//      button  X         0.79   0.72   0.055
//      button  Y         0.70   0.80   0.055
//      button  L         0.06   0.10   0.05
//      button  R         0.16   0.10   0.05
//      button  ZL        0.06   0.22   0.05
//      button  ZR        0.16   0.22   0.05
//      button  PLUS      0.95   0.50   0.035
//      button  MINUS     0.88   0.50   0.035
//      stick   L         0.15   0.75   0.11
//      stick   R         0.80   0.35   0.11
//      dpad    D         0.06   0.88   0.10
//      camera L          0.55   0.10   0.09   # zona de arrastre para la camara (lado derecho)
//
// Nota sobre el escalado: SDL_GetTouchFingers da x/y normalizados a la PANTALLA,
// asi que las zonas se colocan sobre la pantalla completa (letterbox incluido).
// Si el juego se ve con bandas negras, mete los botones un poco hacia el centro.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "hle/input.h"

#define BTN(x) (1ull << (x))

// ---- estado que consume host_input.c
static uint64_t t_buttons;
static int t_lx, t_ly, t_rx, t_ry;
static bool t_visible = true;

uint64_t android_touch_buttons(void) { return t_visible ? t_buttons : 0; }
void android_touch_sticks(int *lx, int *ly, int *rx, int *ry) {
    *lx = t_visible ? t_lx : 0;
    *ly = t_visible ? t_ly : 0;
    *rx = t_visible ? t_rx : 0;
    *ry = t_visible ? t_ry : 0;
}
bool android_touch_visible(void) { return t_visible; }
void android_touch_set_visible(bool v) { t_visible = v; }

// ---- zonas
enum { Z_NONE = 0, Z_BUTTON, Z_STICK, Z_DPAD, Z_CAMERA };
#define MAX_ZONES 48
typedef struct {
    int kind;
    int btn;          // bit BTN_* (Z_BUTTON)
    int stick;        // 0 = izquierdo, 1 = derecho (Z_STICK)
    float x, y;       // centro, normalizado a la pantalla
    float r;          // radio (boton/stick) o ancho de la zona tactil (dpad/camera)
    float h;          // alto (dpad)
    char label[12];
} Zone;

static Zone zones[MAX_ZONES];
static int nzones;

static int button_bit(const char *name) {
    static const char *names[16] = {"A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R", "ZL", "ZR",
                                    "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
    for (int i = 0; i < 16; i++)
        if (!strcmp(name, names[i])) return i;
    return -1;
}

// Busca el layout: junto al ejecutable, en home (app), o ALLGENU_TOUCH_LAYOUT.
static FILE *open_layout(void) {
    const char *env = getenv("ALLGENU_TOUCH_LAYOUT");
    if (env && *env) {
        FILE *f = fopen(env, "r");
        if (f) return f;
    }
    const char *files[] = {"touch_layout.txt", "assets/touch_layout.txt", "../assets/touch_layout.txt"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        FILE *f = fopen(files[i], "r");
        if (f) return f;
    }
    return NULL;
}

void android_touch_init(void) {
    const char *off = getenv("ALLGENU_TOUCH");
    if (off && (!strcmp(off, "0") || !strcmp(off, "off"))) t_visible = false;

    nzones = 0;
    FILE *f = open_layout();
    char line[160];
    if (!f) {
        SDL_Log("touch: sin touch_layout.txt, uso el layout por defecto");
        // (layout por defecto, en el mismo formato que el archivo)
        static const char *def[] = {
            "button A 0.88 0.80 0.055",   "button B 0.79 0.885 0.055",
            "button X 0.79 0.715 0.055",  "button Y 0.70 0.80 0.055",
            "button L 0.07 0.12 0.05",    "button R 0.19 0.12 0.05",
            "button ZL 0.07 0.26 0.05",   "button ZR 0.19 0.26 0.05",
            "button PLUS 0.955 0.55 0.035", "button MINUS 0.88 0.55 0.035",
            "stick L 0.15 0.74 0.105",    "stick R 0.80 0.34 0.105",
            "dpad D 0.07 0.86 0.10",      "camera R 0.55 0.50 0.45",
        };
        for (unsigned i = 0; i < sizeof def / sizeof *def && nzones < MAX_ZONES; i++) {
            Zone *z = &zones[nzones];
            char kind[16], id[16];
            if (sscanf(def[i], "%15s %15s %f %f %f", kind, id, &z->x, &z->y, &z->r) != 5) continue;
            memset(z, 0, sizeof *z);
            sscanf(def[i], "%15s %15s %f %f %f", kind, id, &z->x, &z->y, &z->r);
            if (!strcmp(kind, "button")) {
                z->kind = Z_BUTTON;
                z->btn = button_bit(id);
                snprintf(z->label, sizeof z->label, "%s", id);
                if (z->btn < 0) continue;
            } else if (!strcmp(kind, "stick")) {
                z->kind = Z_STICK;
                z->stick = (!strcmp(id, "R") || !strcmp(id, "right")) ? 1 : 0;
            } else if (!strcmp(kind, "dpad")) {
                z->kind = Z_DPAD;
            } else if (!strcmp(kind, "camera")) {
                z->kind = Z_CAMERA;
            } else {
                continue;
            }
            nzones++;
        }
        SDL_Log("touch: %d zonas (layout por defecto)", nzones);
        return;
    }

    while (fgets(line, sizeof line, f) && nzones < MAX_ZONES) {
        char kind[16] = {0}, id[16] = {0};
        float x, y, r, h = 0;
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        if (sscanf(line, "%15s %15s %f %f %f %f", kind, id, &x, &y, &r, &h) < 5) continue;
        Zone *z = &zones[nzones];
        memset(z, 0, sizeof *z);
        z->x = x, z->y = y, z->r = r, z->h = h;
        if (!strcmp(kind, "button")) {
            z->kind = Z_BUTTON;
            z->btn = button_bit(id);
            snprintf(z->label, sizeof z->label, "%s", id);
            if (z->btn < 0) { SDL_Log("touch: boton '%s' desconocido", id); continue; }
        } else if (!strcmp(kind, "stick")) {
            z->kind = Z_STICK;
            z->stick = (!strcmp(id, "R") || !strcmp(id, "right")) ? 1 : 0;
        } else if (!strcmp(kind, "dpad")) {
            z->kind = Z_DPAD;
        } else if (!strcmp(kind, "camera")) {
            z->kind = Z_CAMERA;
        } else {
            continue;
        }
        nzones++;
    }
    fclose(f);
    SDL_Log("touch: %d zonas cargadas", nzones);
}

// ---- lectura de dedos
typedef struct { SDL_FingerID id; float x, y; } Finger;
#define MAX_FINGERS 20
static Finger prev[MAX_FINGERS];
static int nprev;

static void remember_fingers(SDL_Finger **f, int n) {
    if (n > MAX_FINGERS) n = MAX_FINGERS;
    for (int i = 0; i < n; i++) {
        prev[i].id = f[i]->id;
        prev[i].x = f[i]->x;
        prev[i].y = f[i]->y;
    }
    nprev = n;
}

static const Finger *find_prev(SDL_FingerID id) {
    for (int i = 0; i < nprev; i++)
        if (prev[i].id == id) return &prev[i];
    return NULL;
}

// Zona bajo un dedo. `out_dist` = distancia normalizada al centro (0 = centro).
// Prioridad: botones/sticks/dpad antes que la zona de camara (que es grande y
// taparia los botones si se comprobara primero).
static Zone *zone_at(float x, float y, float *out_dist) {
    for (int pass = 0; pass < 2; pass++)
    for (int i = 0; i < nzones; i++) {
        Zone *z = &zones[i];
        if (pass == 0 && z->kind == Z_CAMERA) continue;
        if (pass == 1 && z->kind != Z_CAMERA) continue;
        float dx = x - z->x, dy = y - z->y;
        if (z->kind == Z_CAMERA) {
            if (fabsf(dx) <= z->r && fabsf(dy) <= (z->h > 0 ? z->h : z->r)) {
                if (out_dist) *out_dist = 0;
                return z;
            }
            continue;
        }
        if (dx * dx + dy * dy <= z->r * z->r) {
            if (out_dist) *out_dist = sqrtf(dx * dx + dy * dy);
            return z;
        }
    }
    return NULL;
}

#define STICK_DEADZONE 0.18f   // en radios del stick (evita deriva)
#define CAMERA_SENS 2.6f       // radianes por radio recorrido

// Llamado desde host_input_update() cada frame.
void ainput_poll(void) {
    t_buttons = 0;
    if (!t_visible) { t_lx = t_ly = t_rx = t_ry = 0; nprev = 0; return; }

    // ejes que se van a construir este frame
    float lx = 0, ly = 0, rx = 0, ry = 0, cam_x = 0, cam_y = 0;
    uint64_t bits = 0;
    // zona "activa" por dedo: si un dedo empezo en el stick, sigue en el stick
    // aunque se salga del circulo (asi se puede girar sin levantar el dedo)
    static struct { SDL_FingerID id; int zone; } lock[MAX_FINGERS];
    static int nlock;

    int ntouch = 0;
    SDL_TouchID *devs = SDL_GetTouchDevices(&ntouch);
    if (!devs || ntouch == 0) { t_lx = t_ly = t_rx = t_ry = 0; return; }

    int seen = 0;
    static Finger now[MAX_FINGERS];
    for (int d = 0; d < ntouch; d++) {
        int nf = 0;
        SDL_Finger **f = SDL_GetTouchFingers(devs[d], &nf);
        for (int i = 0; i < nf && seen < MAX_FINGERS; i++) {
            float x = f[i]->x, y = f[i]->y;
            float dist = 0;
            Zone *z = zone_at(x, y, &dist);
            int zi = z ? (int)(z - zones) : -1;

            // recomprobar el lock de este dedo
            int li = -1;
            for (int k = 0; k < nlock; k++)
                if (lock[k].id == f[i]->id) { li = k; break; }
            if (li < 0 && z && z->kind != Z_CAMERA && nlock < MAX_FINGERS) {
                li = nlock++;
                lock[li].id = f[i]->id;
                lock[li].zone = zi;
            }
            if (li >= 0) {
                z = &zones[lock[li].zone];
                zi = lock[li].zone;
            }

            if (z) {
                if (z->kind == Z_BUTTON) {
                    bits |= BTN(z->btn);
                } else if (z->kind == Z_STICK) {
                    float dx = (x - z->x) / z->r, dy = (y - z->y) / z->r;   // -1..1 (puede pasar de 1 si se sale)
                    float m = sqrtf(dx * dx + dy * dy);
                    if (m > 1) { dx /= m; dy /= m; m = 1; }
                    if (m < STICK_DEADZONE) dx = dy = 0;
                    int vx = (int)(dx * 32767.0f), vy = (int)(-dy * 32767.0f);  // SDL y abajo, guest y arriba
                    if (z->stick == 0) lx = (float)vx / 32767.0f, ly = (float)vy / 32767.0f;
                    else rx = (float)vx / 32767.0f, ry = (float)vy / 32767.0f;
                } else if (z->kind == Z_DPAD) {
                    // dentro del circulo del dpad: elegir la direccion dominante
                    float dx = x - z->x, dy = y - z->y;
                    if (fabsf(dx) > fabsf(dy))
                        bits |= BTN(dx > 0 ? BTN_RIGHT : BTN_LEFT);
                    else
                        bits |= BTN(dy > 0 ? BTN_DOWN : BTN_UP);
                } else if (z->kind == Z_CAMERA) {
                    // arrastre relativo -> stick derecho (como el raton en PC).
                    // Se acumula en cam_x/cam_y y se suma al final, para no
                    // pisar un stick derecho tactil que se este usando a la vez.
                    const Finger *p = find_prev(f[i]->id);
                    if (p) {
                        cam_x += -(x - p->x) * CAMERA_SENS;
                        cam_y +=  (y - p->y) * CAMERA_SENS;
                    }
                }
            }
            now[seen++] = (Finger){f[i]->id, x, y};
        }
    }

    // quitar locks de dedos levantados
    int k = 0;
    for (int i = 0; i < nlock; i++) {
        bool vivo = false;
        for (int j = 0; j < seen; j++)
            if (now[j].id == lock[i].id) { vivo = true; break; }
        if (vivo) lock[k++] = lock[i];
    }
    nlock = k;

    memcpy(prev, now, sizeof(Finger) * (size_t)seen);
    nprev = seen;

    // camara: se suma al stick derecho y se recorta a [-1,1]
    rx += cam_x, ry += cam_y;
    if (rx > 1) rx = 1;
    if (rx < -1) rx = -1;
    if (ry > 1) ry = 1;
    if (ry < -1) ry = -1;

    t_buttons = bits;
    t_lx = (int)(lx * 32767.0f);
    t_ly = (int)(ly * 32767.0f);
    t_rx = (int)(rx * 32767.0f);
    t_ry = (int)(ry * 32767.0f);
}

// ---- dibujo (lo llama el overlay ImGui)
// Se entrega como lista plana para no meter tipos de ImGui en este archivo:
//   x, y, r, pressed (0/1), kind, label
int android_touch_zones(float *out, int max_zones) {
    if (!t_visible) return 0;
    int n = 0;
    for (int i = 0; i < nzones && n < max_zones; i++) {
        Zone *z = &zones[i];
        float *o = out + n * 6;
        o[0] = z->x;
        o[1] = z->y;
        o[2] = z->r;
        o[3] = (z->kind == Z_BUTTON && (t_buttons & BTN(z->btn))) ? 1.0f : 0.0f;
        o[4] = (float)z->kind;
        o[5] = 0;  // etiqueta aparte (ver android_touch_label)
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
    return "";
}
