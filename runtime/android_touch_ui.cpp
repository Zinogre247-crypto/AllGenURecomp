// [AllGenU-Android] Dibujo de los mandos tactiles + panel de ajustes en pantalla.
//
// Se llama desde runtime/overlay.cpp, dentro de ImGui::NewFrame()/Render().
// Toda la logica de toques vive en runtime/android_touch.c (C); aqui solo se pinta.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "config.h"
#include <SDL3/SDL.h>

// ---- lo que expone runtime/android_touch.c
extern "C" {
int android_touch_zones(float *out, int max_zones);  // 7 floats por zona
const char *android_touch_label(int i);
float android_touch_opacity(void);
int android_touch_zone_count(void);
int android_touch_selected(void);
void android_touch_select(int i);
void android_touch_set_zone(int i, float x, float y, float r);
void android_touch_set_opacity(float v);
void android_touch_set_scale_all(float v);
void android_touch_set_snap(bool on);
void android_touch_edit_state(float *o);  // dedo x, y, zona, opacidad, escala, iman
void android_touch_set_edit(bool v);
bool android_touch_edit(void);
void android_touch_set_visible(bool v);
bool android_touch_visible(void);
bool android_touch_save(void);
void android_touch_restore_defaults(void);
void ui_apply_display(void);  // runtime/ui.c
void vi_set_framerate(int fps);  // runtime/hle/vi.c
}

enum { ZK_NONE = 0, ZK_BUTTON, ZK_STICK, ZK_DPAD, ZK_CAMERA };

static const int k_res[][2] = {{0, 0},      {1280, 720},  {1600, 900},  {1920, 1080},
                               {2560, 1440}, {3840, 2160}};
static const char *const k_res_names[] = {"Auto (pantalla)", "1280x720", "1600x900", "1920x1080",
                                          "2560x1440", "3840x2160"};
static const int k_fps[] = {30, 45, 60, 90, 120, 144, 240, 0};
static const char *const k_fps_names[] = {"30", "45", "60", "90", "120", "144", "240", "libre"};

// ------------------------------------------------------------------ dibujo
// Círculos de las zonas, encima del juego. Sin ventana: puro ImGui de fondo.
void android_touch_draw_overlay(float opacity_mul) {
    if (!android_touch_visible()) return;
    ImGuiIO &io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    if (W <= 0 || H <= 0) return;
    float op = android_touch_opacity() * opacity_mul;
    if (op <= 0.01f) return;

    static float buf[48 * 7];
    int n = android_touch_zones(buf, 48);
    if (!n) return;

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    for (int i = 0; i < n; i++) {
        const float *z = buf + i * 7;
        float x = z[0] * W, y = z[1] * H, r = z[2] * H;  // (radio: relativo al alto, así no se deforma)
        float h = z[3] * H;
        bool down = z[4] > 0.5f;
        int kind = (int)z[5];
        int idx = (int)z[6];
        const char *label = android_touch_label(idx);

        ImU32 fill = down ? IM_COL32(255, 255, 255, (int)(150 * op)) : IM_COL32(20, 20, 26, (int)(110 * op));
        ImU32 line = down ? IM_COL32(255, 255, 255, (int)(235 * op)) : IM_COL32(235, 225, 200, (int)(160 * op));
        if (kind == ZK_BUTTON) {
            dl->AddCircleFilled(ImVec2(x, y), r, fill, 32);
            dl->AddCircle(ImVec2(x, y), r, line, 32, 2.0f * (H / 720.0f));
        } else if (kind == ZK_STICK || kind == ZK_DPAD) {
            dl->AddCircleFilled(ImVec2(x, y), r, IM_COL32(15, 15, 20, (int)(70 * op)), 32);
            dl->AddCircle(ImVec2(x, y), r, line, 32, 2.0f * (H / 720.0f));
            dl->AddCircleFilled(ImVec2(x, y), r * 0.18f, IM_COL32(200, 200, 200, (int)(60 * op)), 16);
            if (kind == ZK_DPAD) {  // cruz
                dl->AddLine(ImVec2(x - r * 0.6f, y), ImVec2(x + r * 0.6f, y), line, 1.5f);
                dl->AddLine(ImVec2(x, y - r * 0.6f), ImVec2(x, y + r * 0.6f), line, 1.5f);
            }
        } else if (kind == ZK_CAMERA) {  // zona de arrastre: rectángulo
            dl->AddRectFilled(ImVec2(x - r, y - h), ImVec2(x + r, y + h), fill, r * 0.3f);
            dl->AddRect(ImVec2(x - r, y - h), ImVec2(x + r, y + h), line, r * 0.3f, 0, 2.0f);
        }
        if (label && *label) {
            ImVec2 ts = ImGui::CalcTextSize(label);
            float fs = H / 720.0f;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * fs, ImVec2(x - ts.x * fs * 0.5f, y - ts.y * fs * 0.5f),
                        IM_COL32(255, 255, 255, (int)(220 * op)), label);
        }
    }
}

// ------------------------------------------------------------------ ajustes
static bool chip(const char *label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.25f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.35f, 0.30f, 1.0f));
    }
    bool hit = ImGui::Button(label, ImVec2(0, 0));
    if (active) ImGui::PopStyleColor(2);
    return hit;
}

static void apply_resolution(int i) {
    g_config.width = k_res[i][0];
    g_config.height = k_res[i][1];
    if (k_res[i][0] == 0) {  // "pantalla": el modo mas grande del display
        int dw = 0, dh = 0;
        SDL_DisplayMode const *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm) dw = dm->w, dh = dm->h;
        if (dw && dh) g_config.width = dw, g_config.height = dh;
    }
    char v[32];
    if (k_res[i][0] == 0) snprintf(v, sizeof v, "Desktop");
    else snprintf(v, sizeof v, "%dx%d", g_config.width, g_config.height);
    config_set("Display", "Resolution", v);
    ui_apply_display();
}

static void android_settings_draw(float scale) {
    ImGuiIO &io = ImGui::GetIO();
    float W = io.DisplaySize.x, H = io.DisplaySize.y;
    float w = (W < 900.0f ? W * 0.92f : 760.0f * scale);
    float h = (H < 600.0f ? H * 0.90f : 620.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.09f, 0.97f));
    ImGui::Begin("Ajustes", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    if (ImGui::BeginTabBar("##ajustes")) {
        // ---------------------------------------------------------- Pantalla
        if (ImGui::BeginTabItem("Pantalla")) {
            ImGui::TextDisabled("Resolucion de la ventana");
            int cur = 0;
            for (int i = 1; i < 6; i++)
                if (k_res[i][0] == g_config.width && k_res[i][1] == g_config.height) cur = i;
            for (int i = 0; i < 6; i++) {
                if (i) ImGui::SameLine();
                if (chip(k_res_names[i], cur == i)) apply_resolution(i);
            }
            ImGui::Spacing();
            ImGui::TextDisabled("Limite de FPS");
            int fi = 2;
            for (int i = 0; i < 8; i++)
                if (k_fps[i] == g_config.fps) fi = i;
            for (int i = 0; i < 8; i++) {
                if (i) ImGui::SameLine();
                if (chip(k_fps_names[i], fi == i)) {
                    g_config.fps = k_fps[i];
                    vi_set_framerate(g_config.fps);
                    char v[16];
                    snprintf(v, sizeof v, "%d", g_config.fps);
                    config_set("Performance", "FPS", v);
                }
            }
            ImGui::Spacing();
            bool vs = g_config.vsync != 0;
            if (ImGui::Checkbox("VSync (sin tearing)", &vs)) {
                g_config.vsync = vs ? 1 : 0;
                config_set("Performance", "VSync", vs ? "On" : "Off");
            }
            bool fps = g_config.show_fps != 0;
            if (ImGui::Checkbox("Contador de FPS", &fps)) {
                g_config.show_fps = fps ? 1 : 0;
                config_set("Performance", "ShowFPS", fps ? "On" : "Off");
            }
            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------- Tactil
        if (ImGui::BeginTabItem("Tactil")) {
            bool vis = android_touch_visible();
            if (ImGui::Checkbox("Mandos tactiles", &vis)) {
                android_touch_set_visible(vis);
                config_set("Touch", "Enabled", vis ? "On" : "Off");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(desactivalos si juegas con mando)");

            ImGui::Separator();
            float st[7];
            android_touch_edit_state(st);
            bool editing = android_touch_edit();
            if (ImGui::Button(editing ? "TERMINAR DE MOVER" : "MOVER / TAMANO", ImVec2(-1, 44 * scale)))
                android_touch_set_edit(!editing);
            if (editing) {
                ImGui::TextWrapped("Arrastra con el dedo el boton que quieras mover. Los cambios se ven al "
                                   "instante; pulsa GUARDAR para que se queden.");
                ImGui::Text("Dedo: x %.3f  y %.3f   Zona: %d   (dedos ahora: %d)", st[1], st[2], (int)st[3],
                            android_touch_zone_count());
            }

            ImGui::Separator();
            float op = android_touch_opacity() * 100.0f;
            if (ImGui::SliderFloat("Opacidad", &op, 5.0f, 100.0f, "%.0f %%")) android_touch_set_opacity(op / 100.0f);
            float sc = st[5] * 100.0f;
            if (ImGui::SliderFloat("Tamano de todos", &sc, 40.0f, 250.0f, "%.0f %%")) android_touch_set_scale_all(sc / 100.0f);

            int sel = android_touch_selected();
            const char *names[48];
            static char lbl[48][16];
            for (int i = 0; i < android_touch_zone_count(); i++) {
                snprintf(lbl[i], 16, "%d: %s", i, android_touch_label(i));
                names[i] = lbl[i];
            }
            if (ImGui::Combo("Boton", &sel, names, android_touch_zone_count())) android_touch_select(sel);
            if (sel >= 0) {
                static float buf[48 * 7];
                int n = android_touch_zones(buf, 48);
                if (sel < n) {
                    float zx = buf[sel * 7 + 0] * 100.0f, zy = buf[sel * 7 + 1] * 100.0f;
                    float rz = buf[sel * 7 + 2] * 100.0f;
                    if (ImGui::SliderFloat("Posicion X", &zx, 0.0f, 100.0f, "%.0f %%")) android_touch_set_zone(sel, zx / 100.0f, zy / 100.0f, 0);
                    if (ImGui::SliderFloat("Posicion Y", &zy, 0.0f, 100.0f, "%.0f %%")) android_touch_set_zone(sel, zx / 100.0f, zy / 100.0f, 0);
                    // ojo: el eje Y de la pantalla crece hacia abajo
                    if (ImGui::SliderFloat("Tamano", &rz, 1.0f, 30.0f, "%.0f %%")) android_touch_set_zone(sel, zx / 100.0f, zy / 100.0f, rz / 100.0f);
                }
            }
            ImGui::Spacing();
            if (ImGui::Button("GUARDAR", ImVec2(150 * scale, 40 * scale))) {
                if (android_touch_save()) ImGui::OpenPopup("guardado");
            }
            ImGui::SameLine();
            if (ImGui::Button("Por defecto", ImVec2(0, 40 * scale))) android_touch_restore_defaults();
            if (ImGui::BeginPopup("guardado")) {
                ImGui::Text("Guardado. Se aplica al reiniciar.");
                ImGui::EndPopup();
            }
            ImGui::EndTabItem();
        }

        // ---------------------------------------------------------- Informacion
        if (ImGui::BeginTabItem("Ayuda")) {
            ImGui::TextWrapped("Escape (o un mando) abre este menu. Los ajustes se guardan solos en "
                               "allgenurecomp.ini, junto a los archivos del juego.");
            ImGui::Separator();
            ImGui::TextDisabled("Archivos del juego");
            ImGui::TextWrapped("Android/data/com.kriszts.allgenu/files/game/  ->  exefs/ y romfs/");
            ImGui::TextDisabled("Layout tactil");
            ImGui::TextWrapped("files/touch_layout.txt (el editor lo reescribe)");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ------------------------------------------------------------------ entrada
// Los toques no se convierten en raton (ui.c pone SDL_HINT_TOUCH_MOUSE_EVENTS a 0
// para que el juego no reciba clics fantasma). Para que el panel se pueda usar
// con el dedo, aqui se inyectan como raton SOLO mientras el panel esta abierto.
static void feed_im_gui_with_fingers(void) {
    ImGuiIO &io = ImGui::GetIO();
    SDL_Finger **f = nullptr;
    int n = SDL_GetTouchFingers(SDL_GetPrimaryDisplay(), &f);
    static bool was_down;
    if (n > 0) {
        io.AddMousePosEvent(f[0]->x, f[0]->y);
        if (!was_down) io.AddMouseButtonEvent(ImGuiMouseButton_Left, true), was_down = true;
    } else if (was_down) {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        was_down = false;
    }
}

static bool g_open_settings;
static bool g_gear_down;

// Una llamada por frame desde runtime/overlay.cpp, antes de ImGui::Render().
//   scale: altura de la ventana / 720
//   menu_open: el menu del port (Escape) o el de Forge estan abiertos
void android_touch_overlay_frame(float scale, bool menu_open) {
    ImGuiIO &io = ImGui::GetIO();
    if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) return;
    bool touch_on = android_touch_visible();

    if (g_open_settings || menu_open) {
        android_touch_draw_overlay(1.0f);  // (se ven mientras se ajustan)
        feed_im_gui_with_fingers();
    } else if (touch_on) {
        android_touch_draw_overlay(1.0f);
    }

    // boton AJUSTES: arriba a la izquierda, pequeno, nunca encima del juego jugando
    if (touch_on && !menu_open) {
        float H = io.DisplaySize.y;
        ImGui::SetNextWindowPos(ImVec2(8 * scale, 8 * scale), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * scale, 6 * scale));
        ImGui::Begin("##touchgear", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav);
        float sz = (H < 720 ? 34.0f : 26.0f) * scale;
        if (ImGui::Button("AJUSTES", ImVec2(sz * 2.6f, sz))) g_open_settings = true;
        ImGui::SameLine();
        if (ImGui::Button("TACTIL ON/OFF", ImVec2(sz * 3.4f, sz))) {
            android_touch_set_visible(false);
            config_set("Touch", "Enabled", "Off");
        }
        ImGui::End();
        ImGui::PopStyleVar();
        g_gear_down = false;
        (void)g_gear_down;
    }

    if (g_open_settings) {
        android_settings_draw(scale);
        if (!android_touch_edit() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_open_settings = false;
    }

    // cerrar el panel con el boton de "terminar" ya vacia el editor; si el menu
    // principal se abre, el panel se queda detras
}
