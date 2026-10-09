#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Isolated Linux-only EH575 touch research; no system installation or authentication."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("open", "interrupt", "interrupt-initialized", "touch", "detector"))
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--deps", type=Path)
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Run without sudo, from an unlocked local terminal")
    build = args.build.resolve(strict=True)
    options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    if next(item["value"] for item in options if item["name"] == "drivers") != "egis0575":
        parser.error("Use the isolated EH575 build")
    binary = build / "tests/eh575-touch-probe"
    if not binary.is_file():
        parser.error("Compile the new eh575-touch-probe target first")
    status = subprocess.run(["/usr/bin/systemctl", "is-active", "--quiet", "fprintd.service"],
                            capture_output=True, check=False)
    if status.returncode == 0:
        parser.error("fprintd is active: unlock normally, close fingerprint clients and wait for its idle exit; do not stop authentication services")
    if status.returncode != 3:
        parser.error("Cannot confirm that system fprintd is inactive; refusing the probe")
    env = os.environ.copy()
    for key in ("LD_PRELOAD", "G_MESSAGES_DEBUG", "G_DEBUG", "LIBUSB_DEBUG", "GUSB_DEBUG"):
        env.pop(key, None)
    libraries = []
    if args.deps:
        deps = args.deps.resolve(strict=True)
        libraries.append(str(deps / "usr/lib/x86_64-linux-gnu"))
    env["LD_LIBRARY_PATH"] = ":".join(libraries)
    print("Close other fingerprint clients and keep this terminal visible. Do NOT suspend during the probe.", flush=True)
    os.execve(binary, [str(binary), args.mode], env)


if __name__ == "__main__":
    main()
