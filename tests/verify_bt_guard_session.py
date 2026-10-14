#!/usr/bin/env python3
"""Disposable process/job tests; no Kindle commands or Bluetooth access.

Only compile-time firmware paths/hashes/UID gates are modeled. The real C
fork, exec, pipe EOF, ownership, mapping, timeout and restoration code executes.
"""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time


def clock(pid):
    return int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19])


def alive(pid):
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] not in ("Z", "X")
    except FileNotFoundError:
        return False


def job_helper(action):
    root = Path(os.environ["MODEL_DIR"])
    state = root / "job.json"
    data = json.loads(state.read_text())
    with (root / "actions").open("a") as log:
        log.write(f"{time.monotonic():.6f} {action}\n")
    if action == "power":
        count = int((root / "power-count").read_text()) + 1
        (root / "power-count").write_text(str(count))
        if count == 2 and (root / "fence-power").exists():
            (root / "at-fence").touch()
            while not (root / "release").exists():
                time.sleep(0.01)
        print((root / "power").read_text(), end="")
    elif action == "playback":
        print((root / "playback").read_text(), end="")
    elif action == "status":
        if (root / "unknown").exists():
            print("acsbtfd start/starting, unowned")
        elif data["running"]:
            print(f'acsbtfd start/running, process {data["pid"]}')
        else:
            print("acsbtfd stop/waiting")
    elif action == "stop":
        assert data["running"]
        assert clock(data["pid"]) == data["clock"]
        os.kill(data["pid"], signal.SIGTERM)
        state.write_text(json.dumps({"running": False}))
        if (root / "fence-stop").exists():
            (root / "at-fence").touch()
            while not (root / "release").exists():
                time.sleep(0.01)
        if (root / "partial-stop").exists():
            return 1
        print("acsbtfd stop/waiting")
    elif action == "start":
        assert not data["running"]
        p = subprocess.Popen([os.environ["MODEL_WORKER"], "-f"],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        state.write_text(json.dumps({"running": True, "pid": p.pid, "clock": clock(p.pid)}))
        with (root / "created-pids").open("a") as f:
            f.write(f"{p.pid} {clock(p.pid)}\n")
        print(f"acsbtfd start/running, process {p.pid}")
    else:
        raise AssertionError(action)
    return 0


def wait_for(check, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(0.02)
    raise AssertionError("timed out waiting for modeled recovery")


def main():
    # Reap detached fixture/watchdog children instead of leaving zombies at PID1.
    libc = ctypes.CDLL(None)
    assert libc.prctl(36, 1, 0, 0, 0) == 0  # PR_SET_CHILD_SUBREAPER
    source = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="bt-guard-session-") as temporary:
        root = Path(temporary)
        worker = root / "acs-model"
        worker_source = root / "worker.c"
        worker_source.write_text('''#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    if (getenv("MODEL_OMIT_GUARD") && getenv("LD_PRELOAD")) {
        unsetenv("LD_PRELOAD");
        execl("/proc/self/exe", "/proc/self/exe", "-f", (char *)NULL);
        return 1;
    }
    char path[4096], cwd[4096];
    snprintf(path, sizeof(path), "%s/worker-%ld", getenv("MODEL_DIR"), (long)getpid());
    FILE *f = fopen(path, "w"); if (!f || !getcwd(cwd, sizeof(cwd))) return 1;
    mode_t mask = umask(0); umask(mask);
    fprintf(f, "%lu %lu %o\\n%s\\n%s\\n", (unsigned long)geteuid(),
        (unsigned long)getegid(), (unsigned)mask, cwd, getenv("MODEL_KEEP"));
    fclose(f); for (;;) pause();
}
''')
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", str(worker_source), "-o", str(worker)], check=True)
        guard = root / "guard.so"
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-fPIC", "-shared",
                        str(source / "kindle_bt_idle_guard.c"), "-ldl", "-o", str(guard)], check=True)
        guard.chmod(0o644)
        checksum = hashlib.sha256(guard.read_bytes()).hexdigest()
        alias = root / "tmp-alias"
        alias.symlink_to(root, target_is_directory=True)
        controller = root / "session"
        subprocess.run(["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-DGUARD_SESSION_HOST_TEST", f'-DACS_EXE="{worker}"',
                        str(source / "kindle_bt_guard_session.c"), "-o", str(controller)], check=True)
        helper = root / "helper"
        helper.write_text(f"#!/bin/sh\nexec {sys.executable} {Path(__file__).resolve()} --job \"$@\"\n")
        helper.chmod(0o755)
        cases = ["normal-expiry", "normal-signal", "controller-killed", "death-before-stop",
                 "death-during-stop", "partial-stop", "worker-crash", "watchdog-killed", "wrong-startclock",
                 "unknown-initial-job", "originally-stopped", "baseline-nonzero",
                 "native-baseline-nonzero",
                 "unknown-on-restore", "externally-started-job", "wrong-job-owner",
                 "symlink-directory", "missing-guard-mapping"]
        for case in cases:
            directory = root / case
            directory.mkdir()
            env = {**os.environ, "MODEL_DIR": str(directory), "MODEL_WORKER": str(worker),
                   "MODEL_KEEP": "original-worker-environment"}
            if case == "missing-guard-mapping":
                env["MODEL_OMIT_GUARD"] = "1"
            original = subprocess.Popen([str(worker), "-f"], env=env)
            player = subprocess.Popen(["sleep", "60"])
            outsider = subprocess.Popen(["sleep", "60"])
            process = None
            owned = [(original.pid, clock(original.pid)), (player.pid, clock(player.pid)),
                     (outsider.pid, clock(outsider.pid))]
            log = directory / "session.log"
            try:
                state = {"running": True, "pid": original.pid, "clock": clock(original.pid)}
                if case == "originally-stopped":
                    state = {"running": False}
                if case == "wrong-job-owner":
                    state = {"running": True, "pid": outsider.pid, "clock": clock(outsider.pid)}
                (directory / "job.json").write_text(json.dumps(state))
                (directory / "power").write_text("1\n" if case == "baseline-nonzero" else "0\n")
                (directory / "playback").write_text("1\n" if case == "native-baseline-nonzero" else "0\n")
                (directory / "power-count").write_text("0")
                if case == "unknown-initial-job":
                    (directory / "unknown").touch()
                if case in ("death-before-stop", "death-during-stop"):
                    (directory / ("fence-power" if case == "death-before-stop" else "fence-stop")).touch()
                if case == "partial-stop":
                    (directory / "partial-stop").touch()
                expected_clock = clock(original.pid) + (1 if case == "wrong-startclock" else 0)
                guard_argument = alias / "guard.so" if case == "symlink-directory" else guard
                with log.open("w") as stream:
                    process = subprocess.Popen([str(controller), "2" if case == "normal-expiry" else "20",
                                                str(original.pid), str(expected_clock), str(player.pid),
                                                str(clock(player.pid)), str(guard_argument), checksum, str(helper)],
                                               env=env, stdout=stream, stderr=stream)
                rejected = case in ("wrong-startclock", "unknown-initial-job", "originally-stopped",
                                    "baseline-nonzero", "native-baseline-nonzero", "wrong-job-owner")
                if rejected:
                    assert process.wait(timeout=5) != 0
                    actions = (directory / "actions").read_text()
                    assert " stop\n" not in actions and " start\n" not in actions
                    assert alive(original.pid) and alive(outsider.pid)
                elif case in ("death-before-stop", "death-during-stop"):
                    wait_for(lambda: (directory / "at-fence").exists())
                    process.kill()
                    process.wait(timeout=2)
                    (directory / "release").touch()
                    if case == "death-before-stop":
                        wait_for(lambda: len((directory / "actions").read_text().splitlines()) >= 4)
                        time.sleep(0.2)
                        assert alive(original.pid)
                        assert " stop\n" not in (directory / "actions").read_text()
                    else:
                        wait_for(lambda: "RESTORED" in log.read_text())
                elif case == "partial-stop":
                    assert process.wait(timeout=5) == 4
                    assert "RESTORED" in log.read_text()
                elif case == "missing-guard-mapping":
                    assert process.wait(timeout=8) == 4
                    assert "READINESS FAILED" in log.read_text()
                    assert "ACTIVE" not in log.read_text()
                    assert "RESTORED" in log.read_text()
                else:
                    wait_for(lambda: "ACTIVE" in log.read_text())
                    match = re.search(r"ACTIVE watchdog=(\d+) child=(\d+) start=(\d+)", log.read_text())
                    watchdog, child, child_clock = map(int, match.groups())
                    assert clock(child) == child_clock and alive(child)
                    wait_for(lambda: (directory / f"worker-{child}").exists())
                    assert (directory / f"worker-{child}").read_text() == (directory / f"worker-{original.pid}").read_text()
                    if case == "controller-killed":
                        process.kill()
                        process.wait(timeout=2)
                        wait_for(lambda: "RESTORED" in log.read_text())
                    elif case == "worker-crash":
                        os.kill(child, signal.SIGKILL)
                        assert process.wait(timeout=5) == 0
                    elif case == "watchdog-killed":
                        os.kill(watchdog, signal.SIGKILL)
                        assert process.wait(timeout=5) == 3
                        wait_for(lambda: not alive(child))
                        assert not json.loads((directory / "job.json").read_text())["running"]
                        # Deliberately not automatic recovery: the operator must
                        # verify child gone + known stopped job before starting.
                        subprocess.run([str(helper), "start"], env=env, check=True, stdout=subprocess.DEVNULL)
                    elif case == "unknown-on-restore":
                        (directory / "unknown").touch()
                        process.terminate()
                        assert process.wait(timeout=5) == 2
                        assert "RESTORE BLOCKED" in log.read_text()
                        assert " start\n" not in (directory / "actions").read_text()
                    elif case == "externally-started-job":
                        subprocess.run([str(helper), "start"], env=env, check=True, stdout=subprocess.DEVNULL)
                        external = json.loads((directory / "job.json").read_text())["pid"]
                        external_clock = clock(external)
                        process.terminate()
                        assert process.wait(timeout=5) == 0
                        assert alive(external) and clock(external) == external_clock
                        assert (directory / "actions").read_text().count(" start\n") == 1
                    else:
                        if case in ("normal-signal", "symlink-directory"):
                            process.terminate()
                        assert process.wait(timeout=6) == 0
                    assert not alive(child)
                    wait_for(lambda: not alive(watchdog))
                assert alive(outsider.pid) and alive(player.pid)
                if not rejected and case not in ("death-before-stop", "unknown-on-restore"):
                    restored = json.loads((directory / "job.json").read_text())
                    assert restored["running"] and alive(restored["pid"])
                    assert restored["pid"] != original.pid
                    assert clock(restored["pid"]) == restored["clock"]
                    armed = float(re.search(r"bt-guard-session ([0-9.]+).*ARMED", log.read_text()).group(1))
                    stop_time = float(next(l.split()[0] for l in (directory / "actions").read_text().splitlines()
                                           if l.endswith(" stop")))
                    assert armed <= stop_time
                print(f"PASS session {case}")
            finally:
                (directory / "release").touch()
                if process and process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)
                armed = re.search(r"pid=(\d+): ARMED", log.read_text())
                if armed:
                    wait_for(lambda: not alive(int(armed.group(1))))
                created = directory / "created-pids"
                if created.exists():
                    owned.extend(tuple(map(int, l.split())) for l in created.read_text().splitlines())
                for pid, expected in owned:
                    if alive(pid):
                        assert clock(pid) == expected
                        os.kill(pid, signal.SIGTERM)
                original.wait(timeout=5)
                player.wait(timeout=5)
                outsider.wait(timeout=5)
                time.sleep(0.05)
                while True:
                    try:
                        if os.waitpid(-1, os.WNOHANG)[0] == 0:
                            break
                    except ChildProcessError:
                        break
        print("PASS real host process ownership, normal/death/partial-stop recovery; no device changes")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--job":
        sys.exit(job_helper(sys.argv[2]))
    main()
