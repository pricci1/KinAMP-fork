#!/usr/bin/env python3
"""Public daemon/FIFO regression contract; synthetic media only, serial players.
GPL-3.0-or-later, 2026-10-12. Requires the existing desktop FFmpeg/numpy tools.
"""
import argparse
import json
import shlex
import subprocess
import time
from pathlib import Path
import numpy as np
from verify_opus import Player, capture


def refreshed(p):
    return p.snapshot()


def peak(pcm, rate, channel):
    x = pcm[int(.3 * rate):int(.9 * rate), channel].astype(float)
    spectrum = abs(np.fft.rfft(x * np.hanning(len(x))))
    return float(np.argmax(spectrum) * rate / len(x))


def generate(root):
    root.mkdir(parents=True, exist_ok=True)
    media = []
    for codec, rate, channels in (("opus", 48000, 2), ("aac", 48000, 2),
                                  ("aac", 44100, 1)):
        # Different pitches/channels, non-integral duration and low-frequency
        # amplitude modulation distinguish resampling, no-op, missing tail,
        # channel swap and rate quantization from pitch-preserving stretching.
        t = np.arange(int(8.137 * rate)) / rate
        x = np.column_stack([np.sin(2 * np.pi * f * t) * (7000 + 2000 * np.sin(2*np.pi*2.3*t))
                             for f in (440, 733)][:channels]).astype("<i2")
        raw = root / f"{codec}-{rate}-{channels}.pcm"
        raw.write_bytes(x.tobytes())
        out = raw.with_suffix(".mp4")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ar", str(rate),
                        "-ac", str(channels), "-i", str(raw), "-c:a",
                        "libopus" if codec == "opus" else "aac", "-b:a", "96k", str(out)], check=True)
        # This FAAD configuration upmixes mono to two identical channels;
        # the accepted Opus backend also always emits stereo.
        media.append((out, rate, 2))
    return media


def fixed(player, baseline, root, media, runner=()):
    report = []
    for path, rate, channels in media:
        control = capture([*runner, baseline], root / f"control-{path.stem}", path)
        original = control.read_bytes()
        input_frames = len(original) // (channels * 2)
        for speed in (.5, 1, 1.3, 2, 3):
            case = root / f"fixed-{path.stem}-{speed}"
            out = case / "output.pcm"
            p = Player([*runner, player], case, f'filesink location="{out}" sync=false')
            try:
                assert refreshed(p)["capabilities"] == "speed-v1"
                p.send(f"speed {speed}")
                p.wait(lambda s: float(s["speed"]) == speed)
                p.load(path)
                p.send("index 0")
                p.wait(lambda s: s["index"] == "0")
                p.wait(lambda s: s["state"] == "stopped")
                status = refreshed(p)
                assert status["speed_supported"] == "1"
                raw = out.read_bytes()
                pcm = np.frombuffer(raw, dtype="<i2").reshape(-1, channels)
                seconds = len(pcm) / rate
                assert abs(seconds - input_frames / rate / speed) < .015, (path, speed, seconds)
                pitches = [peak(pcm, rate, ch) for ch in range(channels)]
                expected_pitch = (440, 440 if path.stem.endswith("-1") else 733)
                assert all(abs(a-b) < 5 for a,b in zip(pitches, expected_pitch)), pitches
                if speed == 1:
                    assert raw == original, "1x must bypass Sonic byte-exactly"
                # Duration is source time even if output listening time shrinks.
                assert status["dur"] == "8"
                assert 7900 <= int(status["pos_ms"]) <= 8200, status
                report.append(dict(file=path.name, speed=speed, input_frames=input_frames,
                                   output_frames=len(pcm), output_seconds=seconds,
                                   pitch_hz=pitches, final_pos_ms=int(status["pos_ms"])))
            finally:
                p.close()
    return report


def interactions(player, root, media, runner=()):
    report = []
    for path, rate, channels in media:
        case = root / f"live-{path.stem}"
        out = case / "output.pcm"
        # GStreamer 0.10.25 filesink explicitly disables its get_times callback:
        # sync=true does not pace it. A clocked fakesink provides real timestamp
        # waits; the one-buffer branch bounds capture read-ahead on every API.
        p = Player([*runner, player], case,
                   'tee name=clocked ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 '
                   '! fakesink sync=true async=false clocked. ! '
                   f'filesink location="{out}" sync=false async=false buffer-mode=unbuffered')
        try:
            for arg in ("nan", "inf", "-inf", "0.499", "3.001", "1x", "", "1 2", "1e999"):
                p.send("speed " + arg)
                assert refreshed(p)["speed"] == "1", arg
            p.load(path)
            p.send("index 0")
            p.wait(lambda s: s["state"] == "playing")
            time.sleep(1.2)
            first = refreshed(p)
            assert 900 < int(first["pos_ms"]) < 1600, first
            p.send("speed 3")
            time.sleep(.8)
            second = refreshed(p)
            # A decoder read-ahead or retroactive latest-speed multiplier
            # would put this at the end, or ~6s, instead of ~3s.
            assert 2500 < int(second["pos_ms"]) < 4100, second
            p.send("pause")
            p.wait(lambda s: s["state"] == "paused")
            time.sleep(.15)
            frozen = refreshed(p)
            size = out.stat().st_size
            p.send("speed 0.5")
            time.sleep(.35)
            changed = refreshed(p)
            assert changed["state"] == "paused" and changed["pos_ms"] == frozen["pos_ms"]
            assert out.stat().st_size == size, "speed must not resume a paused sink"
            p.send("play")
            time.sleep(1.1)
            resumed = refreshed(p)
            delta = int(resumed["pos_ms"]) - int(frozen["pos_ms"])
            assert 350 < delta < 1600, (frozen, resumed)
            # Seek is in original media seconds, retains requested speed,
            # and seeking while paused must not start the sink.
            p.send("pause")
            p.wait(lambda s: s["state"] == "paused")
            p.send("seek 5")
            sought = p.wait(lambda s: s["pos_ms"] == "5000")
            assert sought["state"] == "paused" and sought["speed"] == "0.5"
            time.sleep(.1)
            size = out.stat().st_size
            time.sleep(.2)
            assert out.stat().st_size == size
            p.send("play")
            time.sleep(.7)
            after_seek = refreshed(p)
            assert 5200 < int(after_seek["pos_ms"]) < 5600, after_seek
            p.send("pause")
            p.wait(lambda s: s["state"] == "paused")
            begin = time.monotonic()
            p.send("stop")
            p.wait(lambda s: s["state"] == "stopped", timeout=2)
            stop_seconds = time.monotonic() - begin
            assert stop_seconds < 1, stop_seconds
            # New track resets media origin but keeps daemon-session speed.
            next_path = media[(media.index((path, rate, channels)) + 1) % len(media)][0]
            playlist = case / "two.m3u"
            playlist.write_text(str(path) + "\n" + str(next_path) + "\n")
            p.send("load " + str(playlist))
            p.wait(lambda s: s["count"] == "2")
            p.send("index 0")
            p.wait(lambda s: s["state"] == "playing")
            p.send("next")
            p.wait(lambda s: s["index"] == "1")
            time.sleep(.4)
            handoff = refreshed(p)
            assert handoff["path"] == str(next_path) and handoff["speed"] == "0.5"
            assert 100 < int(handoff["pos_ms"]) < 450, handoff
            p.send("speed 1")
            assert refreshed(p)["speed"] == "1"
            report.append(dict(file=path.name, first=first, live_3x=second,
                               paused=frozen, paused_change=changed, resumed=resumed,
                               seek=after_seek, handoff=handoff, stop_seconds=stop_seconds))
        finally:
            p.close()
    return report


def boundaries(player, root, media, runner=()):
    report = []
    for path, rate, channels in media:
        for seek in (5, 8, 9):
            case = root / f"seek-{path.stem}-{seek}"
            out = case / "output.pcm"
            p = Player([*runner, player], case, f'filesink location="{out}" sync=false')
            try:
                p.load(path)
                # This is a fixed-rate seek: queued preroll deliberately keeps
                # its old rate if speed is requested only AFTER decoder start.
                p.send(f"speed 2\nindex 0\npause\nseek {seek}\nplay")
                p.wait(lambda _: "Backend: EOS reached." in (case / "player.log").read_text())
                p.wait(lambda s: s["state"] == "stopped")
                seconds = out.stat().st_size / (rate * channels * 2)
                if seek == 9:
                    assert seconds == 0, (path, seconds)
                else:
                    # Opus is sample exact before stretching; AAC retains the
                    # accepted decoder's compressed-frame-granular seek.
                    tolerance = .015 if path.stem.startswith("opus") else .035
                    assert abs(seconds - (8.137 - seek) / 2) < tolerance, (path, seek, seconds)
                status = refreshed(p)
                assert status["speed"] == "2" and 7900 < int(status["pos_ms"]) < 8200
                report.append(dict(file=path.name, seek=seek, output_seconds=seconds, status=status))
            finally:
                p.close()
    case = root / "automatic-handoff"
    p = Player([*runner, player], case, "fakesink sync=false")
    try:
        playlist = case / "two.m3u"
        playlist.write_text(str(media[0][0]) + "\n" + str(media[1][0]) + "\n")
        p.send("speed 3\nload " + str(playlist) + "\nindex 0")
        status = p.wait(lambda s: s["index"] == "1" and s["state"] == "stopped")
        assert status["speed"] == "3" and status["path"] == str(media[1][0])
        assert (case / "player.log").read_text().count("Backend: EOS reached.") == 2
        report.append(dict(automatic_handoff=status))
    finally:
        p.close()
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--player", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--gst-api", default="1.0", choices=("1.0", "0.10"))
    parser.add_argument("--runner", default="", help="Execution prefix, e.g. qemu-arm -L PRIVATE_TARGET")
    args = parser.parse_args()
    runner = shlex.split(args.runner)
    assert not Path("/tmp/kinamp_audio_pipe").exists(), "Existing global FIFO: leave its owner alone"
    root = args.work.resolve()
    media = generate(root / "fixtures")
    execution = "execution runner" if runner else "x86-64 desktop"
    report = dict(execution=f"{execution}, GStreamer {args.gst_api} silent sinks", runner=runner,
                  fixed=fixed(str(args.player.resolve()), str(args.baseline.resolve()), root, media, runner),
                  interactions=interactions(str(args.player.resolve()), root, media, runner),
                  boundaries=boundaries(str(args.player.resolve()), root, media, runner))
    (root / "results.json").write_text(json.dumps(report, indent=2))
    print("PASS: 15 pitch/duration/EOF/1x controls; 3 live/paused/seek/handoff/stop sequences; 9 invalid speeds each")
    print("      9 speed-adjusted seek/near-EOF/beyond-EOF cases; automatic Opus-to-AAC handoff")
    print(root / "results.json")


if __name__ == "__main__":
    main()
