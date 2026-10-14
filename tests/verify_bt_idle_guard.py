#!/usr/bin/python3
"""Host dynamic-link/real GLib contract check, NOT Kindle acceptance.

The fixture models the examined ACS singleton timer API, not Bluetooth/audio.
All processes and files are owned temporary fixtures, removed on exit.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time

SOURCE = Path(__file__).resolve().parents[1]
CONSUMER = r'''
#include <glib.h>
#include <stdio.h>
static guint timer;
static int disconnected;
int startPowerSaveTimer(GSourceFunc callback, gpointer data) {
    if (timer) return 0;
    timer = g_timeout_add(150, callback, data);
    return 1;
}
void stopPowerSaveTimer(void) {
    if (timer) g_source_remove(timer);
    timer = 0;
}
static gboolean disconnect_link(gpointer data) {
    (void)data;
    disconnected++;
    timer = 0;
    return FALSE;
}
void arm(void) { startPowerSaveTimer(disconnect_link, NULL); }
int count(void) { return disconnected; }
void poll_events(void) {
    while (g_main_context_iteration(NULL, FALSE)) {}
}
'''
DRIVER = r'''
#include <stdio.h>
#include <string.h>
#include <unistd.h>
void arm(void);
void stopPowerSaveTimer(void);
void poll_events(void);
int count(void);
int main(void) {
    setbuf(stdout, NULL);
    char line[32];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcmp(line, "arm\n")) arm();
        if (!strcmp(line, "stop\n")) stopPowerSaveTimer();
        for (int i = 0; i < 220; ++i) {
            poll_events();
            usleep(1000);
        }
        printf("%d\n", count());
    }
    stopPowerSaveTimer();
}
'''


def run(*args):
    subprocess.run(args, check=True, capture_output=True)


def starttime(pid):
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


def main():
    with tempfile.TemporaryDirectory(prefix="kinamp-bt-guard-") as directory:
        work = Path(directory)
        (work / "consumer.c").write_text(CONSUMER)
        (work / "driver.c").write_text(DRIVER)
        glib = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
        run("cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
            str(work / "consumer.c"), "-o", str(work / "consumer.so"), *glib)
        run("cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
            "-DKINAMP_BT_GUARD_HOST_TEST", str(SOURCE / "kindle_bt_idle_guard.c"),
            "-o", str(work / "guard.so"), "-ldl")
        run("cc", "-Wall", "-Wextra", "-Werror", str(work / "driver.c"),
            str(work / "consumer.so"), "-o", str(work / "driver"))
        # Compile the actual firmware-gated path too; it must pass through on
        # this unrelated host consumer, not silently turn on a guessed offset.
        run("cc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
            str(SOURCE / "kindle_bt_idle_guard.c"),
            "-o", str(work / "native-guard.so"), "-ldl")
        owner = subprocess.Popen(["sleep", "60"])
        status = work / "status"

        def write(state="playing", pid=None, daemon=1):
            temporary = work / "status.tmp"
            temporary.write_text(f"pid={pid or owner.pid}\nstate={state}\ndaemon={daemon}\n")
            temporary.replace(status)

        def session(preload=True, **overrides):
            env = dict(os.environ, KINAMP_BT_GUARD_PID=str(owner.pid),
                       KINAMP_BT_GUARD_STARTTIME=starttime(owner.pid),
                       KINAMP_BT_GUARD_STATUS=str(status), KINAMP_BT_GUARD_SECONDS="30")
            if preload:
                env["LD_PRELOAD"] = str(work / "guard.so")
            env.update(overrides)
            return subprocess.Popen([str(work / "driver")], env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, text=True)

        def step(driver, command):
            driver.stdin.write(command + "\n")
            driver.stdin.flush()
            return int(driver.stdout.readline())

        def close(driver):
            driver.stdin.close()
            assert driver.wait(timeout=3) == 0

        try:
            write()
            driver = session(preload=False)
            assert step(driver, "arm") == 1, "baseline must disconnect despite playing status"
            close(driver)
            driver = session()
            assert step(driver, "arm") == 0
            assert step(driver, "arm") == 0, "already armed timer must still be retained"
            assert step(driver, "poll") == 0, "retention must repeat past multiple deadlines"
            write("paused")
            assert step(driver, "poll") == 1, "pause must release at next original deadline"
            for state in ("stopped", "paused"):
                write(state)
                assert step(driver, "arm") == (2 if state == "stopped" else 3)
            write()
            assert step(driver, "arm") == 3
            assert step(driver, "stop") == 3, "native Started/link loss must remove the source"
            write("paused")
            assert step(driver, "poll") == 3, "removed source must not disconnect later"
            write()
            assert step(driver, "arm") == 3, "reconnection must be protected again"
            status.unlink()
            assert step(driver, "poll") == 4, "close/missing status must release"
            write()
            assert step(driver, "arm") == 4
            os.utime(status, (time.time() - 10, time.time() - 10))
            assert step(driver, "poll") == 5, "stale playing status must not hold the link"
            write(pid=owner.pid + 1)
            assert step(driver, "arm") == 6, "another player's status must not hold the link"
            write(daemon=0)
            assert step(driver, "arm") == 7
            close(driver)
            write()
            for overrides in ({"KINAMP_BT_GUARD_STARTTIME": "1"},
                              {"KINAMP_BT_GUARD_PID": "invalid"},
                              {"LD_PRELOAD": str(work / "native-guard.so")}):
                driver = session(**overrides)
                assert step(driver, "arm") == 1, "unknown identity/ABI must pass through"
                close(driver)
            driver = session(KINAMP_BT_GUARD_SECONDS="1")
            assert step(driver, "arm") == 0
            time.sleep(1.1)
            write()
            assert step(driver, "poll") == 1, "hard experiment bound must expire even if fresh"
            close(driver)
            driver = session()
            assert step(driver, "arm") == 0
            owner.terminate()
            owner.wait(timeout=3)
            write()
            assert step(driver, "poll") == 1, "dead owner must not hold link even with fresh status"
            close(driver)
            print("PASS: baseline failure; repeated deadlines; pause/stop/close/stale/crash; "
                  "native cancellation/rearm; reconnect; unowned identity; ABI gate; hard expiry")
        finally:
            if owner.poll() is None:
                owner.terminate()
                owner.wait(timeout=3)


if __name__ == "__main__":
    main()
