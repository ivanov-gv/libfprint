#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run a local native hardware test, never the system authentication stack."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def capture_series(binary, env):
    print("Five independent native captures. No images or templates will be saved.", flush=True)
    try:
        for trial in range(1, 6):
            input(f"Trial {trial}/5: lift fully, leave the reader empty, then press Enter: ")
            print("Wait for calibration, then touch with a small placement variation.", flush=True)
            result = subprocess.run([str(binary), "capture"], env=env, check=False)
            if result.returncode:
                print("Series stopped after an unsuccessful capture; no automatic retry.", flush=True)
                return result.returncode if result.returncode > 0 else 1
    except (KeyboardInterrupt, EOFError):
        print("\nCapture series stopped.", flush=True)
        return 130
    print("Completed 5/5 captures. Minutiae counts do not establish matching reliability.", flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("open", "idle", "capture", "capture-series", "enroll", "verify"))
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--deps", type=Path, help="Optional extracted dependency prefix, not a system installation")
    parser.add_argument("--state", type=Path, default=Path(__file__).resolve().parents[1] / ".state/eh575")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Do not run this prototype as root")
    build = args.build.resolve(strict=True)
    options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    drivers = next(item["value"] for item in options if item["name"] == "drivers")
    if drivers != "egis0575":
        parser.error("Use an isolated build with -Ddrivers=egis0575")
    binary = build / "tests/eh575-smoke"
    if not binary.is_file():
        parser.error("Build the EH575 smoke-test executable first")
    # Reject symlinks in the private artifact path before creating directories.
    state = args.state.absolute()
    if any(part.is_symlink() for part in (state, *state.parents)):
        parser.error("Private test state must not use symlink directories")
    state.mkdir(parents=True, mode=0o700, exist_ok=True)
    if state.stat().st_uid != os.getuid():
        parser.error("Private test state must be owned by you")
    state.chmod(0o700)
    template = state / "native-print.bin"
    if template.is_symlink() or (template.exists() and
            (template.stat().st_uid != os.getuid() or template.stat().st_mode & 0o077)):
        parser.error("Native template has unsafe ownership or permissions")
    env = os.environ.copy()
    libraries = [str(build / "libfprint")]
    if args.deps:
        libraries.append(str(args.deps.resolve(strict=True) / "usr/lib/x86_64-linux-gnu"))
    env["LD_LIBRARY_PATH"] = ":".join(libraries)
    env["FP_DRIVERS_ALLOWLIST"] = "egis0575"
    os.umask(0o077)
    os.chdir(state)
    if args.command == "capture-series":
        return capture_series(binary, env)
    os.execve(binary, [str(binary), args.command], env)


if __name__ == "__main__":
    sys.exit(main())
