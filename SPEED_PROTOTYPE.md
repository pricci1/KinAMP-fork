# Pitch-preserving speed prototype — 2026-10-12

**Delivery:** opt-in `prototype/playback-speed` source branch, based on accepted
[`prototype/opus-mp4` tip 865034f](https://github.com/pricci1/KinAMP-fork/commit/865034f32a4df360e3f624fecf11874c5ae3d184)
(accepted player v2
[`b9f8282`](https://github.com/pricci1/KinAMP-fork/commit/b9f82828255f95329093772bee7410bada7cfee0)).
This is a source-only experimental prototype, not a published binary release
or a permanent device upgrade. The accepted branch remains unchanged. The
host/ARM/QEMU build records below describe the original isolated validation;
later physical testing is summarized under Native Kindle validation. Private
target exports were verified and extracted outside Git. Private sysroots,
proprietary libraries, books and settings are not distributed.

`KINAMP_SPEED=ON` opts in; OFF remains the build default. The accepted Opus
decoder, normalization, preroll, pre-skip, overlap/gap correction and clipping
are unchanged. Normalized PCM then enters pinned Sonic 0.2.0; only speed is
changed, with pitch/rate/volume held at 1. At speed 1, Sonic is bypassed exactly.
The stage supports local AAC MP4/M4A/M4B (FAAD) and Opus MP4, not radio, ADTS
streams or miniaudio formats. Existing volume is applied before stretching.

## Daemon contract for KoReadalong

Use the existing newline command FIFO and atomic key=value status file, not
a new socket. Check capability after daemon startup; old daemons lack it.

```text
capabilities=speed-v1
speed_min=0.5
speed_max=3
speed_formats=aac-mp4,opus-mp4
speed=1.3
speed_supported=1
pos_ms=5321
```

- `speed <number>\n`: finite value in inclusive **0.5–3.0**, including fractional
  values; client may use its 0.1 steps. `speed 1\n` resets. Invalid, empty,
  NaN/infinite, out-of-range, overflow or trailing-junk arguments leave speed
  and playback state unchanged, publish status and log rejection. No separate
  response/command-ID acknowledgment is added; serial clients can request
  `status\n` and inspect the atomic file.
- `speed` is **requested session speed**, not a promise that all queued PCM
  has already changed speed. It can be set while idle/stopped/playing/paused;
  it neither resumes nor restarts playback. It survives load, stop, seek,
  index/next and automatic EOF handoff; daemon restart defaults to 1.
  KoReadalong owns per-book persistence and should resend its remembered value.
- `speed_supported` describes the **last opened backend resource**, including
  when stopped, not a staged playlist entry. Unsupported formats play at 1
  without changing the stored request. On an AAC-only build `speed_formats`
  lists only `aac-mp4`. Do not offer speed based on `speed` alone.
- `seek <seconds>`, `dur` and `pos` keep original presentation-media seconds;
  additive `pos_ms` provides the same position in milliseconds. Status refresh
  remains 1 Hz while playing and command-driven while paused/stopped. Seeking
  while paused now constructs a paused pipeline and remains paused. At EOF the
  final media position is retained; a new track starts its own media origin.
- The existing integer-second seek interface is retained. Opus seek alignment
  is still sample-correct before stretching. AAC retains compressed-frame
  seeking/FAAD priming: it is not a sample-exact MP4 edit-list implementation.
  The speed-enabled AAC path fixes the necessary EOF edge: beyond-end seeking
  emits no PCM rather than silently replaying from zero. Speed-OFF behavior
  remains unchanged.

## Output/media timing and approximations

The decoder records epochs of original input frames and transformed output
frames. Speed changes occur between decoder blocks: first flush/drain the old
Sonic stream at its old rate, then begin a fresh stream. FIFO/queue audio is
not discarded or relabeled. Multiple requests before the next block coalesce
to the latest one. EOF drains Sonic before closing the writer; stop/seek
instead interrupt nonblocking open/poll/partial writes and discard stretch
buffers. Both AAC and Opus use this interruptible path when enabled.

GStreamer `identity datarate` timestamps the submitted PCM bytes. A bounded
100-ms output queue feeds a sink-side identity handoff observer. The pipeline
running clock (excluding pauses), limited by that observer's latest buffer
end and successfully submitted frames, selects the applicable output epoch.
It does **not** multiply all elapsed time by the newest speed or expose the
decoder's read cursor. EOF maps the drained endpoint; duration remains source
time. Pause/resume no longer subtracts an old running clock from source time
for this path, avoiding the accepted baseline's position regression.

Within an open epoch the map uses nominal speed; after drain it interpolates
the exact input/output endpoints, because Sonic's splice lengths are quantized
to pitch periods. Reported positions are clamped monotonically within a play
instance and to duration. This is a **clock-based media-position estimate**,
not per-sample waveform correspondence or the physical speaker playhead.
Splice-window error, AAC compressed-frame seek/priming, a sink buffer and
hardware/Bluetooth latency remain. The observer can precede actual rendering
by a buffer; a sink that ignores PCM timestamps or internally queues large
amounts invalidates physical-playhead assumptions and needs native testing.

The Linux writer requests a 4096-byte FIFO capacity; refusal on an older
kernel is tolerated. Queue bounds alone do not bound FIFO, a large decoded
packet, in-flight sink buffer, or device buffering. Rate-change latency is
therefore **not guaranteed**. A 64-KiB host FIFO was actually observed and
explained a ~0.5-s old-rate tail before the smaller-capacity request. Existing
fixed global `/tmp/kinamp_audio_pipe` is retained: runtime-directory isolation
does not isolate audio. **Serialize all player checks and leave other owners'
sessions/FIFOs alone.**

## Source and build changes

- `pcm_tempo.{h,cpp}` owns Sonic/FIFO output and the output/media epoch map;
  `music_backend` connects AAC/Opus, timestamp/clock observation and lifecycle.
- `cli_player` adds validation, discovery and status; no reader/plugin changes.
- `third_party/sonic` contains unmodified pinned C/header/COPYING, source
  hashes and regeneration instructions. CMake compiles it directly, without
  a new shared dependency. FFmpeg avfilter remains disabled.
- `scripts/build-kindle-candidate.sh` accepts `KINAMP_SPEED=ON`; its default
  stays OFF and its existing exact-symbol ABI gate is retained. It was executed
  with privately supplied target exports as described below. A target C math
  header's `isfinite` macro collides with `std::isfinite`; ordered inclusive
  bounds now reject NaN/infinity without that namespace/macro portability bug.

The existing restricted FFmpeg/libopus recipe was actually rebuilt:

```sh
git submodule update --init
scripts/build-opus-ffmpeg.sh ../ffmpeg-host
export PKG_CONFIG_PATH="$(realpath ../ffmpeg-host)/lib/pkgconfig:$(realpath ../ffmpeg-host)/opus/lib/pkgconfig"
cmake -S . -B ../build-speed -DGST10=ON -DKINAMP_MINIMAL_ONLY=ON \
  -DKINAMP_OPUS_MP4=ON -DKINAMP_SPEED=ON -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build ../build-speed --target KinAMP-minimal -j4
scripts/build-prototype-tests.sh ../build-tests ../ffmpeg-host ../KinAMP-baseline
```

`../KinAMP-baseline` is a detached worktree at the accepted fork tip, not
unmodified upstream or the installed device binary. Its submodule and player
were rebuilt with identical host settings, speed absent. Speed-OFF with Opus
and speed-ON without FFmpeg are separate build controls.

The public GStreamer 0.10 API was also exercised **on x86-64, not ARM/QEMU**:

```sh
# Hash and existing Bison patch are the ones in build-prototype-gst.sh.
curl -fL https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-0.10.36.tar.xz \
  -o ../sources/gstreamer-0.10.36.tar.xz
echo '9151aa108c177054387885763fa0e433e76780f7c5655c70a5390f2a6c6871da  ../sources/gstreamer-0.10.36.tar.xz' | sha256sum -c -
tar -xf ../sources/gstreamer-0.10.36.tar.xz -C ../sources
patch -d ../sources/gstreamer-0.10.36 -p1 < scripts/gst-0.10-bison.patch
mkdir -p ../gst010-host/build
# Run these three commands from ../gst010-host/build:
../../sources/gstreamer-0.10.36/configure --prefix="$(realpath ..)" \
  --disable-gtk-doc --disable-examples --disable-tests --disable-introspection --disable-nls
make -j4
make install
# Then from this source root:
PKG_CONFIG_PATH="$(realpath ../gst010-host)/lib/pkgconfig:$PKG_CONFIG_PATH" \
  cmake -S . -B ../build-speed-gst010 -DGST10=OFF -DKINAMP_MINIMAL_ONLY=ON \
  -DKINAMP_OPUS_MP4=ON -DKINAMP_SPEED=ON -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build ../build-speed-gst010 --target KinAMP-minimal -j4
```

0.10.25/0.10.36 upstream source confirms handoff is `(element, buffer,
user_data)` despite a misleading old `@pad` documentation comment. Timestamp,
duration, datarate, signal-handoffs and queue properties are supported there.
That source inspection is not a claim about proprietary Kindle modifications.

## Executed validation

All player runs were sequential with synthetic fixtures and isolated owned
HOME/cwd/runtime directories. Desktop package dependencies installed in this
orb: CMake, GStreamer 1.0 development/core plugins/tools, FAAD development,
numpy, nasm, bison/flex and libxml2 development. FFmpeg generator is the host
system tool; the player uses the pinned restricted static FFmpeg/libopus.

```sh
/usr/bin/python3 tests/verify_speed.py --work ../validation-speed \
  --player ../build-speed/KinAMP-minimal --baseline ../build-baseline/KinAMP-minimal
GST_PLUGIN_SYSTEM_PATH="$(realpath ../gst010-host)/lib/gstreamer-0.10" GST_PLUGIN_PATH= \
  /usr/bin/python3 tests/verify_speed.py --gst-api 0.10 --work ../validation-speed-gst010 \
  --player ../build-speed-gst010/KinAMP-minimal --baseline ../build-baseline-gst010/KinAMP-minimal
```

Both report **PASS**: 15 pitch/duration/EOF/byte-exact-1x controls; three
live/paused/seek/manual-cross-codec-handoff/stop sequences, nine invalid speeds
each; nine 2x seek/near-EOF/beyond-EOF cases and automatic Opus-to-AAC EOF
handoff. Fixed rates: 0.5, 1, 1.3, 2, 3. Inputs: 8.137-s amplitude-modulated
440/733-Hz asymmetric stereo Opus/AAC at 48 kHz and mono AAC at 44.1 kHz
(FAAD upmixes mono to stereo). Duration tolerance is 15 ms, pitch tolerance
5 Hz; the tests distinguish no-op/resampling/channel swap/missing drain and
retroactive latest-rate clocks. Final live captures use a clock-synchronized
fakesink and a tee to an unbuffered filesink, with the fakesink branch bounded
to one queued buffer. This works on all three tested GStreamer versions;
offline captures use sync=false only for processing/EOF, not timing claims.
Fixed-rate seek tests set speed before starting the decoder: a request after
seek/start intentionally cannot reclassify queued old-rate preroll.

```sh
/usr/bin/python3 tests/verify_opus.py --work ../validation-speed-baseline \
  --player ../build-speed/KinAMP-minimal --baseline ../build-baseline/KinAMP-minimal \
  --pcm ../build-tests/opus-pcm --aac-probe ../build-tests/aac-probe
GST_PLUGIN_SYSTEM_PATH="$(realpath ../gst010-host)/lib/gstreamer-0.10" GST_PLUGIN_PATH= \
  /usr/bin/python3 tests/verify_opus.py --work ../validation-speed-legacy-gst010 \
  --player ../build-speed-gst010/KinAMP-minimal --baseline ../build-baseline-gst010/KinAMP-minimal \
  --pcm ../build-tests/opus-pcm --aac-probe ../build-tests/aac-probe
```

Both report **PASS**: 88 sample/seek cases, 69 overlap/gap/SILK/hybrid/transition
cases, 11 byte-identical legacy PCM controls, eight FIFO/EOF captures plus
metadata/cover and pause/resume/stop/restart/cleanup. Known AAC FD growth
remains +4/cycle, identical to the accepted control. Radio/GTK are not executed.
Exact JSON measurements/logs are retained in the validation directories.

Additional executed controls: speed-ON without FFmpeg passes ten AAC-only
pitch/duration/1x cases and advertises only AAC; speed-OFF has no speed
capability. The same `speed 2` scenario on the accepted baseline produces
8.137 s of output (unknown command), versus 4.0685 s on the prototype.

Temporary negative mutations were actually compiled and exercised, then
restored: removing Sonic EOF flush fails at 16.21375 s versus required
16.274 s for the 0.5x Opus fixture; replacing the epoch map with the newest
speed times total elapsed output fails at position 6113 ms after the live
1x-to-3x change (correct prototype about 3300 ms). All final configurations
and helpers were rebuilt from restored source and positive suites rerun.
No negative mutation or generated Python cache is retained in source.

Host ELF inspection shows Sonic introduces no shared Sonic/libav/libopus
dependency. The speed observer's `g_signal_connect` now makes
`libgobject-2.0.so.0` a **direct** dependency on the 1.0 host build (already
transitive via GStreamer); the accepted host control does not list it directly.
This does not substitute for native exact-symbol closure validation.

## ARM candidate and supplied-runtime QEMU follow-up

All four archive parts and the inventory matched their supplied SHA256 values
before extraction. The reconstructed archive is 11,026,958 bytes, SHA256
`19cd9a39b5031bf42584c07a6adf7728d551bc33ba1199d20a0b8c76371c2f8d`;
safe extraction checked paths/types and yielded 69 regular files. Private
inputs/build trees are under `/home/user/workspace/kinamp-speed-private`, not
Git. The source archive, target libraries and generated private sysroot must
not be included in a public bundle.

Executed from this source root with Debian GCC 12 ARM cross tools and QEMU 7.2:

```sh
pkg_config=/usr/bin/pkg-config KINAMP_SPEED=ON \
  KINAMP_BUILD_CACHE=/home/user/workspace/sources \
  scripts/build-kindle-candidate.sh \
  /home/user/workspace/kinamp-speed-private/target \
  /home/user/workspace/kinamp-speed-private/build-speed-arm
```

The explicit lowercase `pkg_config` selects the installed host metadata reader
for FFmpeg configure (cross-prefixed pkg-config was absent); target-scoped
`PKG_CONFIG_LIBDIR` remains the recipe's own. Restricted FFmpeg 7.1.1/libopus
1.5.2 were rebuilt; avfilter stays disabled. The existing recipe derives GLib
configuration via a target-libc QEMU probe and stages public GStreamer 0.10.25
headers, without replacing the supplied GLib/GStreamer runtime. It does not
prove proprietary public-structure ABI correspondence.

The stripped candidate is
`/home/user/workspace/kinamp-speed-private/build-speed-arm/KinAMP-minimal.pw6-gst010`,
980,124 bytes, SHA256
`e2b1466a5514963e56818a58e6ecb898140eb778b14bb4796ae76ae521fbcde6`.
The existing exact-symbol gate reports **PASS** for player, opus-pcm and
aac-probe: ARMv7/Thumb-2/VFPv3-D16 hard-float, expected
`/lib/ld-linux-armhf.so.3`, no RPATH, no missing libraries or unresolved strong
imports. Player closure includes 41 objects and checks 2,700 unique strong
imports, including the supplied coreelements/mixersink/envload roots; this is
static preflight, not execution of those output plugins. Sonic adds no shared
library dependency; libgobject is now direct. The accepted control was also
rebuilt from the unchanged accepted worktree against the same sysroot/static
FFmpeg and independently passes the gate.

QEMU runs use the actual supplied loader, libc, GLib, GStreamer/core plugins
and FAAD, with guest `LD_LIBRARY_PATH` limited to the private target tree,
`GST_PLUGIN_SYSTEM_PATH=TARGET/usr/lib/gstreamer-0.10`, `GST_PLUGIN_PATH=`,
`GST_REGISTRY_FORK=no` and `LD_BIND_NOW=1`. There is no device session.

The first timed QEMU attempt correctly failed: the supplied 0.10.25 filesink
finished an 8.137-s fixture before 1.2 s despite `sync=true`. Upstream
[0.10.25 filesink disables its get_times callback](https://github.com/GStreamer/gstreamer/blob/RELEASE-0.10.25/plugins/elements/gstfilesink.c#L202-L206),
whereas 0.10.36 inherits BaseSink's timestamp timing. Identity datarate sets
timestamps but does not turn the incoming BYTE segment into TIME; neither
identity sync nor single-segment resolves that filesink opt-out. The final
**test-only** tee/fakesink capture above supplies real clock waits without a
production pipeline change or sleep-time approximation.

The public speed harness now accepts the same `--runner` prefix as the accepted
compatibility harness; temporary QEMU wrappers are no longer needed. Executed:

```sh
target=/home/user/workspace/kinamp-speed-private/target
out=/home/user/workspace/kinamp-speed-private/build-speed-arm
control=/home/user/workspace/kinamp-speed-private/build-accepted-arm/KinAMP-minimal.accepted-control
runner="qemu-arm -L $target -E LD_LIBRARY_PATH=$target/mnt/us/KinAMP/libs_hf:$target/lib:$target/usr/lib -E GST_PLUGIN_SYSTEM_PATH=$target/usr/lib/gstreamer-0.10 -E GST_PLUGIN_PATH= -E GST_REGISTRY_FORK=no -E LD_BIND_NOW=1"
/usr/bin/python3 tests/verify_speed.py --gst-api 0.10 \
  --work /home/user/workspace/kinamp-speed-private/validation-speed-qemu-final \
  --player "$out/KinAMP-minimal.pw6-gst010" --baseline "$control" --runner "$runner"
/usr/bin/python3 tests/verify_opus.py \
  --work /home/user/workspace/kinamp-speed-private/validation-legacy-qemu-final \
  --player "$out/KinAMP-minimal.pw6-gst010" --baseline "$control" \
  --pcm "$out/tests/opus-pcm" --aac-probe "$out/tests/aac-probe" --runner "$runner"
```

The speed suite passes all 15 pitch/duration/EOF/byte-exact-1x cases, three
live/paused/seek/manual handoff/stop sequences, 27 invalid-speed checks, nine source-time seek/end
cases and automatic Opus-to-AAC handoff. At 2x Opus output is 4.0685 s but
final source position is 8137 ms; channel peaks remain about 440/732 Hz, not
880/1466 Hz. Live 1x-to-3x snapshots are about 3.2 s, not a retroactive 6 s;
paused changes leave pos_ms and captured size frozen; paused seek remains
paused. The complete accepted compatibility suite also passes under the same
QEMU runner: 88 sample/seek cases, 69 original timing-correction cases,
11 byte-identical legacy controls, eight FIFO/EOF captures and metadata/
pause/resume/stop/restart/cleanup. Known AAC FD growth remains +4/cycle,
identical to the accepted ARM control.

Additional host/target-QEMU real-daemon probes interrupt actively playing
0.5x AAC and Opus with seek and stop (observed 20–41 ms command-to-status,
not a device latency promise). Clocked 2x Opus writes 195,288 stereo frames
and ends at source 8137 ms; command-to-EOS wall time including startup was
4.075 s on host and 4.261 s under QEMU, against 4.0685 s of PCM with 250-ms
startup/scheduling tolerance. This confirms pacing, not physical rendering.
Final host/ARM players and offline helpers were rebuilt from non-mutated source;
clean player rebuilds reproduced the exact hashes. Final speed suites on host
1.0, host 0.10.36 and supplied ARM 0.10.25 were rerun, as were compatibility
and AAC-only/speed-OFF controls. Exact results, executed commands, final hashes
and ABI reports are in the review bundle's executed-results summary.

The rebuilt host integration binary remains
`/home/user/workspace/build-speed/KinAMP-minimal`, 2,436,840 bytes, SHA256
`493307086cd550c012e3414ab3c0b3808e229e36818111dbdf1e932d141729ef`.
Synthetic integration fixtures remain
`/home/user/workspace/validation-speed/fixtures/opus-48000-2.mp4` and
`/home/user/workspace/validation-speed/fixtures/aac-48000-2.mp4`.
The KoReadalong coordinator independently reports its real adapter tests
passed against this exact rebuilt host binary; that is coordinator-side
evidence, not physical Kindle acceptance.

## Native Kindle validation and remaining gates

On 2026-10-13, owner-authorized temporary testing ran the exact ARM candidate
`e2b1466a5514963e56818a58e6ecb898140eb778b14bb4796ae76ae521fbcde6`
on a Paperwhite 6, firmware 5.19.3.0.1, KOReader v2026.07.1, with native
GStreamer 0.10 mixersink/mixerAPI mappings and connected Bluetooth output.
Public synthetic speech exercised AAC and Opus, 0.5–3x limits, live changes,
paused speed changes/seeks, stop/rate retention, chapter handoff across a
non-narrated spine item, EOF and silent reopening with saved speed applied to
a fresh daemon. The owner listened at 1x and 1.5x and reported both sounded
right; this is subjective feedback, not a whole-book audio-quality guarantee.

| Measurement | Accepted player 1x | Speed candidate 1x | Speed candidate 2x |
| --- | ---: | ---: | ---: |
| AAC observed EOF interval | 28.07 s | 28.07 s | 14.03 s |
| Opus observed EOF interval | 30.06 s | 30.08 s | 15.03 s |
| AAC CPU, median % of one core | 8.80% | 10.96% | 27.45% |
| Opus CPU, median % of one core | 7.49% | 9.48% | 24.92% |

CPU medians use three comparable runs: process user+system tick deltas between
the second and eighth repeated one-second samples, excluding startup/EOF.
EOF intervals are status observations, not audible-output timing. The accepted
player ignored `speed 2` and retained its 1x intervals. Both players showed the
inherited AAC descriptor growth; no bounded long-session claim is made.

KoReadalong native UI testing also fixed paused-clock reconciliation/repaint
and early Exit cleanup in that client. Focused single-Exit trials while paused
and playing both closed KOReader with exit code 0 and released session files
without a daemon-disappeared prompt. The accepted player was never overwritten;
original plugin/profile/book were restored, playback stopped and exact owned
device test trees removed. Private backups remain on the runner. Evidence is
recorded in the [native test thread](https://ampcode.com/threads/T-01a134f8-b4fe-732b-a045-a9e777f47590).

These bounded tests exercise physical mixersink execution, not every proprietary
ABI structure, battery life, long-session reliability, other devices or precise
Bluetooth latency. Tone spectra demonstrate pitch preservation numerically;
Sonic splice transients and whole-book speech quality remain broader gates.
Future device writes/testing require owner authorization. Do not touch unowned
daemons, FIFOs or sessions.

## Licensing boundary

KinAMP changes remain GPLv3+; Sonic's Apache-2.0 copyright/license and exact
sources are retained. Existing pinned FFmpeg 7.1.1 LGPL2.1+, libopus 1.5.2
BSD/patent notices, miniaudio, FAAD and GStreamer source/license obligations
remain unchanged. Preserve complete corresponding source, build recipes and
applicable installation information if any binary is later conveyed. This
source prototype is not a binary release.
