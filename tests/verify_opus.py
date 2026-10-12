#!/usr/bin/python3
"""Synthetic media, public decode API and actual KinAMP FIFO protocol tests.

Run only in an isolated environment: upstream owns /tmp/kinamp_audio_pipe.
Requires ffmpeg/ffprobe and numpy; never opens a device or plays audible audio.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import time

import numpy as np


def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError((command, result.returncode, result.stderr.decode()))
    return result


def fixtures(work):
    work.mkdir(parents=True, exist_ok=True)
    mono = "0.25*sin(2*PI*(173*t+3.1*t*t))+0.1*sin(2*PI*997*t)*(0.5+0.5*sin(2*PI*0.37*t))"
    stereo = mono + "|0.17*sin(2*PI*(479*t+1.7*t*t))"
    for name, expr, duration, codec in [
        ("mono.mp4", mono, "260.137", "libopus"),
        ("stereo.mp4", stereo, "6.137", "libopus"),
        ("surround.mp4", "|".join(f"0.08*sin(2*PI*{f}*t)" for f in (173, 479, 997, 83, 353, 701)), "1.137", "libopus"),
        ("aac.mp4", stereo, "12.137", "aac"),
        ("aac-mono.mp4", mono, "1.137", "aac"),
        ("mp3.mp3", stereo, "12.137", "libmp3lame"),
        ("flac.flac", stereo, "12.137", "flac"),
        ("wav.wav", stereo, "12.137", "pcm_s16le"),
        ("vorbis.ogg", stereo, "12.137", "libvorbis"),
    ]:
        path = work / name
        if not path.exists():
            run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"aevalsrc={expr}:s=48000:d={duration}",
                 "-c:a", codec, "-metadata", "title=Synthetic Opus chapter", "-metadata", "artist=Prototype fixture", "-y", path])
    shutil.copyfile(work / "stereo.mp4", work / "misnamed.bin")
    lowdelay = work / "lowdelay.mp4"
    if not lowdelay.exists():
        run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"aevalsrc={mono}:s=48000:d=1.137",
             "-c:a", "libopus", "-application", "lowdelay", "-y", lowdelay])
    longpacket = work / "longpacket.mp4"
    if not longpacket.exists():
        # A 60-ms stereo PCM packet exceeds PIPE_BUF: a single nonblocking write
        # can be partial. The complete FIFO capture below must retain every byte.
        run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"aevalsrc={stereo}:s=48000:d=1.137",
             "-c:a", "libopus", "-frame_duration", "60", "-y", longpacket])
    if not (work / "smallpacket.mp4").exists():
        # Minimum packet duration exercises all 32 preroll packets and pre-skip
        # spanning multiple packets, unlike the usual 20-ms CELT fixtures.
        run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"aevalsrc={stereo}:s=48000:d=1.137",
             "-c:a", "libopus", "-frame_duration", "2.5", "-y", work / "smallpacket.mp4"])
    for name, bitrate, extra in (("silk.mp4", "12k", []), ("hybrid.mp4", "24k", []),
                                 ("silk-fec.mp4", "12k", ["-fec", "1", "-packet_loss", "25", "-frame_duration", "60"])):
        if not (work / name).exists():
            run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", f"aevalsrc={mono}:s=48000:d=6.137",
                 "-c:a", "libopus", "-application", "voip", "-vbr", "off", "-b:a", bitrate,
                 *extra, "-y", work / name])
    # Nonuniform, monotonic packet timestamps. Payloads are unchanged. Two
    # overlaps reproduce +1205 excess samples; a long gap tests held-frame state.
    for name, sign, second in (("overlap.mp4", "-", 725), ("gap.mp4", "+", 12713)):
        if not (work / name).exists():
            expr = f"setts=ts=TS{sign}if(gte(N\\,20)\\,480\\,0){sign}if(gte(N\\,70)\\,{second}\\,0)"
            run(["ffmpeg", "-v", "error", "-i", work / "stereo.mp4", "-c:a", "copy",
                 "-bsf:a", expr, "-movie_timescale", "48000", "-y", work / name])
    if not (work / "transitions.mp4").exists():
        concat = work / "modes.txt"
        concat.write_text("".join(f"file '{(work / name).resolve()}'\n"
                                  for name in ("stereo.mp4", "silk.mp4", "hybrid.mp4")))
        # Normalize timestamps, not payloads: each Opus packet is 960 samples.
        # Independent encoders intentionally change mode/predictor at boundaries.
        run(["ffmpeg", "-v", "error", "-f", "concat", "-safe", "0", "-i", concat,
             "-c:a", "copy", "-bsf:a", "setts=ts=N*960-312:duration=960",
             "-movie_timescale", "48000", "-y", work / "transitions.mp4"])
    # Synthetic dOps output gain: +6.0195 dB (Q7.8). No clipping in this fixture.
    gained = bytearray((work / "stereo.mp4").read_bytes())
    at = gained.index(b"dOps")
    gained[at + 12:at + 14] = (1541).to_bytes(2, "big", signed=True)
    (work / "gain.mp4").write_bytes(gained)
    metadata = work / "metadata.m4b"
    if not metadata.exists() or metadata.stat().st_size == 0:
        chapters = work / "chapters.ffmetadata"
        chapters.write_text(";FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=1250\ntitle=First\n"
                            "[CHAPTER]\nTIMEBASE=1/1000\nSTART=1250\nEND=6137\ntitle=Second\n")
        cover = work / "cover.png"
        cover.write_bytes(base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6ioAAAAASUVORK5CYII="))
        run(["ffmpeg", "-v", "error", "-i", work / "stereo.mp4", "-f", "ffmetadata", "-i", chapters,
             "-i", cover, "-map", "0:a:0", "-map", "2:v:0", "-map_chapters", "1", "-c", "copy",
             "-disposition:v:0", "attached_pic", "-metadata", "title=Metadata Opus",
             "-metadata", "artist=Prototype fixture", "-metadata", "album=Prototype Album", "-f", "mp4", "-y", metadata])
    return work


def pcm_tests(args, media, evidence):
    results = []
    counts = {"mono.mp4": 12486576, "stereo.mp4": 294576, "surround.mp4": 54576,
              "misnamed.bin": 294576, "gain.mp4": 294576, "lowdelay.mp4": 54576,
              "longpacket.mp4": 54576, "smallpacket.mp4": 54576}
    for name, total in counts.items():
        reference = evidence / "reference.pcm"
        # Independent, installed FFmpeg/libopus decoder; trim to the known
        # synthetic presentation length (MP4's last encoded packet is longer).
        run(["ffmpeg", "-v", "error", "-c:a", "libopus", "-i", media / name, "-ac", "2", "-ar", "48000",
             "-af", f"atrim=end_sample={total}", "-f", "s16le", "-y", reference])
        ref = np.fromfile(reference, dtype="<i2").reshape(-1, 2)
        assert len(ref) == total
        if name == "gain.mp4":
            plain = evidence / "plain.pcm"
            run(["ffmpeg", "-v", "error", "-c:a", "opus", "-i", media / "stereo.mp4", "-ac", "2",
                 "-af", f"atrim=end_sample={total}", "-f", "s16le", "-y", plain])
            original = np.fromfile(plain, dtype="<i2").astype(float)
            ratio = np.sqrt(np.mean(ref.astype(float)**2)) / np.sqrt(np.mean(original**2))
            assert 1.99 < ratio < 2.01, ratio
            plain.unlink()
        targets = [0, 1, 311, 312, 3840, 48000, 48313, total - 1, total, total + 123]
        if name == "mono.mp4":
            targets += [251 * 48000, 259 * 48000 + 45120]
        if name in ("lowdelay.mp4", "smallpacket.mp4"):
            targets += [119, 120, 121]
        for target in targets:
            out = evidence / "decoded.pcm"
            command = args.runner + [args.pcm, str(media / name), str(target), str(out)]
            result = run(command).stdout.decode().strip()
            data = np.fromfile(out, dtype="<i2").reshape(-1, 2)
            assert len(data) == max(0, total - target), (name, target, len(data))
            expected = ref[target:]
            delta = data.astype(float) - expected
            rmse = float(np.sqrt(np.mean(delta**2))) if len(data) else 0
            first_rmse = float(np.sqrt(np.mean(delta[:4800]**2))) if len(data) else 0
            # Coded-packet preroll restores predictor state, but need not be bit-identical
            # to decode-from-start. A 1-sample shift fails by hundreds of counts.
            # Single-sample EOF cases test trimming/count, not statistical quality:
            # their recorded residual can be larger than a 100-ms RMS average.
            if len(data) >= 4800:
                assert rmse < 150 and first_rmse < 200, (name, target, rmse, first_rmse)
            if name == "mono.mp4" and len(data):
                assert np.array_equal(data[:, 0], data[:, 1])
            if name in ("stereo.mp4", "misnamed.bin") and len(data) > 100:
                assert np.sqrt(np.mean((data[:, 0].astype(float) - data[:, 1])**2)) > 1000
            if target == 251 * 48000:
                shifted = ref[target + 1:target + len(data)]
                wrong_rmse = float(np.sqrt(np.mean((data[:-1].astype(float) - shifted)**2)))
                assert wrong_rmse > 20 * rmse
            results.append(dict(media=name, target_sample=target, frames=len(data), rmse_s16=rmse,
                                first_100ms_rmse_s16=first_rmse, output=result))
        reference.unlink()
    (evidence / "decoded.pcm").unlink()
    return results


def timing_tests(args, media, evidence):
    results = []
    plain_path = evidence / "timing-reference.pcm"
    run(["ffmpeg", "-v", "error", "-c:a", "libopus", "-i", media / "stereo.mp4", "-ac", "2",
         "-f", "s16le", "-y", plain_path])
    plain = np.fromfile(plain_path, dtype="<i2").reshape(-1, 2)[:294576]
    for name in ("overlap.mp4", "gap.mp4", "silk.mp4", "hybrid.mp4", "silk-fec.mp4", "transitions.mp4"):
        # TOC proves which modes were actually generated, rather than inferring
        # them from encoder flags or filenames. No private media is used.
        probe = json.loads(run(["ffprobe", "-v", "error", "-select_streams", "a", "-show_packets",
                                "-show_data", "-show_streams", "-of", "json", media / name]).stdout)
        configs = {}
        for packet in probe["packets"]:
            config = int(packet["data"].split(":")[1].strip().split()[0][:2], 16) >> 3
            configs[config] = configs.get(config, 0) + 1
        if name.startswith("silk"):
            assert all(c < 12 for c in configs), configs
        if name == "hybrid.mp4":
            assert all(12 <= c < 16 for c in configs), configs
        if name == "transitions.mp4":
            assert any(c < 12 for c in configs) and any(12 <= c < 16 for c in configs) and any(c >= 16 for c in configs)
        total = int(probe["streams"][0]["duration_ts"])
        assert probe["streams"][0]["time_base"] == "1/48000"
        if name == "overlap.mp4":
            # First-presented PCM wins. Remove the start of packets 20/70,
            # not the end of the previous packet or an arbitrary final tail.
            expected = np.concatenate((plain[:18888], plain[19368:66888], plain[67613:]))
            assert total == 294576 - 1205
        elif name == "gap.mp4":
            expected = np.concatenate((plain[:18888], np.zeros((480, 2), dtype="<i2"),
                                       plain[18888:66888], np.zeros((12713, 2), dtype="<i2"), plain[66888:]))
            assert total == 294576 + 13193
        else:
            run(["ffmpeg", "-v", "error", "-c:a", "libopus", "-i", media / name, "-ac", "2",
                 "-f", "s16le", "-y", plain_path])
            expected = np.fromfile(plain_path, dtype="<i2").reshape(-1, 2)[:total]
            if name != "transitions.mp4":
                assert total == 294576
            else:
                # Three 308-packet sections, 960 samples each, 312 pre-skip.
                assert total == 886728
        assert len(expected) == total
        targets = [0, 18887, 18889, 19128, 48000, 66407, 67001, 73013, total - 1, total, total + 123]
        if name == "transitions.mp4":
            targets += [308 * 960 + 731, 616 * 960 + 827, 700000]
        for target in targets:
            out = evidence / "timing-decoded.pcm"
            log = run(args.runner + [args.pcm, str(media / name), str(target), str(out)]).stdout.decode().strip()
            data = np.fromfile(out, dtype="<i2").reshape(-1, 2)
            assert len(data) == max(0, total - target), (name, target, len(data), total)
            delta = data.astype(float) - expected[target:]
            rmse = float(np.sqrt(np.mean(delta**2))) if len(data) else 0
            first = float(np.sqrt(np.mean(delta[:4800]**2))) if len(data) else 0
            # Full decode must agree tightly. Seeks warm up predictors, not
            # restore arbitrary encoder history bit-for-bit (especially SILK).
            if target == 0:
                assert rmse < 1, (name, rmse)
            elif len(data) >= 4800:
                assert rmse < 150, (name, target, rmse)
            results.append(dict(media=name, toc_configs=configs, target_sample=target,
                                frames=len(data), rmse_s16=rmse, first_100ms_rmse_s16=first, output=log))
        out.unlink()
    plain_path.unlink()
    return results


class Player:
    def __init__(self, command, root, sink):
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        # These are this harness's owned runtime directories. A crashed prior
        # test must not let stale status satisfy startup before FIFO recreation.
        for name in ("kinamp_cmd", "kinamp_status", "kinamp_status.tmp"):
            (root / name).unlink(missing_ok=True)
        self.log = (root / "player.log").open("w")
        env = dict(os.environ, HOME=str(root), KINAMP_RUNTIME_DIR=str(root), KINAMP_SINK=sink)
        self.proc = subprocess.Popen(command + ["--idle"], cwd=root, env=env, stdout=self.log, stderr=self.log)
        self.wait(lambda s: s.get("state") == "stopped")
        self.snapshots = []

    def status(self):
        try:
            return dict(line.split("=", 1) for line in (self.root / "kinamp_status").read_text().splitlines())
        except FileNotFoundError:
            return {}

    def wait(self, condition, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            state = self.status()
            if condition(state):
                return state
            assert self.proc.poll() is None, (self.proc.returncode, (self.root / "player.log").read_text())
            time.sleep(.02)
        raise AssertionError(("timeout", state, (self.root / "player.log").read_text()[-2000:]))

    def send(self, command):
        with (self.root / "kinamp_cmd").open("w") as fifo:
            fifo.write(command + "\n")

    def snapshot(self):
        before = (self.root / "player.log").read_text().count("Command: status\n")
        self.send("status")
        self.wait(lambda _: (self.root / "player.log").read_text().count("Command: status\n") > before)
        time.sleep(.02)  # logging precedes the atomic status rewrite
        s = self.status()
        self.snapshots.append(s)
        return s

    def load(self, media):
        playlist = self.root / "one.m3u"
        playlist.write_text(str(media) + "\n")
        self.send("load " + str(playlist))
        self.wait(lambda s: s.get("count") == "1" and s.get("index") == "-1")

    def close(self):
        if self.proc.poll() is None:
            self.send("quit")
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                # Only the subprocess owned by this test; never pkill KinAMP.
                self.proc.kill()
                self.proc.wait()
                raise
        self.log.close()
        assert self.proc.returncode == 0
        assert not (self.root / "kinamp_cmd").exists()
        assert not (self.root / "kinamp_status").exists()
        assert not Path("/tmp/kinamp_audio_pipe").exists()
        (self.root / "snapshots.json").write_text(json.dumps(self.snapshots, indent=2))


def capture(command, root, media, seek=None):
    pcm = root / "fifo.pcm"
    p = Player(command, root, f'filesink location="{pcm}" sync=false')
    try:
        p.load(media)
        p.send("index 0" + (f"\nseek {seek}" if seek is not None else ""))
        p.wait(lambda s: s.get("index") == "0")
        p.wait(lambda s: s.get("state") == "stopped", timeout=60)
        snapshot = p.snapshot()
        if media.name == "metadata.m4b":
            assert snapshot["title"] == "Metadata Opus" and snapshot["album"] == "Prototype Album"
            assert snapshot["artist"] == "Prototype fixture" and snapshot["dur"] == "6"
            assert Path(snapshot["cover"]).read_bytes() == (media.parent / "cover.png").read_bytes()
        assert "Backend: EOS reached." in (root / "player.log").read_text()
    finally:
        p.close()
    return pcm


def controls(args, media, evidence):
    result = []
    for name in ("aac.mp4", "aac-mono.mp4", "mp3.mp3", "flac.flac", "wav.wav", "vorbis.ogg"):
        for seek in (None, 3) if name != "aac-mono.mp4" else (None,):
            label = name + "-" + str(seek)
            before = capture(args.runner + [args.baseline], evidence / ("before-" + label), media / name, seek)
            after = capture(args.runner + [args.player], evidence / ("after-" + label), media / name, seek)
            assert before.stat().st_size > 0
            assert before.read_bytes() == after.read_bytes(), (name, seek, "PCM changed")
            result.append(dict(format=name, seek=seek, bytes=after.stat().st_size, byte_identical=True))
            before.unlink()
            after.unlink()
    return result


def interaction(args, media, evidence, binary, label, name):
    root = evidence / label
    pcm = root / "fifo.pcm"
    p = Player(args.runner + [binary], root, f'identity sleep-time=20000 ! filesink location="{pcm}" sync=false')
    result = {}
    try:
        p.load(media / name)
        p.send("index 0")
        p.wait(lambda s: s.get("state") == "playing")
        if name == "mono.mp4":
            p.send("seek 251")
            p.wait(lambda s: int(s.get("pos", 0)) >= 251)
        time.sleep(1.3)
        p.send("pause")
        paused = p.wait(lambda s: s.get("state") == "paused")
        time.sleep(.35)  # allow one already-dispatched sink buffer to complete
        size = pcm.stat().st_size
        time.sleep(.7)
        assert pcm.stat().st_size == size
        assert p.snapshot()["pos"] == paused["pos"]
        p.send("play")
        p.wait(lambda s: s.get("state") == "playing")
        time.sleep(.7)
        assert pcm.stat().st_size > size
        resumed = p.snapshot()
        result["pause_pos"] = paused["pos"]
        result["resume_pos"] = resumed["pos"]
        result["position_regressed"] = int(resumed["pos"]) < int(paused["pos"])
        p.send("pause")
        p.wait(lambda s: s.get("state") == "paused")
        began = time.monotonic()
        p.send("stop")
        p.wait(lambda s: s.get("state") == "stopped", timeout=3)
        result["stop_while_paused_seconds"] = time.monotonic() - began
        # Exercise natural decoder exit + explicit join repeatedly. A leaked
        # joinable thread or file descriptor would grow across these cycles.
        counts = []
        virtual_kb = []
        for i in range(12):
            p.send("play")
            p.wait(lambda s: s.get("state") == "playing")
            p.send("seek " + ("260" if name == "mono.mp4" else "11"))
            if name == "mono.mp4":
                p.wait(lambda s: s.get("state") == "stopped")
            else:
                time.sleep(.1)
                p.send("stop")
                p.wait(lambda s: s.get("state") == "stopped")
            counts.append(len(list(Path(f"/proc/{p.proc.pid}/fd").iterdir())))
            proc_status = Path(f"/proc/{p.proc.pid}/status").read_text().splitlines()
            virtual_kb.append(int(next(line for line in proc_status if line.startswith("VmSize:")).split()[1]))
        if name == "mono.mp4":
            assert max(counts[2:]) - min(counts[2:]) <= 1, counts
            # An unjoined decoder retains its multi-MB thread stack each cycle.
            assert virtual_kb[-1] - virtual_kb[2] < 16384, virtual_kb
        result["fd_counts_after_stop"] = counts
        result["virtual_kb_after_stop"] = virtual_kb
    finally:
        p.close()
    pcm.unlink()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--player", required=True)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--pcm", required=True)
    parser.add_argument("--aac-probe", required=True)
    parser.add_argument("--runner", default="")
    args = parser.parse_args()
    args.runner = shlex.split(args.runner)
    args.work = args.work.resolve()
    for key in ("player", "baseline", "pcm", "aac_probe"):
        setattr(args, key, str(Path(getattr(args, key)).resolve()))
    assert not Path("/tmp/kinamp_audio_pipe").exists(), "Do not collide with an existing player"
    media = fixtures(args.work / "fixtures")
    evidence = args.work / "evidence"
    evidence.mkdir(exist_ok=True)
    rejection = subprocess.run(args.runner + [args.aac_probe, str(media / "mono.mp4")], capture_output=True)
    text = (rejection.stdout + rejection.stderr).decode()
    assert rejection.returncode == 3 and "atom 'mp4a' not found" in text, text
    (evidence / "unchanged-rejection.log").write_text(text)
    run(args.runner + [args.aac_probe, str(media / "aac-mono.mp4")])
    report = dict(execution="QEMU ARM emulation" if args.runner else "native x86_64 desktop",
                  unchanged_rejection=text, pcm=pcm_tests(args, media, evidence),
                  timing=timing_tests(args, media, evidence),
                  legacy_controls=controls(args, media, evidence))
    for name, seek, frames in (("mono.mp4", None, 12486576), ("mono.mp4", 251, 438576),
                               ("mono.mp4", 260, 6576), ("misnamed.bin", None, 294576),
                               ("metadata.m4b", None, 294576), ("longpacket.mp4", None, 54576),
                               ("overlap.mp4", None, 293371), ("gap.mp4", None, 307769)):
        out = capture(args.runner + [args.player], evidence / f"opus-{name}-{seek}", media / name, seek)
        assert out.stat().st_size == frames * 4, (name, seek, out.stat().st_size)
        if name == "longpacket.mp4":
            expected = evidence / "longpacket-expected.pcm"
            run(args.runner + [args.pcm, str(media / name), "0", str(expected)])
            assert out.read_bytes() == expected.read_bytes()
            expected.unlink()
        out.unlink()
    report["opus_interaction"] = interaction(args, media, evidence, args.player, "opus-interaction", "mono.mp4")
    report["baseline_aac_interaction"] = interaction(args, media, evidence, args.baseline, "baseline-aac-interaction", "aac.mp4")
    report["patched_aac_interaction"] = interaction(args, media, evidence, args.player, "patched-aac-interaction", "aac.mp4")
    before_fds = report["baseline_aac_interaction"]["fd_counts_after_stop"]
    after_fds = report["patched_aac_interaction"]["fd_counts_after_stop"]
    # Record the upstream AAC leak honestly, and require the patch not to add
    # another one. The Opus path has its own strict no-growth assertion above.
    assert after_fds[-1] - after_fds[0] == before_fds[-1] - before_fds[0]
    (evidence / "results.json").write_text(json.dumps(report, indent=2))
    print(f"PASS: {len(report['pcm'])} sample/seek cases, {len(report['legacy_controls'])} byte-identical legacy controls,")
    print(f"      {len(report['timing'])} overlap/gap/SILK/hybrid/mode-transition cases,")
    print("      8 FIFO/EOF captures, tags/cover, Opus and before/after AAC pause/resume/stop/cleanup.")
    print("Known upstream status-position regression:", report["baseline_aac_interaction"]["position_regressed"])
    print("Known upstream AAC FD growth (before/after):", before_fds, after_fds)
    print("Results:", evidence / "results.json")


if __name__ == "__main__":
    main()
