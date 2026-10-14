/* Experimental ACS-side idle-disconnect policy. NOT a playback registration.
 * Native approval/deployment is separate; never preload into the player.
 * Verified ABI: PW6 5.19.3.0.1 libacsbtfdlib SHA256
 * 569865512abd5ad74bface0a8d01123d695639955ac5dc3bf288d65094f10774.
 * The launcher MUST check that hash before starting the temporary daemon.
 * No firmware settings, events, radio operations or power writes here.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* GLib's public GSourceFunc/gboolean/gpointer ABI, without a GLib dependency. */
typedef int (*TimerCallback)(void *);
typedef int (*StartTimer)(TimerCallback, void *);
static StartTimer real_start;
static TimerCallback disconnect_callback;
static long owner_pid;
static unsigned long long owner_start;
static const char *status_path;
static const char *binding_path;
static double expires;
static int enabled;

static double monotonic_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static unsigned long long env_number(const char *name) {
    const char *value = getenv(name);
    char *end;
    if (!value || *value < '0' || *value > '9') return 0;
    errno = 0;
    unsigned long long result = strtoull(value, &end, 10);
    return errno || *end ? 0 : result;
}

__attribute__((constructor)) static void initialize(void) {
    real_start = (StartTimer)dlsym(RTLD_NEXT, "startPowerSaveTimer");
    unsigned long long pid = env_number("KINAMP_BT_GUARD_PID");
    unsigned long long seconds = env_number("KINAMP_BT_GUARD_SECONDS");
    owner_start = env_number("KINAMP_BT_GUARD_STARTTIME");
    status_path = getenv("KINAMP_BT_GUARD_STATUS");
    binding_path = getenv("KINAMP_BT_GUARD_BINDING");
    double now = monotonic_now();
    enabled = real_start && pid && pid <= 0x7fffffff && owner_start &&
        seconds && seconds <= 3600 && status_path && status_path[0] == '/' && now >= 0;
    if (binding_path && binding_path[0] == '/' && real_start && status_path &&
            status_path[0] == '/' && now >= 0) enabled = 1;
    owner_pid = (long)pid;
    expires = now + seconds;
#ifndef KINAMP_BT_GUARD_HOST_TEST
    Dl_info library;
    if (real_start && dladdr((void *)real_start, &library) &&
            (uintptr_t)real_start - (uintptr_t)library.dli_fbase == 0x133e9) {
        disconnect_callback = (TimerCallback)((uintptr_t)library.dli_fbase + 0xdac9);
    }
#endif
}

static int owner_is_alive(void) {
    char path[64], line[2048];
    snprintf(path, sizeof(path), "/proc/%ld/stat", owner_pid);
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char *read = fgets(line, sizeof(line), file);
    fclose(file);
    if (!read) return 0;
    /* comm can itself contain spaces and parentheses. Fields after its last
     * closing parenthesis begin with state (field 3), then starttime is 22. */
    char *tail = strrchr(line, ')');
    if (!tail) return 0;
    char *save = NULL, *token = strtok_r(tail + 1, " ", &save);
    for (int field = 3; token && field <= 22; ++field) {
        if (field == 3 && (*token == 'Z' || *token == 'X')) return 0;
        if (field == 22) {
            char *end;
            unsigned long long start = strtoull(token, &end, 10);
            return !*end && start == owner_start;
        }
        token = strtok_r(NULL, " ", &save);
    }
    return 0;
}

static int owner_is_playing(void) {
    double now = monotonic_now();
    if (binding_path) {
        /* Root periodically revalidates the executable, credentials and start
         * clock. ACS needs only this read-only, short-lived identity handoff;
         * bluetooth cannot necessarily inspect a root player's /proc/exe. */
        int binding = open(binding_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (binding < 0) return 0;
        struct stat st;
        char bytes[256], extra;
        long pid = 0;
        unsigned long long start = 0;
        double until = 0;
#ifdef KINAMP_BT_GUARD_HOST_TEST
        uid_t uid = geteuid();
#else
        uid_t uid = 0;
#endif
        int valid = !fstat(binding, &st) && S_ISREG(st.st_mode) && st.st_uid == uid &&
            !(st.st_mode & 0222) && st.st_size > 0 && st.st_size < (off_t)sizeof(bytes);
        ssize_t count = valid ? read(binding, bytes, sizeof(bytes) - 1) : -1;
        close(binding);
        if (count <= 0) return 0;
        bytes[count] = 0;
        if (sscanf(bytes, "pid=%ld\nstart=%llu\nuntil=%lf\n%c", &pid, &start, &until, &extra) != 3 ||
                pid < 2 || pid > 0x7fffffff || !start || !(until > now) || until > now + 15.1) return 0;
        owner_pid = pid;
        owner_start = start;
        expires = until;
    }
    if (!enabled || now < 0 || now >= expires || !owner_is_alive()) return 0;
    int fd = open(status_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    struct stat st;
#ifdef KINAMP_BT_GUARD_HOST_TEST
    uid_t expected_uid = geteuid();
#else
    uid_t expected_uid = 0;
#endif
    time_t wall = time(NULL);
    int valid = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == expected_uid &&
        st.st_size > 0 && st.st_size < 4096 && wall != (time_t)-1 &&
        wall >= st.st_mtime && wall - st.st_mtime <= 3;
    FILE *file = valid ? fdopen(fd, "r") : NULL;
    if (!file) {
        close(fd);
        return 0;
    }
    char line[4096];
    int pid_found = 0, playing = 0, daemon = 0;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "pid=", 4)) {
            char *end;
            long pid = strtol(line + 4, &end, 10);
            pid_found = pid == owner_pid && !strcmp(end, "\n");
        } else if (!strncmp(line, "state=", 6)) {
            playing = !strcmp(line, "state=playing\n");
        } else if (!strncmp(line, "daemon=", 7)) {
            daemon = !strcmp(line, "daemon=1\n");
        }
    }
    int failed = ferror(file);
    fclose(file);
    return !failed && pid_found && playing && daemon;
}

static int on_deadline(void *data) {
    if (owner_is_playing()) {
        fprintf(stderr, "kinamp-bt-idle-guard: retained link at idle deadline\n");
        /* GLib repeats this same source at the unchanged firmware interval.
         * Genuine native Started events and link loss still remove the source.
         * No periodic poll, fabricated event, or persistent override is needed. */
        return 1;
    }
    fprintf(stderr, "kinamp-bt-idle-guard: delegated idle deadline\n");
    return disconnect_callback(data);
}

int startPowerSaveTimer(TimerCallback callback, void *data) {
    if (!real_start) return 0;
#ifdef KINAMP_BT_GUARD_HOST_TEST
    int matching = 1;
    disconnect_callback = callback;
#else
    /* Immutable after initialization: no cross-thread callback ownership.
     * Unknown callbacks/data keep the stock behavior. */
    int matching = disconnect_callback && callback == disconnect_callback;
#endif
    if (!enabled || !matching || data || !callback) return real_start(callback, data);
    return real_start(on_deadline, NULL);
}
