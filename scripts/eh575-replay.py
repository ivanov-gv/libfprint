#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Replay PRIVATE prototype NPZ captures through native extraction; print aggregates only.

Requires NumPy for this diagnostic only. Does not import Python enrollments into
native authentication, open USB, save images or send data over the network.
"""
import argparse
import csv
import io
import json
import os
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Private enrollment.npz or probe.npz")
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--deps", type=Path)
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Do not run diagnostics as root")
    try:
        import numpy as np
    except ImportError:
        parser.error("Replay requires NumPy; use the existing prototype virtual environment")
    build = args.build.resolve(strict=True)
    options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    if next(item["value"] for item in options if item["name"] == "drivers") != "egis0575":
        parser.error("Use an isolated -Ddrivers=egis0575 build")
    binary = build / "tests/eh575-quality"
    if not binary.is_file():
        parser.error("Build the eh575-quality diagnostic first")
    try:
        with np.load(args.input, allow_pickle=False) as data:
            images = data["images"]
            if images.dtype != np.uint8 or images.shape[-2:] != (52, 103):
                parser.error("Expected uint8 images of shape (touches, frames, 52, 103) or (frames, 52, 103)")
            if images.ndim == 3:
                images = images[None]
            if images.ndim != 4 or not 1 <= len(images) <= 128 or not 1 <= images.shape[1] <= 5:
                parser.error("Expected 1..128 touches with 1..5 frames each")
            backgrounds = data["backgrounds"] if "backgrounds" in data else data["background"][None]
            if backgrounds.dtype != np.uint8 or backgrounds.shape not in ((1, 52, 103), (len(images), 52, 103)):
                parser.error("Expected one measured empty reference per touch, or one shared reference")
            frames = np.median(images, axis=1).astype(np.uint8)
            backgrounds = np.broadcast_to(backgrounds, (len(images), 52, 103)).copy()
    except (OSError, ValueError, KeyError) as error:
        parser.error(f"Cannot load private recording: {error}")
    env = os.environ.copy()
    libraries = [str(build / "libfprint")]
    if args.deps:
        libraries.append(str(args.deps.resolve(strict=True) / "usr/lib/x86_64-linux-gnu"))
    env["LD_LIBRARY_PATH"] = ":".join(libraries)
    # Do not inherit debug logging, which can disclose minutia coordinates.
    env.pop("G_MESSAGES_DEBUG", None)
    results = {}
    for frame, background in zip(frames, backgrounds):
        result = subprocess.run([str(binary)], input=frame.tobytes() + background.tobytes(),
                                capture_output=True, env=env, check=True, timeout=30)
        for row in csv.DictReader(io.StringIO(result.stdout.decode())):
            key = tuple(int(row[name]) for name in ("gain", "scale", "inverted"))
            results.setdefault(key, []).append(int(row["minutiae"]))
    print(f"Replayed {len(frames)} touch medians. Extraction counts are not matching or security evidence.")
    print("gain scale inverted min median max zero-extractions")
    for key, counts in sorted(results.items()):
        marker = " (current driver)" if key == (2, 2, 0) else ""
        print(*key, min(counts), statistics.median(counts), max(counts), counts.count(0), end=marker + "\n")


if __name__ == "__main__":
    main()
