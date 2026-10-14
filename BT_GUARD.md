# Experimental PW6 Bluetooth idle workaround

This is an unsupported firmware-specific ACS policy workaround, **not native
playback registration**. On the examined Paperwhite 6 firmware 5.19.3.0.1, ACS
disconnects unrecognized playback after 1200 seconds even while KinAMP narrates
and powerd remains Active. The GTK background button is not involved.

The interposer decorates only the examined ACS disconnect callback. At the
original deadline, fresh `playing`/`daemon=1` player status plus a living,
accepted PID/start identity retains the same GLib source. Otherwise the original
callback runs. Native Started/Stopped events and link-loss cancellation remain
stock. `InPlayback` remains 0: this does not pretend to solve native ownership.
No Bluetooth cycling, timer setting, suspend lease, firmware/job/player edit or
pairing change is made. Existing stock stop-hook writes prevention=0, emits
BTstateChanged=0 and removes its stderr log. Enable requires prevention=0 and
native InPlayback=0. Setup/restoration can disconnect headphones and indirectly
restart audiomgr: **Stop first, not Pause**, then reconnect normally before Play.

## Lifecycle

Owned files are `/var/local/kinamp-bt-guard` (root-only writes). Runtime is
`/var/tmp/kinamp-bt-guard`, root0755 so ACS can traverse it. Binding is root0444;
control FIFO, lock, ownership record and logs are root0600. The detached session
survives SSH disconnect. **There is no boot hook**: after full reboot, stock
behavior returns until explicit enable. No shell profile, player launcher,
Upstart job or firmware binary is changed.

The root watchdog revalidates root credentials, process start clock and accepted
player executable every five seconds. SHA256 is cached only while PID/start and
executable device/inode/size/mtime/ctime remain unchanged. It atomically hands
ACS a binding valid for at most 15 monotonic seconds. ACS checks that file and
`/proc/PID/stat`, not the root player's `/proc/PID/exe`. Accepted player restarts
rebind; absent/dead/different players lose protection. This is an identity-bound
player **assertion**, not an authenticated security lease or evidence of PCM
delivery: original status is group-writable. There is no one-hour cutoff, but
renewal must remain alive. Status freshness is still limited to three seconds.

Pause/stop/close/crash/stalled renewal delegate **at the next original deadline**,
which may be up to 20 minutes later. Nothing forces immediate disconnection or
blocks cover/suspend. A timer already delegated/disconnected must be rearmed by
normal connection events. Polling renewal adds unmeasured battery overhead.

The watchdog is armed before stock stop and owns an unreaped direct ACS child
with stock environment/groups/UID/GID/umask/cwd. Controller death/EOF, disable,
child death, unknown job state or an externally started stock job ends the
session and restores only the originally running stock job, if known stopped,
stock hashes match, and no unowned ACS exists. Already running stock is left
alone. The child has PDEATHSIG after privilege drop. **Watchdog death kills its
ACS child but cannot restore Upstart by itself**; losing both supervisors needs
explicit recovery. A stalled watchdog expires the binding; disable returns a
bounded error rather than hanging or claiming restoration.

## Kindle-library shortcut

Tap **Enable Bluetooth Guard** in the normal Kindle library, just like the
KOReader shortcut. It uses the installed SH_Integration Scriptlet handler:
`/mnt/us/documents/Enable Bluetooth Guard.sh`, with `# Name:` metadata and
default FBInk result text. There are no install/run hooks or boot integration.
The result remains visible briefly, then the handler closes the shortcut.

Tap it after a full reboot, then open KOReader, connect headphones normally,
and Play. If no player exists yet it reports **waiting for KinAMP**; only the
accepted root-validated player can later receive protection. Repeated taps on
a ready session report **already enabled** without restarting any service,
including during playback. A fresh enable still requires existing KinAMP
playback stopped (not paused), zero native playback/prevention and exact firmware
identities. A starting/stalled session is not silently replaced.

Deleting just the library shortcut does not disable the running guard.
`bt-guard uninstall` restores stock first and removes its owned shortcut too.

## Commands (root SSH on Kindle)

```sh
/var/local/kinamp-bt-guard/bt-guard status
# After reboot, before opening KOReader; Stop first if a player already exists:
/var/local/kinamp-bt-guard/bt-guard enable
# Inspect after a few seconds: supervised, nonzero pid/start, until > monotonic_now,
# stock_job=stopped. An expired or zero binding is NOT playback protection.
/var/local/kinamp-bt-guard/bt-guard status
# Stop first; these may disconnect/restart audiomgr:
/var/local/kinamp-bt-guard/bt-guard disable
/var/local/kinamp-bt-guard/bt-guard uninstall
```

`status` never opens the FIFO and bounds its job query to eight seconds. Disable
uses O_NONBLOCK, waits at most 30 seconds for the session lock, then bounds the
stock verification to eight seconds. Success means stock is running, not merely
that a stop request was sent. Uninstall refuses to delete on incomplete recovery.

If supervisors died and stock remains stopped, stop playback and run:

```sh
/var/local/kinamp-bt-guard/bt-guard recover
/var/local/kinamp-bt-guard/bt-guard status
```

Recovery takes the exclusive lock and requires the root-only originally-running
record, zero power/native-playback baseline, known stopped job, matching stock
hashes, and **no live ACS executable (including deleted executable mappings)**.
It never kills a PID from the record. Unknown ownership/unreadable processes/
changed firmware/live ACS refuse recovery. If refused, retain files/logs and
investigate; do not issue a blind `start` or process-name kill. A normal reboot
is a user-operated fallback to stock because no boot integration exists.

## Exact supported identity and build

```text
acsbtfd        70e32455351761c90356b8640bc196201af00dbc86471f0f6e8315ca12c35528
libacsbtfdlib  569865512abd5ad74bface0a8d01123d695639955ac5dc3bf288d65094f10774
acsbtfd job    6e4336d6c9ac3cc9bf062374eed397a934115055690b03eef70c2d2810c05170
KinAMP player  e2b1466a5514963e56818a58e6ecb898140eb778b14bb4796ae76ae521fbcde6
```

The **source constants** are authoritative; hash gates reject other identities.
ACS entry/callback Thumb offsets are 0x133e9/0xdac9. Build with Debian bookworm
GCC12 arm-linux-gnueabihf (GCC14's C23 imports are absent from this target):

```sh
scripts/build-bt-guard.sh /absolute/path/to/owned-output
python3 tests/verify_bt_idle_guard.py
python3 tests/verify_bt_guard_session.py
```

The output has binaries, launcher, Scriptlet, this document and SHA256SUMS. An authorized
operator installs those six files root-owned into a new 0755 directory at the
fixed install path (binaries/launcher0755, library/docs/manifest0644), verifies
SHA256SUMS on device, privately backs up `/tmp/btfd-acs.err`, then enables with
playback stopped. Do not overwrite an active install. No proprietary firmware
or private capture belongs in this repository/package. Source is GPL-3.0 under
this fork's existing licensing; building does not authorize device installation.
Copy the owned Scriptlet into `/mnt/us/documents/Enable Bluetooth Guard.sh`;
the existing scanner indexes it like a book when the normal Kindle framework
is running. Return to the Kindle library to find it (clear book-only filters).
The examined session had the scanner service unavailable while KOReader was
open; do not start it manually or assume a full-scan LIPC request is available.
No framework restart or application-registry write is needed. Do not overwrite
an unrelated document.

## Evidence and limits

Bounded native predecessor: original ACS timeout reproduced while playing and
power Active. Guard retained at the actual 1200-second deadline; owner confirmed
audible narration for ~29 minutes. Pause delegated at the next deadline and
stock restoration succeeded. Cover-close entered Screen Saver with no prevention
write; **full suspend was not proven**.

Host checks execute real fork/exec/pipe/ownership and GLib callbacks, modeling
firmware jobs/transport. They cover restoration/death/PID boundaries, unknown
ownership, reconnect/native cancellation, identity rebinding and renewal stall.
ARM checks use private target loader/library closure and firmware timer policy
under emulation; those are not native Bluetooth acceptance. On October 14, the
packaged lifecycle passed native enable/mapping, real bluetooth binding-read and
control/write-denial checks, disable/stock restoration, re-enable and survival
across SSH disconnects. A root native proc/exe scan had zero unexpected readlink
errors (performed after the successful restoration, not a pre-stop errno gate).
Recover correctly refused an active session; watchdog-loss recovery was tested
only in the disposable host model, not by killing the owner's native watchdog.
The revised package has not yet had a new owner-audible 20-minute run.
Long sessions, reboot/power-off, destructive
player-crash tests, native Audible coexistence and energy cost remain unproven.
Keep it explicitly experimental and disable on audio/reader problems.
