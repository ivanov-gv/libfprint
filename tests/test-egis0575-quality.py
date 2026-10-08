#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic-only checks for the offline extraction diagnostic."""
import csv
import io
import itertools
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
from unittest import mock

binary = sys.argv[1]
size = 103 * 52
for payload in (b"", bytes(size), bytes(size * 2 - 1), bytes(size * 2 + 1)):
    result = subprocess.run([binary], input=payload, capture_output=True, check=False)
    assert result.returncode == 2
    assert not result.stdout

result = subprocess.run([binary], input=bytes([128]) * (size * 2), capture_output=True, check=True)
rows = list(csv.DictReader(io.StringIO(result.stdout.decode())))
assert len(rows) == 18
actual = {(int(r["gain"]), int(r["scale"]), int(r["inverted"])) for r in rows}
assert actual == set(itertools.product(range(1, 4), range(1, 4), range(2)))
assert all(r["minutiae"] == "0" and r["extracted"] == "0" for r in rows)
assert not result.stderr
print("Offline diagnostic input/blank-image checks passed")

spec = importlib.util.spec_from_file_location("eh575_run", sys.argv[2])
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
with mock.patch("builtins.input", return_value=""), mock.patch.object(runner.subprocess, "run") as run:
    run.return_value = subprocess.CompletedProcess([], 0)
    assert runner.capture_series(binary, {}) == 0
    assert run.call_count == 5
    assert all(call.args[0] == [binary, "capture"] for call in run.call_args_list)
with mock.patch("builtins.input", return_value=""), mock.patch.object(runner.subprocess, "run") as run:
    run.return_value = subprocess.CompletedProcess([], 1)
    assert runner.capture_series(binary, {}) == 1
    assert run.call_count == 1
for exception in (KeyboardInterrupt, EOFError):
    with mock.patch("builtins.input", side_effect=exception), mock.patch.object(runner.subprocess, "run") as run:
        assert runner.capture_series(binary, {}) == 130
        run.assert_not_called()
print("Capture series readiness/failure/cancellation checks passed")

build = Path(binary).resolve().parents[1]
options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
if next((item["value"] for item in options if item["name"] == "egis0575_swipe"), False):
    for command in ("enroll", "verify"):
        result = subprocess.run([sys.executable, sys.argv[2], command, "--build", str(build)],
                                capture_output=True, check=False)
        assert result.returncode == 2
        assert b"capture-only" in result.stderr
        assert not result.stdout
    print("Experimental swipe enrollment/verification guard checks passed")
