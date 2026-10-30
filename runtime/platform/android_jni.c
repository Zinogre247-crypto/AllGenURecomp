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
//   externo -> SDL_GetAndroidExternalStoragePath()/game = Android/data/<pkg>/files/game
//              el dump del jugador (exefs/ y romfs/)
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jni.h>
#include <SDL3/SDL.h>

#include "platform.h"

extern int allgenu_main(int argc, char **argv);   // runtime/main.c (-Dmain=allgenu_main)
extern char g_android_app_dir[];                   // android.c

static JavaVM *g_vm;
static jobject g_activity;      // referencia global a AllGenUActivity
static char g_internal[1024];
static char g_game[1100];

const char *android_game_dir(void) { return g_game[0] ? g_game : NULL; }

// Llamado por plat_error_box() de android.c, desde cualquier hilo del runtime.
void android_show_message(const char *msg) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", msg);
    if (!g_vm || !g_activity) return;

    JNIEnv *env = NULL;
    bool attached = false;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_vm)->AttachCurrentThread(g_vm, (void **)&env, NULL) != JNI_OK) return;
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

static void setup_paths(void) {
    const char *internal = SDL_GetAndroidInternalStoragePath();
    snprintf(g_internal, sizeof g_internal, "%s", (internal && *internal) ? internal : "/data/local/tmp");
    snprintf(g_android_app_dir, 1024, "%s", g_internal);

    setenv("HOME", g_internal, 1);
    setenv("XDG_DATA_HOME", g_internal, 1);

    const char *ext = SDL_GetAndroidExternalStoragePath();
    if (ext && *ext) {
        snprintf(g_game, sizeof g_game, "%s/game", ext);
        setenv("MHGU_GAME_DIR", g_game, 1);
    } else if (getenv("MHGU_GAME_DIR")) {
        snprintf(g_game, sizeof g_game, "%s", getenv("MHGU_GAME_DIR"));
    }

    SDL_Log("AllGenU: interno=%s", g_internal);
    SDL_Log("AllGenU: dump del juego=%s (%s)", g_game,
            (g_game[0] && access(g_game, R_OK) == 0) ? "legible" : "NO encontrado");
}

// SDLActivity.nativeRunMain -> dlsym("SDL_main") -> aqui.
int SDL_main(int argc, char *argv[]) {
    // SDL3 >= 3.2 expone el JNIEnv y la Activity (devuelven void* en la API).
    JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
    if (env) {
        if ((*env)->GetJavaVM(env, &g_vm) != JNI_OK) g_vm = NULL;
        jobject act = (jobject)SDL_GetAndroidActivity();
        if (act && g_vm) g_activity = (*env)->NewGlobalRef(env, act);
    } else {
        SDL_Log("AllGenU: sin JNIEnv; los dialogos de error iran solo a logcat");
    }

    setup_paths();

    // SDL3 ya inicializo el video desde el Activity; el runtime vuelve a llamar a
    // SDL_Init, que es idempotente.
    return allgenu_main(argc, argv);
}
