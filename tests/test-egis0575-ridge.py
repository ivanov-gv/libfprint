#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic-only native matcher rejection/identity/input/cancel checks."""
import json
import math
import random
import struct
import subprocess
import sys

binary = sys.argv[1]
size = 103 * 52
background = bytes([128]) * size
def noise(seed):
    rng = random.Random(seed)
    return bytes(rng.randrange(40, 211) for _ in range(size))
def payload(reference, probe, gallery=None, frames=None):
    gallery = gallery or [reference] * 6
    frames = frames or [probe] * 5
    return struct.pack('<I', len(gallery)) + b''.join(image + background for image in gallery) + b''.join(frames) + background
def check(reference, probe, cancel=False, gallery=None, frames=None):
    result = subprocess.run([binary] + (['cancel'] if cancel else []),
                            input=payload(reference, probe, gallery, frames), capture_output=True, check=True, timeout=100)
    assert not result.stderr
    return json.loads(result.stdout)
for data in (b'', bytes(4), struct.pack('<I', 25), struct.pack('<I', 6) + bytes(size)):
    result = subprocess.run([binary], input=data, capture_output=True, check=False)
    assert result.returncode == 2 and not result.stdout
a = noise(1)
assert check(a, a)['accepted']
# An early valid match must not hide a bad LAST gallery area.
assert check(a, a, gallery=[a] * 14 + [background])['status'] == 3
# Gallery acceptance remains an OR, including a match in the LAST area.
b = noise(2)
assert check(a, a, gallery=[b] * 14 + [a])['accepted']
# Still require at least three of the SAME five frames against ONE area.
assert not check(a, a, frames=[a, a, b, b, b])['accepted']
assert check(a, a, frames=[a, a, a, b, b])['accepted']
# Two different enrolled areas cannot pool their two matching frames.
assert not check(a, a, gallery=[a, b] * 3, frames=[a, a, b, b, noise(3)])['accepted']
assert not check(a, noise(2))['accepted']
assert check(a, background)['status'] == 2
assert not check(background, a)['accepted']
assert check(a, a, True)['status'] == 4
periodic = bytes(int(128 + 60 * math.sin(2 * math.pi * (i // 103) / 7)) for i in range(size))
assert not check(periodic, periodic)['accepted']
print('Native ridge synthetic identity/wrong-texture/blank/ambiguity/input/cancel checks passed')
