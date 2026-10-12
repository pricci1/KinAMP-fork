# Isolated Opus-in-MP4 prototype — 2026-10-12

This is review/build material, **not a Kindle installation package**. Nothing
was pushed, published, deployed, or installed on a Kindle/runner. KoReadalong
code and books were not changed. All media in this experiment is synthetic.

**Target-linked continuation:** [KINDLE_CANDIDATE.md](KINDLE_CANDIDATE.md) records
the supplied Paperwhite 6 library inventory, corrected GLIBC/GLIBCXX premise,
new target-linked build/preflight, and owner-only native acceptance procedure.
The Debian/QEMU measurements below remain historical evidence, not deployment
artifacts or a claim that this device lacks GLIBC_2.35 / GLIBCXX_3.4.29.

## Provenance and design

- KinAMP source: [main 91b661c](https://github.com/kbarni/KinAMP/commit/91b661cd9ffdd022e9af698b071256426c81351b).
- miniaudio submodule: [f40cf03](https://github.com/mackron/miniaudio/commit/f40cf03f80cdb7e741d43e53b7e706e8c1394bcf).
- The [3.0.0 tag](https://github.com/kbarni/KinAMP/commit/3b5fb6d497f78bf7e2c84b64b2e522e66f8c4f88)
  declares CMake version **2.9**. Main declares 3.0.0. Neither label proves
  provenance of the installed KinAMP binary; these controls are newly compiled
  upstream source, not the user's installed binary.
- FFmpeg **7.1.1**, SHA-256
  `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`.
- GStreamer **0.10.36** source archive, SHA-256
  `9151aa108c177054387885763fa0e433e76780f7c5655c70a5390f2a6c6871da`.
  The emulation build needs one explicit modern-Bison lexer-parameter patch.

`KINAMP_OPUS_MP4=ON` is opt-in; normal builds remain FFmpeg-free. Restricted
static FFmpeg enables only the MOV/MP4 demuxer, **native Opus decoder**, file
protocol and sample conversion libraries. No programs, encoders, muxers,
network protocols, filters, libopus runtime, optional GPL or nonfree components.
The recorded configure output says **LGPL version 2.1 or later**.

The new path probes container bytes and codec ID, even for a misnamed `.bin`.
AAC still uses mp4read/FAAD, other local formats still use miniaudio, and HTTP
stream handling is untouched. FFmpeg owns `dOps`, pre-skip, gain, channel layout
and packet discard handling. Seeking decodes at least 80 ms of preroll, then
discards PCM up to the requested presentation sample. The presentation duration
also clips the last MP4 packet, which can exceed the audible end without discard
padding side data. Output is 48-kHz signed 16-bit stereo PCM through the **same
`/tmp/kinamp_audio_pipe` and GStreamer pipeline**, with the default `mixersink`.
Only tests override the sink with silent filesink/identity elements.

The FIFO writer handles partial writes and uses interruptible nonblocking
open/write/poll. Stop joins completed decoder threads as well as running ones;
a naturally completed Opus thread must not retain its stack across restarts.
Tags, cover art and chapter timestamps use the Opus MP4 demux metadata path.
The command FIFO, status schema and CLI commands are unchanged.

## Rebuild on Debian 12: desktop control

Use an isolated checkout and **do not run this alongside another KinAMP**: the
upstream audio FIFO has one fixed global pathname, even with an isolated HOME.
From this modified checkout:

```sh
sudo apt-get update
sudo apt-get install cmake pkg-config g++ curl xz-utils nasm \
    libgstreamer1.0-dev gstreamer1.0-plugins-base libfaad-dev ffmpeg python3-numpy
git submodule update --init
git worktree add --detach ../upstream 91b661cd9ffdd022e9af698b071256426c81351b
git -C ../upstream submodule update --init
scripts/build-opus-ffmpeg.sh ../ffmpeg-host
PKG_CONFIG_PATH="$(realpath ../ffmpeg-host)/lib/pkgconfig" \
  cmake -S . -B ../build-host -DGST10=ON -DKINAMP_MINIMAL_ONLY=ON \
    -DKINAMP_OPUS_MP4=ON -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build ../build-host --target KinAMP-minimal -j4
cmake -S ../upstream -B ../build-upstream-host -DKINAMP_KOBO=ON \
    -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build ../build-upstream-host --target KinAMP-minimal -j4
scripts/build-prototype-tests.sh ../build-host ../ffmpeg-host ../upstream
/usr/bin/python3 tests/verify_opus.py --work ../validation-host \
    --player ../build-host/KinAMP-minimal --baseline ../build-upstream-host/KinAMP-minimal \
    --pcm ../build-host/opus-pcm --aac-probe ../build-host/aac-probe
```

The source archive includes miniaudio's complete source; a Git checkout uses
the pinned submodule. Keep dependency archives in `../sources` for offline
rebuilds (`KINAMP_BUILD_CACHE` can select another existing cache). These builds
are recipe-reproducible, not claimed bit-for-bit reproducible across compilers,
paths and distro updates. Exact package versions are recorded in evidence.

## ARM hard-float / GStreamer 0.10 emulation

This targets ARMv7 VFPv3-D16 hard-float, **not NEON**, and GStreamer 0.10 API.
It uses Debian headers/libs, not the Kindle's sysroot. `GST10=OFF` means 0.10;
despite the name, `GST10=ON` selects GStreamer 1.0.

```sh
sudo dpkg --add-architecture armhf
sudo apt-get update
sudo apt-get install gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf qemu-user \
    libglib2.0-dev:armhf libxml2-dev:armhf libfaad-dev:armhf bison flex
scripts/build-opus-ffmpeg.sh ../ffmpeg-arm arm-linux-gnueabihf-
scripts/build-prototype-gst.sh ../gst-arm
export KINAMP_FFMPEG_PREFIX="$(realpath ../ffmpeg-arm)"
export KINAMP_GST_PREFIX="$(realpath ../gst-arm)"
cmake -S . -B ../build-arm -DCMAKE_TOOLCHAIN_FILE=scripts/armhf-prototype.cmake \
    -DGST10=OFF -DKINAMP_MINIMAL_ONLY=ON -DKINAMP_OPUS_MP4=ON \
    -DKINAMP_CHECK_ABI=OFF -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build ../build-arm --target KinAMP-minimal -j4
scripts/build-upstream-arm-control.sh ../upstream ../gst-arm ../build-upstream-arm
scripts/build-prototype-tests.sh ../build-arm ../ffmpeg-arm ../upstream arm-linux-gnueabihf-
/usr/bin/python3 tests/verify_opus.py --work ../validation-arm \
    --player ../build-arm/KinAMP-minimal --baseline ../build-upstream-arm/KinAMP-minimal \
    --pcm ../build-arm/opus-pcm --aac-probe ../build-arm/aac-probe \
    --runner "qemu-arm -L / -E LD_LIBRARY_PATH=/usr/lib/arm-linux-gnueabihf:$KINAMP_GST_PREFIX/lib -E GST_PLUGIN_SYSTEM_PATH=$KINAMP_GST_PREFIX/lib/gstreamer-0.10 -E GST_PLUGIN_PATH= -E GST_REGISTRY_FORK=no"
```

Use the matching multiarch ARM loader (`qemu-arm -L /`), not the cross compiler's
older `/usr/arm-linux-gnueabihf` loader with newer multiarch libc. That mismatch
crashed even an unmodified GStreamer fakesrc/fakesink thread-start test; the
matching loader ran it to EOS. Registry forking is disabled **only for QEMU**
so it doesn't try to execute the ARM scanner outside the emulator.

The optional feature also builds disabled, without FFmpeg discovery. Upstream's
`KINAMP_CHECK_ABI` references a missing `cmake/check_glibcxx.sh` at this revision;
the emulation recipe disables it explicitly and retains readelf evidence,
**not a claim of passing a device ABI check**.

## Measured binary sizes and requirements

All sizes below are stripped executables; controls use the same compiler family
and size-optimization settings. Full build products are retained in the orb.

| Executable | Upstream bytes | Prototype bytes | Increase |
| --- | ---: | ---: | ---: |
| ARM hard-float, GST 0.10 | 356,700 | 1,045,712 | 689,012 |
| x86-64, GST 1.0 | 675,704 | 1,900,984 | 1,225,280 |

ARM ELF direct dependencies: `libfaad.so.2`, `libm.so.6`,
`libgstreamer-0.10.so.0`, `libglib-2.0.so.0`, `libstdc++.so.6`,
`libgcc_s.so.1`, `libc.so.6`; interpreter `/lib/ld-linux-armhf.so.3`.
No libav* or libopus shared-library requirement is added. GStreamer additionally
needs its own transitive libraries and coreelements plugin (filesrc, queue,
identity and test sinks); native Kindle output needs Lab126's mixersink.
The emulation binary has an orb-local GStreamer RUNPATH, which must be replaced
or overridden when moving it to another **emulation** environment.

The prototype requests **GLIBC_2.35 and GLIBCXX_3.4.29**; the rebuilt upstream
control requests GLIBC_2.34 / GLIBCXX_3.4.29. These are **not certified Kindle
requirements**. In particular, 3.4.29 exceeds upstream's default 3.4.28 ceiling.
No lipc is available here, so neither emulation build includes the optional
Bluetooth keepalive. No mixersink binary is emulated or redistributed.

## Verification contract and remaining gates

`verify_opus.py` tests 72 decode/seek cases: non-periodic mono 48-kHz MP4
through 251 s and 259.940 s; asymmetric stereo; 5.1 downmix; 312- and 120-sample
pre-skip; positive dOps gain; 60-ms packets; misnamed content; single-sample,
exact-end and beyond-end boundaries. It compares against the installed desktop
FFmpeg decoder and independently known presentation sample counts. The long
fixture has **12,486,576** stereo frames; seek 251 emits **438,576** frames.
Decode-from-start comparison is effectively identical within integer rounding.
80-ms seek preroll is **not a promise of bit-identical predictor state** to
decode-from-start: numerical residuals, including single-sample end cases, are
reported explicitly. This is not an audible-quality assertion.

The unchanged mp4read/FAAD parser rejects Opus with `atom 'mp4a' not found` and
accepts mono AAC at 48 kHz. Eleven full/seek controls compare **byte-identical
FIFO PCM** before/after for AAC mono/stereo, MP3, FLAC, WAV and Vorbis. Six actual
CLI FIFO/EOF captures cover Opus, late/end seek, extension independence,
metadata/cover and packets larger than PIPE_BUF. Interaction tests observe PCM
freezing while paused, resuming on play, bounded stop while paused, repeated
restart/EOF, flat descriptor counts for Opus, and runtime cleanup on quit.
Desktop virtual memory is flat; QEMU's **host-process** VmSize increases about
3.7 MB over 12 cycles, below the missing-thread-join signature tested by the
harness. That is not a claim of native ARM heap stability. Evidence labels
desktop and QEMU separately. An additional public MusicBackend metadata probe
verified 6,137,000,000 ns duration, two chapters (the second at 12,500,000 in
100-ns units), and exact 68-byte cover extraction on both architectures.

Measured mono seek-251 residual versus decode-from-start is about 10.86 signed
16-bit counts RMS overall and 103.71 in its first 100 ms; a one-sample shift is
about 950 counts RMS. This supports sample alignment, not an audible comparison.

Two existing upstream defects are **preserved and reported, not hidden**:

1. Pause/resume can regress the status `pos`, although PCM pause/resume works.
2. Repeated AAC load/restart leaks four file descriptors per tested cycle;
   `mp4read_close()` never closes `g_fin`. Before/after tests record the same
   growth. The new Opus path must independently show no resource growth.

Thus the prototype is not a claim that every upstream lifecycle invariant is
already healthy. Fixing these unrelated defects needs a separate scoped decision.
Internet-radio transport and the GTK frontend were not executed; they were not
redesigned. Encrypted MP4 and arbitrary edit-list/channel-map variants are not
certified by these fixtures.

**Native acceptance is still gated** on a compatible kox Kindle toolchain and
target sysroot (matching libc, libstdc++, GLib, GStreamer 0.10, FAAD and optional
lipc), rebuilding this restricted FFmpeg with that compiler/sysroot, checking
all ELF symbol versions against the actual device libraries, and then an
explicitly authorized native test on Paperwhite 6 / firmware 5.19.3.0.1 with
mixersink/Bluetooth/KOReader v2026.07.1. `build-opus-ffmpeg.sh` accepts a compiler
cross prefix and optional sysroot for that future rebuild. The upstream
`armhf-toolchain.cmake` describes target-only header/pkg-config lookup; do not
reuse the Debian prototype toolchain as a Kindle certification.
No native performance, battery use, audible quality, installation acceptance,
or the user's private media has been tested in this orb.

## Licensing and retained source

KinAMP/mp4read and these modifications are **GPLv3 or later**. Modified files
are dated; the original LICENSE/LICENSE.md and source notices remain. Static
FFmpeg is LGPL 2.1+ (with its individual BSD/MIT notices), compatible with the
GPLv3+ combined program. No `--enable-gpl` or `--enable-nonfree` is used. No
external libopus is linked; the desktop test generator's libopus is a system
tool dependency, not part of the KinAMP executable.

Retain and offer corresponding source/build scripts beside any conveyed
binary: modified KinAMP, pinned miniaudio including stb notices, FFmpeg's exact
source archive plus configure recipe/output, and notices. The review bundle
also retains the GStreamer source archive/Bison patch and Debian FAAD2 2.10.1
source/packaging archives. GStreamer carries LGPL notices; FAAD2 is GPLv2+ and
can combine under GPLv3. Distro libc/GLib/compiler dependencies are not bundled;
their exact package versions are recorded. Redistribution would require the
appropriate source/notices for any additional bundled dependencies, not just
this patch. Preserve the rebuild and, where applicable, GPL installation
information obligations; this prototype grants no permission to omit them.
No package release or publication was performed.
