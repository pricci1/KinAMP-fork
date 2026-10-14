/* Diagnostic or explicitly enabled experimental dogfooding session.
 * The detached watchdog owns its unreaped direct child, never an external PID.
 * Losing the controller's pipe restores the originally running Upstart job.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ACS_EXE
#define ACS_EXE "/usr/bin/acsbtfd"
#endif
#define ACS_SHA "70e32455351761c90356b8640bc196201af00dbc86471f0f6e8315ca12c35528"
#define ACS_LIB_SHA "569865512abd5ad74bface0a8d01123d695639955ac5dc3bf288d65094f10774"
#define ACS_JOB_SHA "6e4336d6c9ac3cc9bf062374eed397a934115055690b03eef70c2d2810c05170"
#define PLAYER_SHA "e2b1466a5514963e56818a58e6ecb898140eb778b14bb4796ae76ae521fbcde6"
static volatile sig_atomic_t interrupted;
static const char *guard_path, *guard_sha;
static pid_t stock_worker, player;
static unsigned long long stock_clock, player_clock;
static unsigned seconds;
static const char *runtime;
static int lock_fd = -1, fifo_fd = -1;
#ifndef PLAYER_STATUS
#define PLAYER_STATUS "/tmp/kinamp_status"
#endif
#ifdef GUARD_SESSION_HOST_TEST
static const char *helper;
#endif

static void signal_stop(int sig) { (void)sig; interrupted = 1; }
static double now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void note(const char *message) {
    fprintf(stderr, "bt-guard-session %.3f pid=%ld: %s\n", now(), (long)getpid(), message);
}
static int identity(pid_t pid, unsigned long long *clock, pid_t *parent) {
    char path[80], line[4096], *tail, *save, *token;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok || !(tail = strrchr(line, ')'))) return 0;
    token = strtok_r(tail + 1, " ", &save);
    for (int field = 3; token && field <= 22; ++field) {
        if (field == 3 && (*token == 'Z' || *token == 'X')) return 0;
        if (field == 4) *parent = (pid_t)strtol(token, NULL, 10);
        if (field == 22) {
            char *end;
            *clock = strtoull(token, &end, 10);
            return *clock && !*end;
        }
        token = strtok_r(NULL, " ", &save);
    }
    return 0;
}
static int same(pid_t pid, unsigned long long expected) {
    unsigned long long clock; pid_t parent;
    return identity(pid, &clock, &parent) && clock == expected;
}
/* These short job/property/checksum commands are bounded, including failure.
 * Each is also our direct child: a timeout cannot signal a recycled PID. */
static int command(char *const argv[], char *out, size_t capacity) {
    int pipefd[2], status = 0; size_t used = 0;
    if (pipe2(pipefd, O_CLOEXEC)) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return 0; }
    if (!pid) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execvp(argv[0], argv); _exit(127);
    }
    close(pipefd[1]); fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    double until = now() + 8;
    for (;;) {
        ssize_t n = read(pipefd[0], out + used, capacity - used - 1);
        if (n > 0) used += (size_t)n;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            while (used + 1 < capacity && (n = read(pipefd[0], out + used, capacity - used - 1)) > 0)
                used += (size_t)n;
            close(pipefd[0]); out[used] = 0;
            return WIFEXITED(status) && !WEXITSTATUS(status);
        }
        if (result < 0 || now() >= until || used + 1 == capacity) {
            if (result == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
            close(pipefd[0]); out[used] = 0; return 0;
        }
        usleep(20000);
    }
}
static int job_command(const char *action, char *out, size_t size) {
#ifdef GUARD_SESSION_HOST_TEST
    char *argv[] = {(char *)helper, (char *)action, "acsbtfd", NULL};
#else
    char path[64]; snprintf(path, sizeof(path), "/sbin/%s", action);
    char *argv[] = {path, "acsbtfd", NULL};
#endif
    return command(argv, out, size);
}
/* Only these exact stable Upstart states are actionable; unknown is not off. */
static int job_state(pid_t *pid) {
    char out[512]; long parsed; int consumed = 0;
    if (!job_command("status", out, sizeof(out))) return -1;
    if (!strcmp(out, "acsbtfd stop/waiting\n")) return 0;
    if (sscanf(out, "acsbtfd start/running, process %ld%n", &parsed, &consumed) == 1 &&
            parsed > 1 && parsed <= 0x7fffffff && !strcmp(out + consumed, "\n")) {
        *pid = (pid_t)parsed; return 1;
    }
    return -1;
}
static int baseline_zero(void) {
    char out[128];
#ifdef GUARD_SESSION_HOST_TEST
    char *argv[] = {(char *)helper, "power", NULL};
    char *playback[] = {(char *)helper, "playback", NULL};
#else
    char *argv[] = {"/usr/bin/lipc-get-prop", "com.lab126.powerd", "preventScreenSaver", NULL};
    char *playback[] = {"/usr/bin/lipc-get-prop", "com.lab126.playermgr", "InPlayback", NULL};
#endif
    return command(argv, out, sizeof(out)) && !strcmp(out, "0\n") &&
        command(playback, out, sizeof(out)) && !strcmp(out, "0\n");
}
static int hash_is(const char *path, const char *expected) {
    char out[1024];
#ifdef GUARD_SESSION_HOST_TEST
    char *argv[] = {"sha256sum", (char *)path, NULL};
#else
    char *argv[] = {"/usr/bin/sha256sum", (char *)path, NULL};
#endif
    return strlen(expected) == 64 && command(argv, out, sizeof(out)) &&
        !strncmp(out, expected, 64) && out[64] == ' ';
}
static int executable_is(pid_t pid, const char *expected_path) {
    char path[80], link[1024];
    snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n < 0) return 0;
    link[n] = 0; return !strcmp(link, expected_path);
}
static int acs_absent(void) {
    DIR *proc = opendir("/proc"); struct dirent *entry;
    if (!proc) return 0;
    int absent = 1;
    while ((entry = readdir(proc))) {
        if (strspn(entry->d_name, "0123456789") != strlen(entry->d_name)) continue;
        char path[80], link[1024];
#ifdef GUARD_SESSION_HOST_TEST
        /* Unprivileged disposable harness cannot inspect unrelated root jobs.
         * Target runs as root and must fail on any unreadable process. */
        struct stat directory;
        snprintf(path, sizeof(path), "/proc/%ld", strtol(entry->d_name, NULL, 10));
        if (stat(path, &directory) || directory.st_uid != geteuid()) continue;
#endif
        snprintf(path, sizeof(path), "/proc/%ld/exe", strtol(entry->d_name, NULL, 10));
        ssize_t n = readlink(path, link, sizeof(link) - 1);
        if (n < 0) {
#ifndef GUARD_SESSION_HOST_TEST
            if (errno != ENOENT && errno != ESRCH) absent = 0;
#endif
            continue;
        }
        link[n] = 0;
        if (!strcmp(link, ACS_EXE) || !strcmp(link, ACS_EXE " (deleted)")) absent = 0;
    }
    closedir(proc); return absent;
}
/* Environment stays private in memory. Add to, rather than erase, any original
 * preloads. Use original worker credentials/groups, not a root ACS daemon. */
static char **worker_environment(void) {
    char path[80], bytes[131073]; size_t length;
    snprintf(path, sizeof(path), "/proc/%ld/environ", (long)stock_worker);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    length = fread(bytes, 1, sizeof(bytes), f);
    int failed = ferror(f); fclose(f);
    if (failed || !length || length == sizeof(bytes) || bytes[length - 1]) return NULL;
    char **env = calloc(length + 8, sizeof(*env)); if (!env) return NULL;
    size_t count = 0;
    for (size_t i = 0; i < length; i += strlen(bytes + i) + 1) {
        if (!strncmp(bytes + i, "LD_PRELOAD=", 11)) {
            if (asprintf(&env[count++], "LD_PRELOAD=%s:%s", guard_path, bytes + i + 11) < 0) return NULL;
        } else if (strncmp(bytes + i, "KINAMP_BT_GUARD_", 16)) env[count++] = strdup(bytes + i);
    }
    int preload = 0;
    for (size_t i = 0; i < count; ++i) if (!strncmp(env[i], "LD_PRELOAD=", 11)) preload = 1;
    if (!preload && asprintf(&env[count++], "LD_PRELOAD=%s", guard_path) < 0) return NULL;
    if (asprintf(&env[count++], "KINAMP_BT_GUARD_PID=%ld", (long)player) < 0 ||
            asprintf(&env[count++], "KINAMP_BT_GUARD_STARTTIME=%llu", player_clock) < 0 ||
            asprintf(&env[count++], "KINAMP_BT_GUARD_SECONDS=%u", seconds) < 0) return NULL;
    env[count++] = strdup("KINAMP_BT_GUARD_STATUS=" PLAYER_STATUS);
    if (runtime && asprintf(&env[count++], "KINAMP_BT_GUARD_BINDING=%s/binding", runtime) < 0) return NULL;
    return env;
}
static int credentials(uid_t *uid, gid_t *gid, gid_t *groups, size_t *count, mode_t *mask) {
    char path[80], line[4096]; unsigned a, b, c, d;
    int have_uid = 0, have_gid = 0, have_groups = 0, have_mask = 0;
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)stock_worker);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Uid: %u %u %u %u", &a, &b, &c, &d) == 4 && a == b && b == c && c == d)
            { *uid = a; have_uid = 1; }
        if (sscanf(line, "Gid: %u %u %u %u", &a, &b, &c, &d) == 4 && a == b && b == c && c == d)
            { *gid = a; have_gid = 1; }
        if (sscanf(line, "Umask: %o", &a) == 1 && a <= 0777)
            { *mask = a; have_mask = 1; }
        if (!strncmp(line, "Groups:", 7)) {
            char *p = line + 7, *end; *count = 0; have_groups = 1;
            while (*p && *p != '\n') {
                unsigned long value = strtoul(p, &end, 10);
                if (p == end) break;
                if (*count == 64) { have_groups = 0; break; }
                groups[(*count)++] = (gid_t)value; p = end;
            }
        }
    }
    fclose(f); return have_uid && have_gid && have_groups && have_mask;
}
static int mapped(pid_t pid) {
    char path[80], line[4096];
    /* /tmp is a symlink to /var/tmp on this firmware; maps uses the resolved
     * name. Keep the original lstat/hash gates, but compare canonical paths. */
    char *canonical = realpath(guard_path, NULL);
    if (!canonical) return 0;
    snprintf(path, sizeof(path), "/proc/%ld/maps", (long)pid);
    FILE *f = fopen(path, "r"); if (!f) { free(canonical); return 0; }
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '/');
        if (p && !strncmp(p, canonical, strlen(canonical)) && p[strlen(canonical)] == '\n') ok = 1;
    }
    fclose(f); free(canonical); return ok;
}
static int restore(pid_t child, int stopped) {
    char out[512]; pid_t job_pid = 0;
    if (child > 0) {
        /* Never reaped before this point. The kernel cannot reuse this PID.
         * No pid-file or process-name based kill is permitted. */
        kill(child, SIGTERM);
        double deadline = now() + 3; int status;
        while (now() < deadline) {
            pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) { child = 0; break; }
            if (result < 0) { note("RESTORE BLOCKED: lost child ownership"); return 0; }
            usleep(20000);
        }
        if (child) { kill(child, SIGKILL); if (waitpid(child, &status, 0) != child) return 0; }
    }
    if (!stopped) return 1;
    int state = job_state(&job_pid);
    if (state == 1) { note("stock job already running; left untouched"); return 1; }
    if (state != 0) { note("RESTORE BLOCKED: unknown job state; no start/kill"); return 0; }
    if (!acs_absent()) { note("RESTORE BLOCKED: unowned ACS or unreadable process"); return 0; }
#ifndef GUARD_SESSION_HOST_TEST
    if (!hash_is(ACS_EXE, ACS_SHA) || !hash_is("/usr/lib/libacsbtfdlib.so.1.0", ACS_LIB_SHA) ||
            !hash_is("/etc/upstart/acsbtfd.conf", ACS_JOB_SHA)) {
        note("RESTORE BLOCKED: changed stock files"); return 0;
    }
#endif
    if (!job_command("start", out, sizeof(out)) || job_state(&job_pid) != 1) {
        note("RESTORE FAILED: original job start"); return 0;
    }
    note("RESTORED originally running stock job"); return 1;
}
static int runtime_file(const char *name, const char *bytes, mode_t mode) {
    char path[1024], temporary[1024];
    if (snprintf(path, sizeof(path), "%s/%s", runtime, name) >= (int)sizeof(path) ||
            snprintf(temporary, sizeof(temporary), "%s/.%s-%ld", runtime, name, (long)getpid()) >= (int)sizeof(temporary)) return 0;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return 0;
    size_t length = strlen(bytes);
    int ok = write(fd, bytes, length) == (ssize_t)length && !fchmod(fd, mode);
    close(fd);
    if (ok) ok = !rename(temporary, path);
    if (!ok) unlink(temporary);
    return ok;
}
static int player_binding(void) {
    /* Status is a player assertion, NOT a security boundary. Root validates
     * the candidate on every refresh; hash is cached only for an unchanged
     * executable inode/metadata AND unchanged PID/start clock. */
    static pid_t cached_pid;
    static unsigned long long cached_clock;
    static struct stat cached_exe;
    static int cached_ok;
    int fd = open(PLAYER_STATUS, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat status, exe;
    long candidate = 0;
    char bytes[4096], path[80];
    if (fd >= 0) {
        if (!fstat(fd, &status) && S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
                status.st_size > 0 && status.st_size < (off_t)sizeof(bytes)) {
            ssize_t n = read(fd, bytes, sizeof(bytes) - 1);
            if (n > 0) {
                bytes[n] = 0;
                char *save, *line = strtok_r(bytes, "\n", &save);
                for (; line; line = strtok_r(NULL, "\n", &save)) {
                    char extra;
                    if (sscanf(line, "pid=%ld%c", &candidate, &extra) == 1) break;
                    candidate = 0;
                }
            }
        }
        close(fd);
    }
    unsigned long long clock = 0; pid_t parent;
    int valid = candidate > 1 && candidate <= 0x7fffffff && identity((pid_t)candidate, &clock, &parent);
    if (valid) {
        snprintf(path, sizeof(path), "/proc/%ld/status", candidate);
        FILE *f = fopen(path, "r");
        unsigned a, b, c, d; int root = 0;
        while (f && fgets(bytes, sizeof(bytes), f)) {
            if (sscanf(bytes, "Uid:\t%u\t%u\t%u\t%u", &a, &b, &c, &d) == 4)
                root = a == geteuid() && b == a && c == a && d == a;
        }
        if (f) fclose(f);
        snprintf(path, sizeof(path), "/proc/%ld/exe", candidate);
        valid = root && !stat(path, &exe);
    }
    if (valid && (cached_pid != candidate || cached_clock != clock ||
            cached_exe.st_dev != exe.st_dev || cached_exe.st_ino != exe.st_ino ||
            cached_exe.st_size != exe.st_size ||
            cached_exe.st_mtim.tv_sec != exe.st_mtim.tv_sec || cached_exe.st_mtim.tv_nsec != exe.st_mtim.tv_nsec ||
            cached_exe.st_ctim.tv_sec != exe.st_ctim.tv_sec || cached_exe.st_ctim.tv_nsec != exe.st_ctim.tv_nsec)) {
#ifdef GUARD_SESSION_HOST_TEST
        const char *expected = getenv("MODEL_PLAYER_SHA");
        cached_ok = expected && hash_is(path, expected);
#else
        cached_ok = hash_is(path, PLAYER_SHA);
#endif
        cached_pid = (pid_t)candidate; cached_clock = clock; cached_exe = exe;
    }
    valid = valid && cached_ok && same((pid_t)candidate, clock);
    snprintf(bytes, sizeof(bytes), "pid=%ld\nstart=%llu\nuntil=%.3f\n",
        valid ? candidate : 0, valid ? clock : 0, now() + 15);
    return runtime_file("binding", bytes, 0444);
}
static int watchdog(int control, int ready) {
    pid_t job_pid = 0, parent; unsigned long long clock;
    uid_t uid = 0; gid_t gid = 0, groups[64]; size_t group_count = 0;
    mode_t mask = 0; char path[80], cmdline[1024];
    char **env; char out[512]; int stopped = 0, active = 0; pid_t child = 0;
    if (setsid() < 0 || job_state(&job_pid) != 1 || !baseline_zero() ||
            !identity(stock_worker, &clock, &parent) || clock != stock_clock ||
            (stock_worker != job_pid && parent != job_pid) || !same(player, player_clock) ||
            !executable_is(stock_worker, ACS_EXE) ||
            !credentials(&uid, &gid, groups, &group_count, &mask) || !(env = worker_environment())) {
        note("ABORT before stop: identity/job/power/environment gate"); return 1;
    }
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)stock_worker);
    FILE *f = fopen(path, "rb"); if (!f) return 1;
    size_t length = fread(cmdline, 1, sizeof(cmdline), f); fclose(f);
    if (length != sizeof(ACS_EXE "\0-f") || memcmp(cmdline, ACS_EXE "\0-f", length)) return 1;
    snprintf(path, sizeof(path), "/proc/%ld/cwd", (long)stock_worker);
    if (chdir(path)) return 1;
#ifndef GUARD_SESSION_HOST_TEST
    char proc_exe[80]; snprintf(proc_exe, sizeof(proc_exe), "/proc/%ld/exe", (long)player);
    if (geteuid() != 0 || uid != 1003 || gid != 150 ||
            !hash_is(ACS_EXE, ACS_SHA) || !hash_is(proc_exe, PLAYER_SHA) ||
            !hash_is("/usr/lib/libacsbtfdlib.so.1.0", ACS_LIB_SHA) ||
            !hash_is("/etc/upstart/acsbtfd.conf", ACS_JOB_SHA)) {
        note("ABORT before stop: firmware/player/credential gate"); return 1;
    }
#endif
    struct stat st;
    if (lstat(guard_path, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 022) || !hash_is(guard_path, guard_sha)) {
        note("ABORT before stop: guard file gate"); return 1;
    }
    if (runtime) {
        char bytes[256];
        snprintf(bytes, sizeof(bytes), "originally_running=1\nstock_worker=%ld\nstock_start=%llu\n",
            (long)stock_worker, stock_clock);
        if (!runtime_file("ownership", bytes, 0600) || !player_binding()) return 1;
    }
    note("ARMED independent watchdog before stock stop");
    if (write(ready, "R", 1) != 1) return 1;
    close(ready);
    char action;
    if (read(control, &action, 1) != 1 || action != 'B' || interrupted) return 1;
    /* Recheck after arm; refuse a changed/reused worker, job or power baseline. */
    if (job_state(&job_pid) != 1 || !identity(stock_worker, &clock, &parent) ||
            clock != stock_clock || (stock_worker != job_pid && parent != job_pid) ||
            !baseline_zero() || !same(player, player_clock)) return 1;
    struct pollfd preflight = {control, POLLIN | POLLHUP, 0};
    if (interrupted || poll(&preflight, 1, 0) > 0) return 1;
    double deadline = now() + seconds;
    /* A failed/partial stop may still have changed the job: inspect on cleanup. */
    stopped = 1;
    if (!job_command("stop", out, sizeof(out)) || job_state(&job_pid) != 0) goto cleanup;
    if (interrupted || poll(&preflight, 1, 0) > 0) goto cleanup;
    pid_t expected_parent = getpid();
    child = fork();
    if (child < 0) { child = 0; goto cleanup; }
    if (!child) {
        close(control);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        umask(mask);
        if (geteuid() == 0 && (setgroups(group_count, groups) || setgid(gid) || setuid(uid))) _exit(126);
        /* Set this AFTER privilege drop, which otherwise clears PDEATHSIG. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != expected_parent) _exit(126);
        char *argv[] = {ACS_EXE, "-f", NULL}; execve(ACS_EXE, argv, env); _exit(127);
    }
    /* Do not waitpid/reap this child until restoration; ownership is immutable. */
    double startup = now() + 5; unsigned long long child_clock = 0;
    while (now() < startup && !interrupted) {
        if (identity(child, &child_clock, &parent) && parent == getpid() &&
                executable_is(child, ACS_EXE) && mapped(child)) { active = 1; break; }
        struct pollfd p = {control, POLLIN | POLLHUP, 0};
        if (poll(&p, 1, 20) > 0) goto cleanup;
    }
    if (!active || !same(child, child_clock) || !mapped(child)) {
        active = 0;
        note("READINESS FAILED: child identity/executable/canonical guard mapping");
        goto cleanup;
    }
    fprintf(stderr, "bt-guard-session %.3f ACTIVE watchdog=%ld child=%ld start=%llu\n",
        now(), (long)getpid(), (long)child, child_clock);
    if (runtime) {
        char bytes[256];
        snprintf(bytes, sizeof(bytes), "originally_running=1\nwatchdog=%ld\nacs=%ld\nacs_start=%llu\nstock_worker=%ld\nstock_start=%llu\n",
            (long)getpid(), (long)child, child_clock, (long)stock_worker, stock_clock);
        if (!runtime_file("ownership", bytes, 0600)) goto cleanup;
    }
    double refresh = 0;
    while (!interrupted && (runtime || now() < deadline) && same(child, child_clock)) {
        if (runtime && now() >= refresh) {
            if (!player_binding()) break;
            refresh = now() + 5;
            pid_t external = 0;
            if (job_state(&external) != 0) break; /* never coexist with a restarted stock job */
        }
        struct pollfd p = {control, POLLIN | POLLHUP, 0};
        if (poll(&p, 1, 100) > 0) break; /* normal STOP or controller death/EOF */
    }
cleanup:
    if (runtime) runtime_file("binding", "pid=0\nstart=0\nuntil=0\n", 0444);
    note("restoration begins");
    /* Successful restoration alone is not a successful readiness gate. */
    return restore(child, stopped) ? (active ? 0 : 4) : 2;
}
static int control_operation(const char *action, const char *directory) {
    char path[1024], bytes[512]; struct stat st;
    if (directory[0] != '/' || strlen(directory) > 800 || lstat(directory, &st) ||
            !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 022)) return 1;
    snprintf(path, sizeof(path), "%s/lock", directory);
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 1;
    int idle = !flock(fd, LOCK_EX | LOCK_NB);
    if (!idle && errno != EWOULDBLOCK) { close(fd); return 1; }
    if (!strcmp(action, "--disable") && !idle) {
        snprintf(path, sizeof(path), "%s/control", directory);
        int control = open(path, O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        int ok = control >= 0 && !fstat(control, &st) && S_ISFIFO(st.st_mode) &&
            st.st_uid == geteuid() && !(st.st_mode & 077) && write(control, "S", 1) == 1;
        if (control >= 0) close(control);
        double deadline = now() + 30;
        while (ok && now() < deadline && !(idle = !flock(fd, LOCK_EX | LOCK_NB))) usleep(100000);
        if (!idle) { puts("disable incomplete: supervisor stalled; see recovery instructions"); close(fd); return 2; }
    }
    if (!strcmp(action, "--status")) {
        puts(idle ? "session inactive" : "session supervised (check binding expiry below)");
        snprintf(path, sizeof(path), "%s/binding", directory);
        int binding = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (binding >= 0 && !fstat(binding, &st) && S_ISREG(st.st_mode)) {
            ssize_t n = read(binding, bytes, sizeof(bytes) - 1);
            if (n > 0) { bytes[n] = 0; fputs(bytes, stdout); }
        }
        if (binding >= 0) close(binding);
        printf("monotonic_now=%.3f\n", now());
        pid_t job = 0; int state = job_state(&job);
        printf("stock_job=%s\n", state == 1 ? "running" : state == 0 ? "stopped" : "unknown");
        close(fd); return 0;
    }
    if (!idle) { close(fd); return 2; }
    pid_t job = 0;
    if (job_state(&job) == 1) { puts("stock running; no recovery needed"); close(fd); return 0; }
    if (strcmp(action, "--recover")) {
        puts("stock not verified running; run bt-guard recover with playback stopped"); close(fd); return 2;
    }
    /* Never signal a PID from a manifest. A held lock or ANY live ACS refuses
     * recovery. Manifest proves this install previously replaced a running job;
     * exact stock hashes/state must still match before starting that job. */
    snprintf(path, sizeof(path), "%s/ownership", directory);
    int manifest = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int known = manifest >= 0 && !fstat(manifest, &st) && S_ISREG(st.st_mode) &&
        st.st_uid == geteuid() && !(st.st_mode & 077);
    ssize_t n = known ? read(manifest, bytes, sizeof(bytes) - 1) : -1;
    if (manifest >= 0) close(manifest);
    known = n >= 21 && !memcmp(bytes, "originally_running=1\n", 21);
    runtime = directory;
    int ok = known && baseline_zero() && restore(0, 1);
    close(fd);
    if (!ok) puts("recovery refused: unknown ownership/process/job/files or nonzero power/playback; do not kill unknown processes");
    return ok ? 0 : 2;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
#ifdef GUARD_SESSION_HOST_TEST
    if (argc == 4 && !strncmp(argv[1], "--", 2)) {
        helper = argv[3]; return control_operation(argv[1], argv[2]);
    }
#else
    if (argc == 3 && (!strcmp(argv[1], "--status") || !strcmp(argv[1], "--disable") || !strcmp(argv[1], "--recover"))) {
        if (geteuid() != 0) return 1;
        return control_operation(argv[1], argv[2]);
    }
#endif
    /* Exact args, identities freshly sampled by the approved operator. */
#ifdef GUARD_SESSION_HOST_TEST
    if (argc != 9 && argc != 10) return 64;
    helper = argv[argc - 1];
    if (argc == 10) runtime = argv[8];
#else
    if (argc != 8 && argc != 9) return 64;
    if (argc == 9) runtime = argv[8];
#endif
    char *end;
    seconds = (unsigned)strtoul(argv[1], &end, 10);
    if (*end || (!seconds && !runtime) || seconds > 3600 || (runtime && seconds)) return 64;
    stock_worker = (pid_t)strtol(argv[2], &end, 10); if (*end || stock_worker < 2) return 64;
    stock_clock = strtoull(argv[3], &end, 10); if (*end || !stock_clock) return 64;
    player = (pid_t)strtol(argv[4], &end, 10); if (*end || player < 2) return 64;
    player_clock = strtoull(argv[5], &end, 10); if (*end || !player_clock) return 64;
    guard_path = argv[6]; guard_sha = argv[7];
    if (guard_path[0] != '/' || strlen(guard_sha) != 64 || strspn(guard_sha, "0123456789abcdef") != 64) return 64;
    if (runtime) {
        char path[1024]; struct stat st;
        if (runtime[0] != '/' || strlen(runtime) > 800 || lstat(runtime, &st) ||
                !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 022)) return 64;
        snprintf(path, sizeof(path), "%s/lock", runtime);
        lock_fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB)) return 1;
        snprintf(path, sizeof(path), "%s/control", runtime);
        if (unlink(path) && errno != ENOENT) return 1;
        if (mkfifo(path, 0600)) return 1;
        fifo_fd = open(path, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        if (fifo_fd < 0) return 1;
    }
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
    signal(SIGHUP, runtime ? SIG_IGN : signal_stop);
    signal(SIGPIPE, SIG_IGN);
    int control[2], ready[2];
    if (pipe2(control, O_CLOEXEC) || pipe2(ready, O_CLOEXEC)) return 1;
    pid_t wd = fork();
    if (wd < 0) return 1;
    if (!wd) {
        close(control[1]); close(ready[0]);
        if (fifo_fd >= 0) close(fifo_fd);
        _exit(watchdog(control[0], ready[1]));
    }
    close(control[0]); close(ready[1]); char armed;
    int ok = read(ready[0], &armed, 1) == 1 && armed == 'R'; close(ready[0]);
    if (ok && !interrupted) ok = write(control[1], "B", 1) == 1;
    else ok = 0;
    double deadline = now() + seconds;
    int status;
    while (ok && !interrupted && (runtime || now() < deadline)) {
        pid_t result = waitpid(wd, &status, WNOHANG);
        if (result == wd) { close(control[1]); return WIFEXITED(status) ? WEXITSTATUS(status) : 3; }
        if (result < 0) break;
        if (runtime) {
            char action;
            if (read(fifo_fd, &action, 1) == 1 && action == 'S') break;
        }
        usleep(100000);
    }
    close(control[1]); /* also closes automatically on SIGKILL/controller death */
    if (waitpid(wd, &status, 0) != wd) return 3;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 3;
}
