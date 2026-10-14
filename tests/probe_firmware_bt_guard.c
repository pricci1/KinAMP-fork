/* PRIVATE firmware is supplied externally, never distributed with this test.
 * Executes its actual handleBtPowerSaveLogic -> PLT -> guard -> original timer
 * and real vendor GLib under ARM emulation. No device/services/radio access.
 * Only state getters, disconnect transport and the clock interval are modeled.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
typedef int (*Callback)(void *);
typedef struct _GSource GSource;
static unsigned timeout_calls, disconnects, installed;
static Callback callback;
static int operating_mode = 2;
static GSource *last_source;
static const char *status_path;
int getCurrentState(void) { return 2; }
int getCurrentBToperatingModeUtil(void) { return operating_mode; }
int disconnectAllA2dpSink(void) { ++disconnects; return 0; }
extern GSource *g_source_ref(GSource *);
extern void g_source_unref(GSource *);
extern int g_source_is_destroyed(GSource *);
GSource *g_timeout_source_new_seconds(unsigned interval) {
    GSource *(*real)(unsigned) = dlsym(RTLD_NEXT, "g_timeout_source_new_seconds");
    if (!real || interval != 1200) abort();
    ++timeout_calls;
    return real(1); /* Offline clock acceleration, not the candidate's policy. */
}
void g_source_set_callback(GSource *source, Callback cb, void *data, void (*notify)(void *)) {
    void (*real)(GSource *, Callback, void *, void (*)(void *)) = dlsym(RTLD_NEXT, "g_source_set_callback");
    if (!real || data || notify) abort();
    if (last_source) g_source_unref(last_source);
    last_source = g_source_ref(source);
    callback = cb; ++installed; real(source, cb, data, notify);
}
extern void handleBtPowerSaveLogic(void);
extern void setBtPowerSaveTimeout(int);
extern int stopPowerSaveTimer(void);
extern int g_main_context_iteration(void *, int);
static void status(const char *state) {
    FILE *f = fopen(status_path, "w"); if (!f) abort();
    fprintf(f, "pid=%s\nstate=%s\ndaemon=1\n", getenv("KINAMP_BT_GUARD_PID"), state);
    fclose(f);
    struct stat st;
    f = fopen(status_path, "r"); if (!f) abort();
    int result = fstat(fileno(f), &st); fclose(f);
    if (result || !S_ISREG(st.st_mode) || st.st_uid || st.st_size <= 0 ||
            time(NULL) < st.st_mtime || time(NULL) - st.st_mtime > 3) abort();
}
int main(int argc, char **argv) {
    if (argc != 3) return 64;
    int guarded = !strcmp(argv[1], "guarded"); status_path = argv[2];
    status("playing"); setBtPowerSaveTimeout(1200);
    handleBtPowerSaveLogic();
    if (timeout_calls != 1 || installed != 1 || !callback) abort();
    Dl_info library;
    void *actual_start = dlsym(dlopen("libacsbtfdlib.so.1.0", RTLD_NOW), "startPowerSaveTimer");
    if (!dladdr(actual_start, &library)) abort();
    uintptr_t original = (uintptr_t)library.dli_fbase + 0xdac9;
    if ((uintptr_t)callback == original ? guarded : !guarded) abort();
    g_main_context_iteration(NULL, 1);
    if (disconnects != (guarded ? 0u : 1u)) abort();
    if (guarded) {
        /* The real firmware policy, not a shim removal API, cancels the source
         * when the modeled native operating mode changes. */
        operating_mode = 1;
        handleBtPowerSaveLogic();
        if (!g_source_is_destroyed(last_source) || disconnects) abort();
        operating_mode = 2;
        handleBtPowerSaveLogic();
        if (timeout_calls != 2 || installed != 2) abort();
        status("paused");
        g_main_context_iteration(NULL, 1);
        if (disconnects != 1) abort();
    }
    g_source_unref(last_source);
    unlink(status_path);
    printf("PASS ARM actual firmware policy/PLT/timer/GLib: %s; original=1200s, offline clock=1s\n", argv[1]);
    return 0;
}
