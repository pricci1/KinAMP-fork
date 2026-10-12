# Paperwhite 6 target-linked candidate

This continues the isolated Opus prototype, not KoReadalong or deployment.
No runner/device was contacted and nothing was installed, pushed or published.
Native ownership remains with the source thread/user. The original host and
Debian/QEMU artifacts and evidence are preserved unchanged.

**v2 supersedes this candidate:** [TIMELINE_CORRECTION.md](TIMELINE_CORRECTION.md)
records the generic overlap/gap correction, static libopus build, new sizes and
expanded synthetic verification. The v1 measurements below are historical.
Native player acceptance remains blocked pending owner-side tests and timing
investigation; no private-file cause has been established.

## Corrected target evidence and toolchain choice

The private library inventory supplied for Paperwhite 6 / firmware 5.19.3.0.1
exports **GLIBC_2.35 and GLIBCXX_3.4.29**. The earlier report did not establish
that those versions were absent on this device. They are not rejection criteria
here: individual imports, dependency closure and actual loader execution matter.
The privately uploaded archive was checked against its supplied SHA-256 before
extraction. Its libraries must NOT be included in source/candidate packages,
redistributable notices, or release checksum manifests.

I inspected koxtoolchain 2025.05, pinned builder
[fdba8ca](https://github.com/koreader/koxtoolchain/commit/fdba8ca6e52980359dbe25f2c3ed5bae71c92044).
Its kindlehf toolchain is GCC 14.2.0 / glibc 2.20, not a multimedia SDK.
A real GCC-14 iostream negative control imports
`_ZSt21ios_base_library_initv@GLIBCXX_3.4.32`, absent from this target. Its
preflight correctly fails. Using that compiler would add an unnecessary static
C++ runtime or require other compatibility work.

The simpler **equivalent target-linked toolchain** is GCC 12.2 cross-compiling
against a private sysroot containing the actual device runtimes. Debian ARM
libc development headers/startup objects (2.36) and GCC-12 C++ headers are used;
this is not claimed to be Amazon's exact SDK. Every resulting required symbol
is checked against the actual 2.35/3.4.29 exports. There is no Debian shared
library payload and the Debian prototype executable is NOT reused.

GLib public headers are generated from upstream **2.29.18**, matching version
objects read from the supplied library; GStreamer headers/configuration are
generated from **0.10.25**, matching its version string. GLib pthread sizes and
initializer bytes are measured under QEMU with supplied libc, not hard-coded
struct offsets. GST uses `--disable-loadsave`: native GST has binary registry
symbols and no XML symbols/libxml dependency. No GLib/GStreamer library is
rebuilt or shipped. FAAD uses its public 2.10.1 header; the actual supplied
FAAD library is used at link/test time. KinAMP's existing MIT openlipc header
and existing keepalive implementation are used without new ABI declarations.

## Rebuild

On Debian 12, install **orb/host build tools only**, never on the device:

```sh
sudo apt-get install gcc-12-arm-linux-gnueabihf g++-12-arm-linux-gnueabihf \
    libc6-dev-armhf-cross cmake pkg-config curl make xz-utils bzip2 gettext \
    libglib2.0-dev qemu-user nasm
# Unpack the private device tree separately, outside this source checkout.
# ROOT must have lib/, usr/lib/ and mnt/us/KinAMP/libs_hf/libfaad.so.2.
scripts/build-kindle-candidate.sh ../target-private ../build-kindle-target
```

All fetched header/FFmpeg archives are SHA-256 pinned in the scripts, retained
in the source cache and review artifacts. CMake >=3.24 is needed for `--fresh`.
Compiler/package versions and the link map are retained. Builds are reproducible
by recipe, not asserted bit-for-bit across changed paths/tool/package versions.
The script builds restricted **static FFmpeg 7.1.1**, the minimal player, and
the existing `opus-pcm` / `aac-probe` test executables, then gates success on
`check-target-abi.py`. Source AAC parser files are unchanged from pinned upstream.
`-latomic` is satisfied by the compiler's static archive, not a new target
SONAME; the recorded player link map pulls no libatomic members.

No RPATH/RUNPATH is emitted. Interpreter is `/lib/ld-linux-armhf.so.3`.
ARM attributes: ARMv7-A, Thumb-2, VFPv3-D16, VFP argument ABI, little-endian
ELF32; no NEON tag is required by the executable. Existing vendor libraries
can themselves require NEON, as the installed player already does.

| Stripped executable | Bytes |
| --- | ---: |
| `KinAMP-minimal.pw6-gst010` | 1,045,720 |
| `opus-pcm` | 699,012 |
| `aac-probe` | 23,212 |

Player direct dependencies: FAAD2, **liblipc.so.1.0**, libm, GStreamer 0.10,
GObject, GLib, libstdc++, libgcc_s, libc. Default output remains **mixersink**.
No new libav*, libopus or libatomic shared dependency. Existing
`/mnt/us/KinAMP/libs_hf` is needed to find FAAD; other libraries stay native.

## Preflight and emulated validation

The player plus loader, envload, native coreelements/mixersink and their full
closure comprises **41 ELF objects**. **2,698 distinct strong imports** resolve,
including versioned imports; no missing dependencies or strong symbols.
The helper closures also pass. Optional unresolved weak hooks are reported,
not falsely treated as required. All objects have ARM little-endian hard-float
ELF headers. Highest player GLIBC / GLIBCXX requirements are 2.35 / 3.4.29.
The three actual LIPC imports are `LipcOpenNoName`, `LipcSetIntProperty`,
`LipcClose`; all exist in the supplied native library.

Under **QEMU with the actual target loader/libraries and LD_BIND_NOW=1**:

- 72 sample/seek cases pass using the existing offline PCM harness.
- Eleven legacy-format full/seek controls are byte-identical before/after,
  using an independently built unmodified upstream player with the same target
  runtime. This control is not equated to the installed player's provenance.
- Five target-GST FIFO captures pass: seek 251, near EOF, long packets,
  extension independence, tags/cover. Late seek emits exactly 438,576 frames.
- PCM pause/resume, stop while paused (~0.021 s), twelve EOF/restart cycles,
  quit and global/command/status FIFO cleanup pass. Stopped Opus FD count is
  flat at 11. QEMU host VmSize grows ~3 MB; no native memory claim is made.
- Target FAAD probe initializes AAC at 48 kHz; unchanged parser rejects Opus.
- Loader diagnostics confirm target libstdc++/libc/libgcc/libm and successful
  version checks. The GCC-14 missing-symbol negative control fails as intended.

The original status-position regression persists (252 -> 251 on resume in
this run). Legacy AAC leakage and failed-Opus shutdown are not repaired.
Source-thread **reported native baseline**, not an orb observation: installed
player rejects synthetic Opus and then hangs on stop/quit until owned SIGTERM;
native AAC through mixersink reaches EOS/clean exit, including a 12.137-s
stereo fixture that the user explicitly confirmed hearing. Reader pixels and
sidecar are reported unchanged. Existing keepalive returns LIPC error 8.
Symbol/link success does NOT establish that keepalive will work.

## Scoped native acceptance — performed only by the native owner

Do not run the repository launchers, overwrite installed KinAMP, modify
library paths globally, cycle Bluetooth or use a private book. Stage only the
three candidate executables and synthetic fixtures in a disposable directory
on `/mnt/us`; use `/tmp` for the private runtime/config working directory.
Record candidate checksums and device firmware/runtime before testing.

**Before every player test**, confirm no KinAMP/mpv process is active and
`/tmp/kinamp_audio_pipe` is absent. If either exists, stop and ask the native
owner; never remove an unowned FIFO or terminate a pre-existing process.
`KINAMP_RUNTIME_DIR` isolates command/status paths, NOT this global audio FIFO.
Keep reader open but do not navigate; compare its prior pixel/sidecar hashes.

First run helpers with only the existing FAAD dependency path:

```sh
export LD_LIBRARY_PATH=/mnt/us/KinAMP/libs_hf
# CANDIDATE/FIXTURES/OUT are absolute paths to disposable staged files.
"$CANDIDATE/aac-probe" "$FIXTURES/aac-mono.mp4"
"$CANDIDATE/opus-pcm" "$FIXTURES/mono.mp4" 0 "$OUT/mono-start.pcm"
"$CANDIDATE/opus-pcm" "$FIXTURES/mono.mp4" 12048000 "$OUT/mono-251.pcm"
"$CANDIDATE/opus-pcm" "$FIXTURES/mono.mp4" 12486576 "$OUT/mono-eof.pcm"
"$CANDIDATE/opus-pcm" "$FIXTURES/stereo.mp4" 0 "$OUT/stereo.pcm"
"$CANDIDATE/opus-pcm" "$FIXTURES/lowdelay.mp4" 121 "$OUT/lowdelay-121.pcm"
```

Target positions are **48-kHz samples**, not seconds. Expected mono frame
counts are 12,486,576 / 438,576 / 0; output is four bytes/frame (S16 stereo).
Stereo count is 294,576; lowdelay seek-121 count is 54,455. Store regular PCM
files on `/mnt/us`, not potentially small RAM-backed `/tmp`. Export only these
synthetic outputs to compare independently against desktop FFmpeg and the
retained harness (including asymmetric channels and pre-skip boundaries).
Require counts and alignment; do not demand bit-identical predictor state
after 80-ms preroll. Numerical residual limits remain in `verify_opus.py`.
The AAC probe checks parser/init, not complete AAC sample decoding.

For the player, create a fresh `/tmp/kinamp-opus-accept-XXXXXX`, set HOME and
KINAMP_RUNTIME_DIR to it, **cd there**, and launch the candidate directly with
`--idle`, capturing its exact PID/stdout/stderr. Idle startup must be stopped
and produce no audio. Start with test-only `KINAMP_SINK='fakesink sync=true'`;
repeat the important audible cases with KINAMP_SINK **unset** for mixersink.
Keep connected headphones and seek the long fixture immediately, rather than
play 260 seconds from the beginning. Never write BTenable; log error 8 without
trying to repair the radio/keepalive state.

Write a one-line absolute synthetic playlist. Commands, each newline-ended,
go to `$KINAMP_RUNTIME_DIR/kinamp_cmd`:

1. `load /absolute/synthetic.m3u`: count 1, index -1, no playback.
2. `index 0` followed immediately by `seek 251` for mono; verify duration 260,
   near-251 position and Opus decoder init. Check actual audio/PCM, not pos alone.
3. Pause after ~1 s, hold ~1 s, resume: audio/PCM must freeze/resume. Record
   the known pos regression separately. Repeat pause then stop; stop must return
   within 3 s. Restart and let the final ~9.137 s reach EOS.
4. Repeat stop/load/end-seek cycles and record own PID fd count and RSS before/
   after warm-up; account for one-time library/LIPC initialization. Check no
   accumulating descriptors for Opus. Check stereo and lowdelay fixtures.
5. Replay synthetic AAC stereo (12.137 s) and mono (1.137 s), then MP3/FLAC/
   Vorbis/WAV if those retained synthetic controls are available. A stopped
   snapshot at 3 s for the 1.137-s AAC is expected EOS, not a failure.
6. `quit`: within 5 s, exit 0 and owned command/status/global FIFO removed.
   On timeout, inspect the confirmed owned PID and use SIGTERM only for that
   process; retain logs/exit/cleanup evidence. Do not start another test until
   the owned audio FIFO is cleaned and no player remains.

Native acceptance remains pending until these helper and hardware checks are
reported. No audible Opus, performance, battery, native memory or installation
acceptance is asserted by the orb/QEMU results.

## Source, licenses and packaging boundary

KinAMP/modifications: GPLv3+; static restricted FFmpeg: LGPL2.1+ with retained
BSD/MIT notices; miniaudio/STB and openlipc notices stay with source. The same
restricted configure enables no encoders, muxers, network, GPL/nonfree options
or external libopus. Public GLib/GST/FAAD/zlib header-source archives and
compiler/runtime copyright notices are retained. Compiler runtime exception
and system-library rules apply to ordinary GCC/libc development components;
this is not permission to redistribute vendor libraries. Source/build recipes
and notices accompany the candidate; any later publication must separately
honor GPL corresponding-source/installation obligations.

Package allowlist: our three executables, modified source/miniaudio, patch/Git
bundle, public source archives, notices, build configuration, link map and
non-private test/ELF summaries. No private sysroot, proprietary `.so`, installed
player, uploaded archive parts, books, settings or private data is packaged.
