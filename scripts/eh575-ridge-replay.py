#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Replay PRIVATE recorded audit through native ridge matching in memory.

NumPy is diagnostic-only. No sensor, templates, images or biometric coordinates
are output, imported, published or saved. Existing recordings are development
data, not fresh independent validation of this native implementation.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import statistics
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('enrollment', type=Path)
    parser.add_argument('audit', type=Path)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--deps', type=Path)
    parser.add_argument('--baseline', type=Path, help='Previously built diagnostic binary; compare decisions/timing without saving biometrics')
    parser.add_argument('--repeats', type=int, choices=(1, 2, 3), default=1)
    args = parser.parse_args()
    import numpy as np
    with np.load(args.enrollment, allow_pickle=False) as data:
        images, backgrounds = data['images'], data['backgrounds']
        finger = str(data['finger'])
        if images.dtype != np.uint8 or images.ndim != 4 or images.shape[2:] != (52, 103) or not 6 <= len(images) <= 24 or not 1 <= images.shape[1] <= 5:
            parser.error('Expected 6..24 bounded enrollment touches')
        if backgrounds.dtype != np.uint8 or backgrounds.shape != (len(images), 52, 103):
            parser.error('Expected one measured background per enrollment touch')
        medians = np.median(images, axis=1).astype(np.uint8)
    gallery = struct.pack('<I', len(medians)) + b''.join(im.tobytes() + bg.tobytes() for im, bg in zip(medians, backgrounds))
    binary = args.build.resolve(strict=True) / 'tests/eh575-ridge-check'
    baseline = args.baseline.resolve(strict=True) if args.baseline else None
    timings = {name: {'genuine': [], 'wrong_finger': []} for name in ('current', 'baseline')}
    env = os.environ.copy()
    env.pop('G_MESSAGES_DEBUG', None)
    paths = [str(args.build.resolve() / 'libfprint')]
    if args.deps:
        root = args.deps.resolve(strict=True) / 'usr/lib/x86_64-linux-gnu'
        paths += [str(root), str(root / 'openblas-pthread')]
    env['LD_LIBRARY_PATH'] = ':'.join(paths)
    counts = dict(genuine_trials=0, genuine_accepts=0, wrong_finger_trials=0, wrong_finger_accepts=0, invalid_or_failed=0)
    files = sorted(args.audit.glob('*.npz'))
    if not 1 <= len(files) <= 128:
        parser.error('Expected 1..128 audit probes')
    for index, path in enumerate(files):
        with np.load(path, allow_pickle=False) as data:
            images, background = data['images'], data['background']
            actual = str(data['finger'])
            if images.dtype != np.uint8 or images.shape != (5, 52, 103) or background.dtype != np.uint8 or background.shape != (52, 103):
                parser.error('Expected five probe frames and a measured background')
            payload = gallery + images.tobytes() + background.tobytes()
        group = 'genuine' if actual == finger else 'wrong_finger'
        decisions = {}
        for repeat in range(args.repeats):
            programs = [('current', binary)] + ([('baseline', baseline)] if baseline else [])
            if (index + repeat) % 2:
                programs.reverse()
            for name, program in programs:
                start = time.perf_counter()
                result = subprocess.run([str(program)], input=payload, env=env, capture_output=True, check=True, timeout=30)
                elapsed = (time.perf_counter() - start) * 1000
                if result.stderr:
                    parser.error('Diagnostic emitted unexpected stderr; no biometric output will be forwarded')
                value = json.loads(result.stdout)
                if name in decisions and (value['accepted'], value['status']) != (decisions[name]['accepted'], decisions[name]['status']):
                    parser.error('Repeated diagnostic decision changed; do not deploy this build')
                decisions[name] = value
                timings[name][group].append(elapsed)
            if baseline and (decisions['current']['accepted'], decisions['current']['status']) != (decisions['baseline']['accepted'], decisions['baseline']['status']):
                parser.error('Baseline/current decision mismatch; do not deploy this build')
        decision = decisions['current']
        counts[group + '_trials'] += 1
        counts[group + '_accepts'] += bool(decision['accepted'])
        counts['invalid_or_failed'] += decision['status'] in (3, 5)
    print(json.dumps(counts, indent=2))
    if baseline:
        summary = {name: {group: {'runs': len(values), 'median_ms': round(statistics.median(values), 1),
                                 'max_ms': round(max(values), 1)} for group, values in groups.items() if values}
                   for name, groups in timings.items()}
        print(json.dumps({'all_decisions_match_baseline': True, 'process_inclusive_timing': summary}, indent=2))
    print('Historical development replay only; not authorization to enable login.')

if __name__ == '__main__':
    main()
