#!/usr/bin/python3
"""Bounded local array comparison; no compiler, codec or FFmpeg invocation."""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path)
    parser.add_argument("case")
    parser.add_argument("native_pcm", type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.bundle / "manifest.json").read_text())
    case = next(c for c in manifest["cases"] if c["id"] == args.case)
    path = args.bundle / case["reference_pcm"]
    record = next(f for f in manifest["files"] if f["path"] == case["reference_pcm"])
    frames = case["expected_frames"]
    # Check sizes before reading: a bad input must not allocate arbitrary RAM.
    assert args.native_pcm.stat().st_size == frames * 4, "native frame count mismatch"
    assert path.stat().st_size == record["bytes"] and record["bytes"] <= 4 * 886728
    assert hashlib.sha256(path.read_bytes()).hexdigest() == record["sha256"], "reference checksum mismatch"
    native = np.fromfile(args.native_pcm, dtype="<i2").reshape(-1, 2)
    reference = np.fromfile(path, dtype="<i2").reshape(-1, 2)[case["reference_offset_frames"]:]
    assert len(reference) == frames
    delta = native.astype(float) - reference
    rms = float(np.sqrt(np.mean(delta**2))) if frames else 0
    first = float(np.sqrt(np.mean(delta[:4800]**2))) if frames else 0
    limits = case["thresholds"]
    passed = rms < limits["rmse_s16_max"] and first < limits["first_100ms_rmse_s16_max"]
    print(json.dumps(dict(case=args.case, frames=frames, rmse_s16=rms,
                          first_100ms_rmse_s16=first, thresholds=limits, passed=passed)))
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
