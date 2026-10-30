// Android host layer (runtime/platform.h) — puerto de linux.c
//
// Diferencias con linux.c que importan en bionic/Android:
//
// 1. SIGSEGV: el guest reserva 4 GB en GUEST_BASE (0x10000000000 = 1 TB) y las
//    paginas se respaldan bajo demanda desde el handler. En arm64 Android el
//    handler NECESITA leer/escribir el PC y el SP del ucontext con los nombres
//    de arm64 (uc_mcontext.pc / .sp), no con REG_RIP/REG_RSP de x86-64.
// 2. MAP_FIXED_NOREPLACE: en kernels Android viejos (< 4.17) no existe; se
//    emula con una comprobacion previa con /proc/self/maps y MAP_FIXED.
// 3. Direccion de 1 TB: viable, pero necesita que el kernel del telefono tenga
//    39/48 bits de VA (todos los arm64 modernos). Si falla, plat_reserve_guest
//    devuelve false y main.c aborta con "not free" — ahi se ve.
// 4. Log: en Android no hay stderr util; todo va a log.txt en el almacenamiento
//    de la app Y a logcat via __android_log_write.
// 5. Rutas: plat_user_dir() usa el directorio de datos de la app
//    (/data/data/<pkg>/files), sin XDG.
// 6. plat_error_box(): no hay zenity/kdialog; se usa un Toast/AlertDialog via JNI
//    (ver android_jni.c) y como respaldo, android_log.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <sched.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AllGenU", __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AllGenU", __VA_ARGS__)
#else
#define ALOGE(...) fprintf(stderr, __VA_ARGS__)
#define ALOGI(...) fprintf(stderr, __VA_ARGS__)
#endif

#include "../platform.h"

// La ruta de datos de la app la pone android_jni.c (JNI_OnLoad / SDL_main).
// Si esta vacia se cae a "." (util para pruebas en el telefono con adb shell).
char g_android_app_dir[PATH_MAX] = "";

// ---- guest address space
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull
#define MAX_CHUNKS ((0x100000000ull + COMMIT_CHUNK - 1) / COMMIT_CHUNK)

enum { CHUNK_FREE = 0, CHUNK_BUSY, CHUNK_READY };
static _Atomic unsigned char g_chunk[MAX_CHUNKS];

static bool chunk_back(uint64_t base) {
    void *p = mmap((void *)(uintptr_t)base, COMMIT_CHUNK, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    return p == (void *)(uintptr_t)base;
}

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    uint64_t idx = (a - g_lo) / COMMIT_CHUNK;
    if (idx >= MAX_CHUNKS) return false;
    unsigned char expect = CHUNK_FREE;
    if (atomic_compare_exchange_strong(&g_chunk[idx], &expect, CHUNK_BUSY)) {
        bool ok = chunk_back(g_lo + idx * COMMIT_CHUNK);
        atomic_store(&g_chunk[idx], ok ? CHUNK_READY : CHUNK_FREE);
        return ok;
    }
    while (atomic_load(&g_chunk[idx]) == CHUNK_BUSY) atomic_thread_fence(memory_order_acquire);
    return atomic_load(&g_chunk[idx]) == CHUNK_READY;
}

// Emulacion de MAP_FIXED_NOREPLACE para kernels Android antiguos: mira
// /proc/self/maps y comprueba que [base, base+size) este libre.
static bool range_is_free(uint64_t base, uint64_t size) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return true;  // sin maps no se puede comprobar: intentamos igual
    char line[512];
    bool libre = true;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo = 0, hi = 0;
        if (sscanf(line, "%llx-%llx", &lo, &hi) != 2) continue;
        if (base < hi && base + size > lo) { libre = false; break; }
    }
    fclose(f);
    return libre;
}

static void fault_handler(int sig, siginfo_t *si, void *uc);

bool plat_reserve_guest(uint64_t base, uint64_t size) {
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    void *p = mmap((void *)(uintptr_t)base, size, PROT_NONE, flags | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)base) {
        // Kernel sin MAP_FIXED_NOREPLACE: comprobar a mano y forzar.
        if (!range_is_free(base, size)) {
            ALOGE("[plat] el rango %llx+%llx contiene algo mapeado", (unsigned long long)base,
                  (unsigned long long)size);
            return false;
        }
        p = mmap((void *)(uintptr_t)base, size, PROT_NONE, flags | MAP_FIXED, -1, 0);
        if (p != (void *)(uintptr_t)base) {
            ALOGE("[plat] mmap de la ventana guest fallo: %s", strerror(errno));
            return false;
        }
    }
    g_lo = base;
    g_hi = base + size;
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    ALOGI("[plat] ventana guest %llx-%llx reservada", (unsigned long long)g_lo, (unsigned long long)g_hi);
    return true;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~(COMMIT_CHUNK - 1), e = (addr + size + COMMIT_CHUNK - 1) & ~(COMMIT_CHUNK - 1);
    for (uint64_t c = a; c < e && c < g_hi; c += COMMIT_CHUNK) lazy_commit(c);
    if (e > g_hi)
        ALOGE("[plat] commit %llx+%llx fuera de la ventana", (unsigned long long)addr, (unsigned long long)size);
}

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;  // sin write-watch (igual que linux.c): el renderer compara contenidos
}

bool plat_committed(uint64_t addr, uint64_t *end) {
    if (!g_lo || addr < g_lo || addr >= g_hi) { *end = addr + PLAT_PAGE; return false; }
    uint64_t idx = (addr - g_lo) / COMMIT_CHUNK;
    if (atomic_load(&g_chunk[idx]) != CHUNK_READY) {
        *end = g_lo + (idx + 1) * COMMIT_CHUNK;
        return false;
    }
    uint64_t n = idx;
    while (n + 1 < MAX_CHUNKS && n - idx < 4096 && atomic_load(&g_chunk[n + 1]) == CHUNK_READY) n++;
    *end = g_lo + (n + 1) * COMMIT_CHUNK;
    return true;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return mprotect((void *)(uintptr_t)addr, size, p) == 0 ? 0 : -1;
}

// ---- time
uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void plat_sleep_until_ns(uint64_t t) {
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { sched_yield(); continue; }
        struct timespec due = {(time_t)(left / 1000000000ull), (long)(left % 1000000000ull)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &due, NULL);
    }
}

// ---- process
void plat_init(void) {
    signal(SIGPIPE, SIG_IGN);
    // En Android el "cwd" del proceso es "/" y no es escribible: nos movemos al
    // directorio privado de la app, que es donde van log.txt, cache/, mods/, cheats/.
    const char *dir = g_android_app_dir[0] ? g_android_app_dir : getenv("MHGU_HOME");
    if (dir && *dir) {
        mkdir(dir, 0755);
        if (chdir(dir) != 0) ALOGE("[plat] chdir(%s) fallo: %s", dir, strerror(errno));
    }
}

bool plat_exe_path(char *buf, size_t size) {
    // /proc/self/exe en Android apunta al zygote/app_process: se usa la ruta de la
    // biblioteca nativa si el linker la expone, y si no, el directorio de la app.
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n > 0 && (size_t)n < size - 1) {
        buf[n] = 0;
        // app_process: no sirve como "carpeta del ejecutable"
        if (!strstr(buf, "app_process")) return true;
    }
    if (g_android_app_dir[0]) {
        snprintf(buf, size, "%s", g_android_app_dir);
        return true;
    }
    return false;
}

static bool g_own_log;

void plat_standalone(void) {
    const char *home = g_android_app_dir[0] ? g_android_app_dir : getenv("MHGU_HOME");
    if (home && *home) {
        mkdir(home, 0755);
        if (chdir(home) != 0) { /* nos quedamos donde estemos */ }
    }
    // En Android stderr no es una terminal: el log va a archivo y a logcat.
    if (!isatty(STDERR_FILENO)) {
        if (freopen("log.txt", "w", stderr)) { setvbuf(stderr, NULL, _IONBF, 0); g_own_log = true; }
    }
}

static bool mkdir_p(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) return false;
        *s = '/';
    }
    return mkdir(buf, 0777) == 0 || errno == EEXIST;
}

// En Android no hay XDG: el "home" del juego es el directorio de datos de la app
// (/data/data/<pkg>/files), que es lo que devuelve android_jni.c.
bool plat_user_dir(char *buf, size_t size) {
    const char *base = g_android_app_dir[0] ? g_android_app_dir : getenv("HOME");
    if (!base || !*base) return false;
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s/AllGenURecomp", base) >= (int)sizeof dir) return false;
    if (!mkdir_p(dir)) return false;
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    snprintf(buf, size, "%s", dir);
    return true;
}

// Sin zenity ni kdialog: android_jni.c registra un callback que muestra un
// AlertDialog en el hilo de UI (lo llama el hilo que detecta el error).
void android_show_message(const char *msg);  // android_jni.c

void plat_error_box(const char *msg) {
    ALOGE("*** %s", msg);
    android_show_message(msg);
}

void plat_fatal_notice(void) {
    if (g_own_log)
        plat_error_box("El juego se detuvo por un error. Los detalles estan en log.txt, "
                       "en el almacenamiento de la app.");
}

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[PATH_MAX];
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_fname) return NULL;
    const char *slash = strrchr(di.dli_fname, '/');
    snprintf(name, sizeof name, "%s", slash ? slash + 1 : di.dli_fname);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_fbase;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    // backtrace() existe en bionic >= 21 (libdl / libc)
    int n = backtrace(bt, 64);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = rt_host_fn_name(pc, &off);
        if (!s) s = plat_symbol(pc, &off);
        if (s) ALOGE("  #%-2d %016llx %s+%llx", i, (unsigned long long)pc, s, (unsigned long long)off);
        else ALOGE("  #%-2d %016llx", i, (unsigned long long)pc);
    }
}

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    default: return "signal";
    }
}

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *ctx = (ucontext_t *)uc;
    uint64_t fa = si && si->si_addr ? (uint64_t)(uintptr_t)si->si_addr : 0;
    if ((sig == SIGSEGV || sig == SIGBUS) && lazy_commit(fa)) return;  // pagina guest nueva: respaldarla y seguir

    // ARM64: los nombres del ucontext son distintos a x86-64 (esto es lo que hay
    // que cambiar respecto a linux.c cuando se compila para arm64).
#if defined(__aarch64__)
    uint64_t pc = (uint64_t)ctx->uc_mcontext.pc;
    uint64_t sp = (uint64_t)ctx->uc_mcontext.sp;
#else
    uint64_t pc = (uint64_t)ctx->uc_mcontext.gregs[REG_RIP];
    uint64_t sp = (uint64_t)ctx->uc_mcontext.gregs[REG_RSP];
#endif
    uint64_t lr = 0, end = 0;
    if (plat_committed(sp, &end) && end - sp >= 8) lr = *(const uint64_t *)(uintptr_t)sp;
    char what[80];
    snprintf(what, sizeof what, "%s (code %d, addr %llx)", sig_name(sig), si ? si->si_code : 0,
             (unsigned long long)fa);
    rt_fault_report(what, fa, pc, lr);
    _exit(139);  // (rt_fault_report no vuelve)
}

void plat_crash_handler_install(void) {
    static char altstack[256 * 1024] __attribute__((aligned(16)));
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack}, old;
    if (sigaltstack(NULL, &old) == 0 && !(old.ss_flags & SS_ONSTACK)) sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}
