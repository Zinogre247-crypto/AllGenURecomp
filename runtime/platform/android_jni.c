// Puente Android: entrada de SDL3, rutas de la app y mensajes de error (JNI).
//
// Como arranca una app SDL3 en Android:
//   SDLActivity (Java) -> System.loadLibrary("main") -> dlsym("SDL_main") -> SDL_main(argc, argv)
// Por eso:
//   * runtime/main.c se compila con -Dmain=allgenu_main (no hay que tocar el archivo)
//   * este archivo define SDL_main(), prepara las rutas y llama a allgenu_main
//
// Rutas (en Android no hay HOME ni XDG):
//   interno -> SDL_GetAndroidInternalStoragePath()  = /data/data/<pkg>/files
//              log.txt, cache/, mods/, cheats/ y saves
//   dump    -> se BUSCA (ya no hay que ponerlo a mano en Android/data/...):
//              1. .../Android/data/com.kriszts.allgenu/files/game
//              2. .../Android/data/com.kriszts.allgenu/files/ (si tiene exefs/)
//              3. <almacenamiento>/Download (y una carpeta dentro de ella)
//              4. <almacenamiento>/Download/game
//              5. <almacenamiento>/Documents (y una carpeta dentro)
//              6. /data/data/com.kriszts.allgenu/files/game
//              Vale que el dump tenga exefs/ + romfs/, o el zip exefs.zip (se
//              descomprime solo la primera vez).
//
// DIAGNOSTICO (en el movil, sin PC) - tres sitios donde mirar:
//   1) EN PANTALLA: AllGenUActivity pone un texto amarillo encima del juego con
//      lo ultimo que paso (y los errores se quedan ahi).
//   2) /sdcard/Download/AllGenU-log.txt  (se abre con Archivos -> Descargas y se
//      puede copiar/pegar; necesita el permiso de archivos).
//   3) files/diagnostico.txt dentro de la app, y log.txt al lado:
//        su -c 'cat /data/data/com.kriszts.allgenu/files/diagnostico.txt'
//      desde Termux con root (o adb desde un PC).
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#include <android/log.h>
#include <jni.h>
#include <SDL3/SDL.h>

#include "platform.h"

extern int allgenu_main(int argc, char **argv);   // runtime/main.c (-Dmain=allgenu_main)
extern char g_android_app_dir[];                   // android.c

static JavaVM *g_vm;
static jobject g_activity;      // referencia global a AllGenUActivity
static char g_internal[1024];
static char g_game[1100];
static char g_ext_root[1024];   // /storage/emulated/0
static char g_log_path[1100];   // files/diagnostico.txt
static char g_zip[1100];        // exefs.zip encontrado (si el dump viene en zip)
// Copia visible del registro: /sdcard/Download/AllGenU-log.txt (si hay permiso).
// Asi el jugador abre Archivos -> Descargas y puede copiar el texto.
static char g_public_log[1100];
static char g_all[16384];
static size_t g_all_len;

// Reescribe la copia publica: nuestras lineas + el final de log.txt (que es
// donde el propio runtime escribe lo suyo).
static void mirror_public_log(void) {
    if (!g_public_log[0]) return;
    FILE *f = fopen(g_public_log, "w");
    if (!f) return;              // sin permiso todavia: se reintenta luego
    if (g_all_len) fwrite(g_all, 1, g_all_len, f);

    // cola de log.txt (el runtime escribe ahi: LOG(), errores, volcados...)
    char lp[1200];
    snprintf(lp, sizeof lp, "%s/log.txt", g_internal);
    FILE *in = fopen(lp, "rb");
    if (in) {
        if (fseek(in, 0, SEEK_END) == 0) {
            long sz = ftell(in);
            long start = sz > 6000 ? sz - 6000 : 0;
            fseek(in, start, SEEK_SET);
            fputs("\n--- log.txt (final) ---\n", f);
            char buf[2048];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, f);
        }
        fclose(in);
    }
    fclose(f);
}

// Cada 2 s refresca la copia publica (asi se ve el avance aunque el juego se
// quede colgado y nunca llegue a escribir nada mas).
static void *public_log_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(2);
        mirror_public_log();
    }
    return NULL;
}

static void keep_line(const char *tag, const char *line) {
    if (g_all_len + strlen(line) + 8 >= sizeof g_all) return;
    int n = snprintf(g_all + g_all_len, sizeof g_all - g_all_len, "%s%s\n", tag, line);
    if (n > 0) g_all_len += (size_t)n;
}

const char *android_game_dir(void) { return g_game[0] ? g_game : NULL; }

// ---------------------------------------------------------------- registro
// Se escribe a la vez en logcat y en files/diagnostico.txt (a prueba de balas:
// no hace falta PC ni adb para leerlo).
void android_log_note(const char *fmt, ...) {
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_INFO, "AllGenU", "%s", line);
    keep_line("", line);
    if (!g_log_path[0]) return;
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

void android_log_error(const char *fmt, ...) {
    char line[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_ERROR, "AllGenU", "%s", line);
    keep_line("ERROR: ", line);
    if (!g_log_path[0]) return;
    FILE *f = fopen(g_log_path, "a");
    if (!f) return;
    fprintf(f, "ERROR: %s\n", line);
    fclose(f);
}


// Llamado por plat_error_box() de android.c, desde cualquier hilo del runtime.
void android_show_message(const char *msg) {
    android_log_error("%s", msg);
    if (!g_vm || !g_activity) return;

    JNIEnv *env = NULL;
    bool attached = false;
    if ((*g_vm)->GetEnv(g_vm, (void **)(void *)&env, JNI_VERSION_1_6) != JNI_OK) {
        JNIEnv *tmp = NULL;
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&tmp, NULL) != JNI_OK) return;
        env = tmp;
        attached = true;
    }

    jclass cls = (*env)->GetObjectClass(env, g_activity);
    if (cls) {
        jmethodID m = (*env)->GetMethodID(env, cls, "showMessage", "(Ljava/lang/String;)V");
        if (m) {
            jstring s = (*env)->NewStringUTF(env, msg);
            if (s) {
                (*env)->CallVoidMethod(env, g_activity, m, s);   // AllGenUActivity.showMessage
                (*env)->DeleteLocalRef(env, s);
            }
        }
        (*env)->DeleteLocalRef(env, cls);
    }

    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

// Manda una linea de estado a la Activity (se ve ENCIMA del juego).
// Asi, sin PC ni consola, se sabe hasta donde llego el arranque.
void android_status(const char *msg) {
    android_log_note("[estado] %s", msg);
    if (!g_vm || !g_activity) return;
    JNIEnv *env = NULL;
    bool attached = false;
    if ((*g_vm)->GetEnv(g_vm, (void **)(void *)&env, JNI_VERSION_1_6) != JNI_OK) {
        JNIEnv *tmp = NULL;
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&tmp, NULL) != JNI_OK) return;
        env = tmp;
        attached = true;
    }
    jclass cls = (*env)->GetObjectClass(env, g_activity);
    if (cls) {
        jmethodID m = (*env)->GetMethodID(env, cls, "showStatus", "(Ljava/lang/String;)V");
        if (m) {
            jstring s = (*env)->NewStringUTF(env, msg);
            if (s) {
                (*env)->CallVoidMethod(env, g_activity, m, s);
                (*env)->DeleteLocalRef(env, s);
            }
        }
        (*env)->DeleteLocalRef(env, cls);
    }
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

// ---------------------------------------------------------------- busqueda del dump
static bool is_dir(const char *p) {
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p) {
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

// Un "dump" valido tiene exefs/ (o exefs.zip) dentro.
static int dump_kind(const char *dir) {
    char p[1200];
    snprintf(p, sizeof p, "%s/exefs/main", dir);
    if (is_file(p)) return 2;                     // dump completo
    snprintf(p, sizeof p, "%s/exefs", dir);
    if (is_dir(p)) return 1;                      // exefs/ pero sin main (falta algo)
    snprintf(p, sizeof p, "%s/exefs.zip", dir);
    if (is_file(p)) return 3;                     // dump en zip
    return 0;
}

static bool try_game_dir(const char *dir) {
    if (!is_dir(dir)) return false;
    int k = dump_kind(dir);
    if (!k) return false;
    snprintf(g_game, sizeof g_game, "%s", dir);
    if (k == 3) {
        snprintf(g_zip, sizeof g_zip, "%s/exefs.zip", dir);
        android_log_note("[busqueda] dump en zip: %s", g_zip);
    } else {
        android_log_note("[busqueda] dump en %s (exefs %s)", dir, k == 2 ? "con main" : "sin main");
    }
    return true;
}

// Mira dentro de una carpeta (un nivel) por si el jugador dejo ahí algo como
// "Download/MHGU/exefs" en vez de "Download/exefs".
static bool scan_one_level(const char *parent, int max) {
    DIR *d = opendir(parent);
    if (!d) return false;
    struct dirent *e;
    int n = 0;
    bool found = false;
    while (!found && (e = readdir(d)) && n++ < max) {
        if (e->d_name[0] == '.') continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", parent, e->d_name);
        if (!is_dir(sub)) continue;
        found = try_game_dir(sub);
    }
    closedir(d);
    return found;
}

static void setup_paths(void) {
    const char *internal = SDL_GetAndroidInternalStoragePath();
    snprintf(g_internal, sizeof g_internal, "%s", (internal && *internal) ? internal : "/data/local/tmp");
    snprintf(g_android_app_dir, 1024, "%s", g_internal);
    snprintf(g_log_path, sizeof g_log_path, "%s/diagnostico.txt", g_internal);

    // Marca de "la parte nativa arranco": si existe, SDL_main corrio.
    {
        FILE *f = fopen(g_log_path, "a");
        if (f) {
            fprintf(f, "=== AllGenURecomp: arranco la parte nativa ===\n");
            fclose(f);
        }
    }

    setenv("HOME", g_internal, 1);
    setenv("XDG_DATA_HOME", g_internal, 1);

    const char *ext = SDL_GetAndroidExternalStoragePath();   // .../Android/data/<pkg>/files
    snprintf(g_ext_root, sizeof g_ext_root, "%s", (ext && *ext) ? ext : "");
    android_log_note("[rutas] interno=%s", g_internal);
    android_log_note("[rutas] externo=%s", g_ext_root[0] ? g_ext_root : "(no disponible)");

    // Raiz del almacenamiento: .../Android/data/<pkg>/files -> subir 4 niveles.
    char shared[1024] = "";
    if (g_ext_root[0]) {
        snprintf(shared, sizeof shared, "%s", g_ext_root);
        for (int i = 0; i < 4; i++) {
            char *s = strrchr(shared, '/');
            if (!s || s == shared) break;
            *s = 0;
        }
        android_log_note("[rutas] almacenamiento=%s", shared);
        snprintf(g_public_log, sizeof g_public_log, "%s/Download/AllGenU-log.txt", shared);
        keep_line("", "Copia del registro (pega esto en el chat si algo falla).");
        keep_line("", "Se actualiza sola cada 2 segundos.");
        pthread_t th;
        if (pthread_create(&th, NULL, public_log_thread, NULL) == 0) pthread_detach(th);
    }

    // 1) lo que diga el entorno (por si algun dia se lanza desde adb)
    const char *env = getenv("MHGU_GAME_DIR");
    if (env && *env) try_game_dir(env);

    // 2) sitios normales
    if (!g_game[0] && g_ext_root[0]) {
        char p[1200];
        snprintf(p, sizeof p, "%s/game", g_ext_root);
        try_game_dir(p);
        if (!g_game[0]) try_game_dir(g_ext_root);
    }
    if (!g_game[0] && shared[0]) {
        char p[1200];
        snprintf(p, sizeof p, "%s/Download", shared);
        if (!try_game_dir(p)) scan_one_level(p, 200);
        if (!g_game[0]) {
            snprintf(p, sizeof p, "%s/Download/game", shared);
            try_game_dir(p);
        }
        if (!g_game[0]) {
            snprintf(p, sizeof p, "%s/Documents", shared);
            if (!try_game_dir(p)) scan_one_level(p, 200);
        }
        if (!g_game[0]) {
            snprintf(p, sizeof p, "%s/Games", shared);
            if (!try_game_dir(p)) scan_one_level(p, 200);
        }
    }
    if (!g_game[0]) {
        char p[1200];
        snprintf(p, sizeof p, "%s/game", g_internal);
        try_game_dir(p);
    }

    if (g_game[0]) {
        setenv("MHGU_GAME_DIR", g_game, 1);
        android_log_note("[rutas] dump del juego = %s", g_game);
        char p[1200];
        snprintf(p, sizeof p, "%s/romfs", g_game);
        android_log_note("[rutas] romfs = %s", is_dir(p) ? "si" : "NO (el juego pedira romfs)");
    } else {
        android_log_error("[rutas] NO encontre el dump. Buscado: %s/game, %s/Download, "
                          "%s/Download/game, %s/Documents", g_ext_root, shared, shared, shared);
        setenv("MHGU_GAME_DIR", "", 1);
    }
}

// Descomprime exefs.zip en la MISMA carpeta (una sola vez) y devuelve la carpeta
// con el dump. Asi el jugador puede dejar el zip tal cual en Descargas.
static const char *ensure_unzipped(void) {
    if (!g_zip[0]) return g_game[0] ? g_game : NULL;
    static char dir[1100];
    snprintf(dir, sizeof dir, "%s", g_zip);
    char *s = strrchr(dir, '/');
    if (s) *s = 0;

    char main_path[1200];
    snprintf(main_path, sizeof main_path, "%s/exefs/main", dir);
    if (is_file(main_path)) {          // ya descomprimido antes
        android_log_note("[zip] %s ya estaba descomprimido", dir);
        return dir;
    }

    android_log_note("[zip] descomprimiendo %s (puede tardar un poco)...", g_zip);
    char cmd[1400];
    snprintf(cmd, sizeof cmd,
             "cd '%s' && (unzip -o -q 'exefs.zip' 2>&1 || "
             "/system/bin/toybox unzip -o -q 'exefs.zip' 2>&1 || "
             "/system/bin/busybox unzip -o -q 'exefs.zip' 2>&1)", dir);
    FILE *p = popen(cmd, "r");
    char out[400] = "";
    if (p) {
        size_t n = fread(out, 1, sizeof out - 1, p);
        out[n] = 0;
        pclose(p);
    }
    if (out[0]) android_log_note("[zip] salida: %.300s", out);

    if (is_file(main_path)) {
        android_log_note("[zip] listo: %s", dir);
        return dir;
    }
    // puede que el zip traiga exefs/ dentro de otra carpeta
    if (scan_one_level(dir, 50) && dump_kind(g_game) == 2) return g_game;
    android_log_error("[zip] no pude descomprimir %s", g_zip);
    return NULL;
}

// SDLActivity.nativeRunMain -> dlsym("SDL_main") -> aqui.
int SDL_main(int argc, char *argv[]) {
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (env) {
        if ((*env)->GetJavaVM(env, &g_vm) != JNI_OK) g_vm = NULL;
        jobject act = (jobject)SDL_GetAndroidActivity();
        if (act && g_vm) g_activity = (*env)->NewGlobalRef(env, act);
        if (g_activity) {
            jclass cls = (*env)->GetObjectClass(env, g_activity);
            if (cls) {
                jmethodID m = (*env)->GetMethodID(env, cls, "onNativeStart", "()V");
                if (m) (*env)->CallVoidMethod(env, g_activity, m);
                (*env)->DeleteLocalRef(env, cls);
            }
        }
    }

    setup_paths();

    if (g_game[0]) {
        char m[600];
        snprintf(m, sizeof m, "Juego encontrado: %.180s", g_game);
        android_status(m);
    }

    const char *game = ensure_unzipped();
    if (game) {
        snprintf(g_game, sizeof g_game, "%s", game);
        setenv("MHGU_GAME_DIR", g_game, 1);
    } else if (!g_game[0]) {
        android_show_message(
            "No encontre los archivos del juego.\n\n"
            "Pon las carpetas exefs y romfs (o el archivo exefs.zip) en la "
            "carpeta Download del telefono, o en Download/game, y vuelve a abrir "
            "la app.\n\n"
            "Detalles en: Archivos internos de la app / files / diagnostico.txt");
        android_status("NO encontre el dump del juego (ni exefs/ ni exefs.zip)");
        android_status("Ponlo en Download/ o Download/game y reabre la app");
    }

    android_status("C: llamando al juego (main)");
    int rc = allgenu_main(argc, argv);
    {
        char m[200];
        snprintf(m, sizeof m, "C: el juego termino con codigo %d", rc);
        android_status(m);
    }
    return rc;
}
