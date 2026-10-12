#!/usr/bin/python3
"""Orb-only reference decode/export. Never uses the candidate's PCM decoder."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

import numpy as np

from verify_opus import run


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("fixtures", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = args.fixtures.resolve()
    output = args.output.resolve()
    (output / "pcm").mkdir(parents=True, exist_ok=True)
    (output / "fixtures").mkdir(exist_ok=True)
    commands = []
    cases = []
    used = set()
    with tempfile.TemporaryDirectory() as scratch:
        def reference(name, frames):
            raw = Path(scratch) / "reference.pcm"
            command = ["ffmpeg", "-v", "error", "-c:a", "libopus", "-i", str(source / name),
                       "-ac", "2", "-ar", "48000", "-af", f"atrim=end_sample={frames}",
                       "-f", "s16le", "-y", str(raw)]
            run(command)
            commands.append(dict(fixture=name, end_sample=frames, command=command))
            data = np.fromfile(raw, dtype="<i2").reshape(-1, 2)
            assert len(data) == frames, (name, len(data), frames)
            used.add(name)
            return data

        def save(name, fixture, data, derivation, targets, origin=0):
            path = output / "pcm" / (name + ".s16le")
            data.astype("<i2").tofile(path)
            for target in targets:
                cases.append(dict(id=name if len(targets) == 1 else name + "-" + str(target),
                                  fixture="fixtures/" + fixture, target_sample=target,
                                  reference_pcm="pcm/" + path.name,
                                  reference_offset_frames=target - origin,
                                  expected_frames=len(data) - (target - origin),
                                  derivation=derivation,
                                  thresholds=dict(rmse_s16_max=150 if origin else 1,
                                                  first_100ms_rmse_s16_max=200 if origin else 1)))
            used.add(fixture)

        mono = reference("mono.mp4", 12486576)
        save("mono-251", "mono.mp4", mono[12048000:],
             "Decode from start with FFmpeg/libopus; trim to 12486576 frames; take [12048000:12486576]. No input-side seek.",
             [12048000], origin=12048000)
        del mono
        stereo = reference("stereo.mp4", 294576)
        overlap = np.concatenate((stereo[:18888], stereo[19368:66888], stereo[67613:]))
        save("overlap-full", "overlap.mp4", overlap,
             "Unchanged stereo payloads; first-presented wins: concatenate base[0:18888], base[19368:66888], base[67613:294576]. Remove 480+725 samples at packet 20/70 starts.", [0])
        gap = np.concatenate((stereo[:18888], np.zeros((480, 2), dtype="<i2"), stereo[18888:66888],
                              np.zeros((12713, 2), dtype="<i2"), stereo[66888:]))
        save("gap-full", "gap.mp4", gap,
             "Concatenate base[0:18888], 480 silent frames, base[18888:66888], 12713 silent frames, base[66888:294576]. No stretch/resampling.", [0])
        for name, total in (("silk", 294576), ("hybrid", 294576), ("transitions", 886728)):
            save(name + "-full", name + ".mp4", reference(name + ".mp4", total),
                 f"Independent FFmpeg/libopus decode from start; trim to the known {total}-sample presentation endpoint.", [0])
        for name, targets in (("lowdelay", [0, 119, 120, 121]), ("smallpacket", [0, 119, 120, 121, 311, 312])):
            save(name + "-boundary", name + ".mp4", reference(name + ".mp4", 54576),
                 "Independent decode from start, codec pre-skip already honored; trim to 54576 frames. Compare the indicated presentation-sample suffix; no candidate-derived expectation.", targets)

    files = []
    for name in sorted(used):
        shutil.copyfile(source / name, output / "fixtures" / name)
    for path in sorted([*(output / "pcm").iterdir(), *(output / "fixtures").iterdir()]):
        files.append(dict(path=str(path.relative_to(output)), bytes=path.stat().st_size, sha256=sha(path)))
    manifest = dict(format="s16le", sample_rate=48000, channels=2, bytes_per_frame=4,
                    provenance=dict(ffmpeg=run(["ffmpeg", "-version"]).stdout.decode(), numpy=np.__version__,
                                    reference_decoder="installed FFmpeg/libopus, not KinAMP or its pinned FFmpeg build"),
                    rules="Exact frame count and fixture/reference hashes required. RMS is over both S16 channels; first100ms means min(4800,remaining) frames. Bounds are exclusive. Do not infer audible quality.",
                    commands=commands, cases=cases, files=files)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Exported {len(cases)} cases, {len(files)} public fixture/reference files: {output}")


if __name__ == "__main__":
    main()
