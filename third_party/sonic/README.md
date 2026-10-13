# Pinned Sonic PCM library

Unmodified `sonic.c`, `sonic.h`, and `COPYING` from Bill Cox's Sonic,
[`release-0.2.0`](https://github.com/waywardgeek/sonic/tree/60eeb064f62a106069d2ce148fabd724f9df9780),
commit `60eeb064f62a106069d2ce148fabd724f9df9780`.
The upstream release contains no NOTICE file. Copyright headers and Apache-2.0
license are retained; Apache-2.0 is compatible with the GPLv3+ combined player.

Source regeneration (in a separate scratch directory):

```sh
git clone https://github.com/waywardgeek/sonic.git sonic-source
git -C sonic-source checkout --detach 60eeb064f62a106069d2ce148fabd724f9df9780
cp sonic-source/sonic.c sonic-source/sonic.h sonic-source/COPYING third_party/sonic/
(cd third_party/sonic && sha256sum -c SHA256SUMS)
```

`cmake -DKINAMP_SPEED=ON` compiles this C source directly into KinAMP using
the chosen toolchain, with no Sonic runtime library or FFmpeg avfilter.
Pitch/rate/volume stay 1; only Sonic speed changes. At 1x the PCM stage bypasses
Sonic byte-for-byte. See `SPEED_PROTOTYPE.md` for recipes, protocol, timing
approximations and validation boundaries. Convey these exact sources/license
and the complete KinAMP build recipe with any future binary; retain the existing
FFmpeg/Opus/miniaudio/FAAD/GStreamer notices and source obligations as well.
