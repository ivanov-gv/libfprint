#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Isolated Linux-only EH575 touch research; no system installation or authentication."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess


def wake_ready():
    """Metadata-only preflight; never changes power permission or sends USB."""
    spec = importlib.util.spec_from_file_location("wake_permission", Path(__file__).with_name("eh575-wakeup.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    readers, nodes = module.targets(check_platform=True)
    if len(readers) != 1 or any(value != "enabled" for value in nodes.values()):
        raise ValueError("Require one tested reader and enabled reader/hub/platform wake permission")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("open", "interrupt", "interrupt-initialized", "touch", "detector", "detector-contact", "detector-suspend", "detector-suspend-released", "detector-suspend-unclaimed"))
    parser.add_argument("--allow-suspend-test", action="store_true", help="explicitly permit a manual detector-suspend experiment")
    parser.add_argument("--allow-contact-test", action="store_true", help="explicitly permit the awake one-shot contact handoff experiment")
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--deps", type=Path)
    args = parser.parse_args()
    suspend_test = args.mode in ("detector-suspend", "detector-suspend-released", "detector-suspend-unclaimed")
    contact_test = args.mode == "detector-contact"
    if args.allow_suspend_test != suspend_test:
        parser.error("Suspend modes require --allow-suspend-test; do not use that flag with awake modes")
    if args.allow_contact_test != contact_test:
        parser.error("detector-contact requires --allow-contact-test; do not use that flag with other modes")
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
    if suspend_test:
        wake = subprocess.run(["/usr/bin/systemctl", "is-active", "--quiet", "eh575-wakeup.service"],
                              capture_output=True, check=False)
        if wake.returncode != 0:
            parser.error("Start the already-installed opt-in eh575-wakeup.service first; the probe never changes wake policy")
        try:
            wake_ready()
        except (OSError, ValueError) as error:
            parser.error(str(error))
    env = os.environ.copy()
    for key in ("LD_PRELOAD", "G_MESSAGES_DEBUG", "G_DEBUG", "LIBUSB_DEBUG", "GUSB_DEBUG"):
        env.pop(key, None)
    libraries = []
    if args.deps:
        deps = args.deps.resolve(strict=True)
        libraries.append(str(deps / "usr/lib/x86_64-linux-gnu"))
    env["LD_LIBRARY_PATH"] = ":".join(libraries)
    print("Close other fingerprint clients and keep this terminal visible. " +
          ("Suspend ONLY after SUSPEND TEST READY." if suspend_test else "Do NOT suspend during the probe."), flush=True)
    command = [str(binary), args.mode] + (["--allow-suspend-test"] if suspend_test else ["--allow-contact-test"] if contact_test else [])
    os.execve(binary, command, env)


if __name__ == "__main__":
    main()
