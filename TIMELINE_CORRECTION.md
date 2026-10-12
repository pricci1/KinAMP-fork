# Opus presentation-timeline correction — prototype v2

This supersedes the decoder/build in the first PW6 candidate. The previous
host/QEMU artifacts and `pw6-candidate/` remain unchanged historical evidence.
**Native player acceptance is still blocked.** No candidate player has been
reported running on the Kindle. The private resource's cause is unconfirmed;
runner connectivity and non-private timing aggregates are still pending.
This orb has not contacted the runner/device, changed KoReadalong, installed,
pushed or published anything. Work remains on local `prototype/opus-mp4`.

## What was reproduced, and what was not

The native owner reports a private track duration of 78,623,088 samples but
78,624,293 frames from start (+1,205). Late seeks return duration minus target.
Its own tail/full-output agreement does not establish reference alignment.
No private audio or packet payload was transferred. These are reported native
observations, not orb reproductions or evidence of the cause.

The old public decoder appends every frame after the initial target, clipping
only against that frame's PTS and final duration. It has no cumulative output
cursor. Synthetic MP4s with **unchanged encoded payloads** demonstrate both
overlap duplication and missing gaps. The exact same public-backend regression
fails on the previous binary and passes on v2:

| Synthetic case | Before | After / required |
| --- | ---: | ---: |
| Two overlaps (480 + 725 samples), from start | 294,576 frames | 293,371 |
| Same overlap file, seek 48,000 | 246,096 | 245,371 |
| Two gaps (480 + 12,713 samples), from start | 294,576 | 307,769 |
| Same gap file, seek inside long gap at 73,013 | 227,688 | 234,756, with correctly positioned silence/audio |

Counts alone are insufficient: tests compare actual asymmetric stereo PCM
against independently spliced reference audio, including both sides of each
boundary, a seek inside silence, final single sample, EOF and beyond EOF.
They remove the beginning of overlapped later packets (first-presented wins),
not an arbitrary final tail. Gaps insert zeros, without stretching audio.
The MP4 movie timescale is 48,000 to avoid hiding sub-millisecond differences
in rounded edit-list endpoints. The generator and expectations are in
`tests/verify_opus.py`; fixtures are public synthetic media.

An additional normalized-timestamp stream switches CELT → SILK → hybrid.
FFmpeg's native Opus has an internal SILK resampler: output samples can be
delayed/flushed while frame timestamps come from the current packet. Thus
naively treating each native-decoder frame as a timestamp-aligned PCM interval
is unsafe. Upstream also has later mono SILK uninitialized-value fixes:
[normal frames](https://github.com/FFmpeg/FFmpeg/commit/3f2b452a22597e34883bd913d0f11a8087090717)
and [LBRR](https://github.com/FFmpeg/FFmpeg/commit/f3daf49b50edfb60a280a0c1c5bf1438d58c1b5b).
These facts justify avoiding this implementation for timeline alignment;
they do **not** identify the private file's failure. A standalone synthetic
continuous-mode comparison gave ~239 S16 RMS for the prior decoder versus
~0.14 for FFmpeg/libopus; raw output and commands are retained as investigation
evidence. SILK/FEC encoding is requested in the fixture; TOC confirms SILK
mode, not the presence of every possible LBRR flag.

## Smallest corrected path

- Keep restricted FFmpeg 7.1.1 for MOV demux, metadata, skip/discard and channel
  conversion, but select its **libopus decoder**, statically linked to pinned
  Xiph **Opus 1.5.2**. No second decoder adapter or new shared dependency.
- `OpusMp4::read()` tracks the next presented sample, drops already-presented
  overlap, holds a frame while emitting bounded silence for gaps, and clips
  exactly at the known presentation duration.
- Seeking backs up **32 coded packets** in the MOV sample index. Timestamp-only
  80-ms preroll can land in a gap and decode just one earlier packet; a synthetic
  seek inside the long gap demonstrated this problem. Opus packets are at least
  2.5 ms, so 32 guarantee at least 80 ms of coded history, bounded by 3.84 s for
  120-ms packets (640 ms for usual 20-ms packets). This trades some seek work
  for correct warm-up despite nonuniform timestamps. Native latency is unmeasured.
- Require an accessible MOV sample index; unusual fragmented/indexless MP4,
  encryption and arbitrary edit-list layouts are **not certified** here.
- AAC/mp4read/FAAD, miniaudio, idle/FIFO/status controls, default mixersink,
  existing LIPC keepalive and radio behavior remain unchanged. Existing AAC
  descriptor leakage and pause/resume status-position regression are preserved.

For an actually discontinuous file, a plain FFmpeg PCM concatenation clipped
by sample count is **not** a presentation-timeline oracle. Compare timestamp-
normalized intervals with the stated overlap/gap policy. On continuous fixtures
the reference is ordinary decode-from-start, trimmed to the known media end.
Do not silently reinterpret the private reference until its timing is available.

## Rebuild and verification

Use the same GCC-12/private-runtime SDK generation and public headers described
in `KINDLE_CANDIDATE.md`, with **separate output directories**:

```sh
scripts/build-kindle-candidate.sh ../target-private ../build-kindle-timeline-v2
scripts/build-opus-ffmpeg.sh ../ffmpeg-timeline-host
PKG_CONFIG_PATH="$(realpath ../ffmpeg-timeline-host)/lib/pkgconfig:$(realpath ../ffmpeg-timeline-host)/opus/lib/pkgconfig" \
  cmake -S . -B ../build-timeline-host -DGST10=ON -DKINAMP_MINIMAL_ONLY=ON \
    -DKINAMP_OPUS_MP4=ON -DKINAMP_AUDIO_SINK=fakesink -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build ../build-timeline-host --target KinAMP-minimal -j4
scripts/build-prototype-tests.sh ../build-timeline-host-tests ../ffmpeg-timeline-host ../upstream
/usr/bin/python3 tests/verify_opus.py --work ../validation-timeline-host \
  --player ../build-timeline-host/KinAMP-minimal \
  --baseline ../build-upstream-host/KinAMP-minimal \
  --pcm ../build-timeline-host-tests/opus-pcm --aac-probe ../build-timeline-host-tests/aac-probe
```

The same suite supports `--runner` for target-runtime QEMU:

```sh
target=$(realpath ../target-private)
runner="qemu-arm -L $target -E LD_LIBRARY_PATH=$target/mnt/us/KinAMP/libs_hf:$target/lib:$target/usr/lib -E GST_PLUGIN_SYSTEM_PATH=$target/usr/lib/gstreamer-0.10 -E GST_PLUGIN_PATH= -E GST_REGISTRY_FORK=no -E LD_BIND_NOW=1"
/usr/bin/python3 tests/verify_opus.py --work ../validation-timeline-qemu \
  --player ../build-kindle-timeline-v2/KinAMP-minimal.pw6-gst010 \
  --baseline ../build-kindle-target/KinAMP-minimal.upstream-control \
  --pcm ../build-kindle-timeline-v2/tests/opus-pcm \
  --aac-probe ../build-kindle-timeline-v2/tests/aac-probe --runner "$runner"
```

The baseline is the retained v1 target-linked unmodified-upstream control,
not the installed device binary. **Never run two CLI harnesses concurrently**:
KinAMP has a global `/tmp/kinamp_audio_pipe`. Offline `opus-pcm` does not use it.

Desktop validation: **88** PCM/sample/seek cases (including 2.5-ms packets and
multi-packet pre-skip), **69** overlap/gap/SILK/hybrid/transition cases,
**11** byte-identical full/seek legacy controls, **8** actual silent FIFO/EOF
captures, metadata/cover, pause/resume, stop, 12 EOF/restart cycles and cleanup.
Full timing-fixture reference RMS is at most 0.409 S16 counts; seek residuals
are recorded, not claimed bit-identical. Mono seek251 emits 438,576 frames,
RMS ~0.319 against independently decoded full audio. These are not audible
quality, native speed, battery or installation results.

The **same full matrix passes under ARM QEMU**, binding the supplied private
target loader/libraries with `LD_BIND_NOW=1` and actual GStreamer 0.10 core
plugins (silent sink only). This is emulation, **not native PW6 execution**.
Frame counts are verified for eight complete PCM FIFO captures, with byte
equality for the long-packet capture and eleven legacy controls.
Resource/status observations are retained separately from desktop;
no inference about native memory or hardware sink follows from QEMU metrics.
The unchanged AAC parser still rejects synthetic Opus with `mp4a not found`
and initializes AAC at 48 kHz. Legacy AAC before/after FD growth is equal.

| Target-built stripped executable | Bytes |
| --- | ---: |
| `KinAMP-minimal.pw6-gst010` | 971,900 |
| `opus-pcm` | 625,196 |
| `aac-probe` | 23,212 |

Player is 73,820 bytes smaller than v1. Interpreter remains
`/lib/ld-linux-armhf.so.3`, ARMv7/Thumb-2/VFPv3-D16 hard-float, no NEON tag,
no RPATH/RUNPATH. All nine direct dependencies are unchanged. Required versions
still include GLIBC_2.35 / GLIBCXX_3.4.29, actually exported by the target.
Exact symbol preflight passes: 41-object player/plugin/loader closure,
2,696 strong imports, no missing strong symbol or dependency. Helpers pass.
Existing `LipcOpenNoName`, `LipcSetIntProperty`, `LipcClose` bind to native lipc;
this does not repair or prove keepalive (native baseline still reports error 8).

## Native gate and owner-only procedure

Do not overwrite or relaunch the installed daemon. Stage this v2 player and
both helpers under a new owned `/mnt/us` test directory and record SHA256SUMS.
First repeat the public helper suite, particularly overlap0 (293,371 frames),
overlap48000 (245,371), gap0 (307,769), gap73013 (234,756), SILK/hybrid full
decode (294,576 each), transitions full (886,728), smallpacket full (54,576),
mono251 (438,576), EOF/last-sample/beyond and AAC parser/init.
Compare exported **synthetic** PCM against the retained independent reference
harness. Use `/mnt/us`, not small RAM-backed `/tmp`, for PCM outputs.

Before native player tests, re-establish runner connectivity and verify actual
process/FIFO state: last confirmed idle daemon quit does not establish the
current state after unknown read-only outcomes. Follow `KINDLE_CANDIDATE.md`'s
isolated HOME/cwd/runtime/PID procedure, fakesink first, then authorized native
mixersink/Bluetooth acceptance with headphones. No simultaneous player/mpv or
unowned global FIFO, launchers, BTenable writes, radio cycling, user books or
reader navigation. Cover late251 seek, EOS, pause/resume, stop while paused,
quit/cleanup and resource counts. Existing AAC audible baseline is separate
from an Opus claim. No native candidate player acceptance has occurred yet.

The original private-file failure remains a separate gate. Only timing/TOC/
skip aggregates are requested when access returns; no payload/audio export.
Compare v2 locally on the device only under the native owner's authorization.
Do not claim the overlap fixture establishes that private file's cause.

## Source and licensing boundary

Opus archive SHA256 (also published by Xiph):
`65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`.
Source and COPYING (BSD-3-clause and patent-license links) accompany the v2
bundle. Opus DRED/deep-PLC/OSCE, assembly and intrinsics are disabled; no model
files or runtime CPU detection are needed. FFmpeg still enables only MOV/file,
libopus decode, avformat/avcodec/avutil/swresample: no encoders, muxers, network,
filters, GPL/nonfree options or shared FFmpeg/Opus payload. Its license remains
LGPL2.1+. The combined KinAMP modifications are GPLv3+; previous miniaudio,
openlipc and public header-source notices remain. Retain complete corresponding
source/recipes/notices and any applicable installation obligations if binaries
are later conveyed; this work is not publication or deployment.

Bundle allowlist: three executables, public synthetic fixtures, exact modified
KinAMP/miniaudio source, Git bundle and patches, pinned public source archives,
licenses, build configs/link map and non-private verification/ABI summaries.
**No device libraries, installed binary, private sysroot/archive parts, books,
settings or private media** enter the bundle or redistributable checksum list.
