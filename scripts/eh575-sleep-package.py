#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare, never install, a separate opt-in automatic-suspend-wake add-on."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import tempfile

NAME = "eh575-touch-wake-experimental"
UNIT = "/usr/lib/systemd/system/eh575-detector-sleep.service"
HOOK = "/usr/lib/systemd/system-sleep/eh575-detector-sleep"
SERVICE = """# Opt-in only. Installation does not start or enable this service.
[Unit]
Description=Experimental EH575 detector for suspend touch wake
Requires=eh575-wakeup.service
After=eh575-wakeup.service

[Service]
Type=simple
ExecStart=/usr/libexec/eh575-sleep serve
RuntimeDirectory=eh575-detector-sleep
RuntimeDirectoryMode=0700
TimeoutStopSec=25
KillMode=control-group
ProtectHome=yes
PrivateTmp=yes
NoNewPrivileges=yes
ProtectSystem=full
RestrictAddressFamilies=AF_UNIX AF_NETLINK
UnsetEnvironment=LD_PRELOAD LD_LIBRARY_PATH PYTHONPATH DBUS_SYSTEM_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS

[Install]
WantedBy=multi-user.target
"""
SLEEP_HOOK = """#!/bin/sh
# Both stages finish before systemd advances to sleep/resume notifications.
# Never communicate with a frozen user service. Only our root supervisor.
if [ -S /run/eh575-detector-sleep/control ]; then
    /usr/libexec/eh575-sleep "$1" "$2" || :
fi
"""
README = """EXPERIMENTAL AUTOMATIC EH575 SUSPEND TOUCH WAKE

This optional add-on is separate from the working fingerprint matcher package.
It does not change libfprint, fprintd, GNOME, PAM, templates or passwords.
Installation neither starts nor enables its service. Contact is NOT a match.
It targets systemctl/menu/lid ordinary suspend through logind on the tested
revision-1072 sensor. Hibernation/hybrid/suspend-then-hibernate are not armed.
Awake blank-screen wake is not implemented by this add-on.

Explicit trial:
  sudo systemctl start eh575-detector-sleep.service
  systemctl status eh575-detector-sleep.service
Keep the sensor empty BEFORE suspending normally. Do not run manual probes.
Wait at least 20-30 seconds after actual sleep, then touch. Use keyboard/power
if it does not wake, and keep password fallback available. Verify the enrolled
finger matches after resume; also test non-enrolled-finger rejection. Inspect:
  journalctl -b -u eh575-detector-sleep.service -u systemd-suspend.service --no-pager
Repeat longer cycles, keyboard wake, pending verification and failed/empty
calibration cases before enabling at boot:
  sudo systemctl enable eh575-detector-sleep.service

Only after fprintd/logind sleep preparation, the pre hook requests a native
volatile measured detector, releases its interface claim, and retains its USB
handle. It sends NO USB traffic while waiting/asleep. The post hook requests
capture restoration, USB release/close and child exit before returning; normal
GNOME verification remains responsible for unlock. No authentication success,
input event, firmware/NVM write, USB reset, interface stealing, fprintd stop or
automatic suspend is performed. There is no RTC/wake timer or sleep inhibitor.
Sysfs wake permission remains owned by the existing eh575-wakeup service.

A busy reader, finger during calibration, unknown status, disconnect or timeout
skips touch wake and logs the failure. A missing hook or cancelled sleep recovers
after 120 AWAKE seconds. Native setup is bounded to 12 seconds and recovery to
5 seconds; supervisor waits are bounded and reaping is required for success.
Pathological kill/disconnect cannot guarantee restoration: use normal wake and
test fprintd before another suspend. An open handle may affect runtime power;
it is retained only for the sleep cycle, not all awake time.

The manual open-handle trial woke by touch and then normal fprintd matched.
That does NOT prove this automatic hook's ordering, reliability or live GNOME
password fallback. Those require owner-run physical trials of THIS package.
No images, prints, raw packets or USB serial numbers are logged or shipped.

Stop the trial: sudo systemctl disable --now eh575-detector-sleep.service
Remove ONLY this add-on: sudo apt remove eh575-touch-wake-experimental
The working matcher and stored prints remain. The separately opted-in USB wake
permission service is NOT stopped or disabled by add-on removal. Its own rollback
is sudo systemctl disable --now eh575-wakeup.service.
"""


def command(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, text=True,
                          capture_output=True, **kwargs).stdout.strip()


def elf_policy(text):
    required = ("libgusb.so.2", "libgio-2.0.so.0", "libglib-2.0.so.0", "libc.so.6")
    for name in required:
        if "Shared library: [" + name + "]" not in text:
            raise ValueError("Missing expected native dependency: " + name)
    for line in text.splitlines():
        if "(RPATH)" in line or "(RUNPATH)" in line:
            value = re.search(r"\[([^]]*)\]", line)
            if value is None or value.group(1):
                raise ValueError("Worker must not retain build search paths")
    if "libasan" in text or "libubsan" in text:
        raise ValueError("Do not deploy sanitizer instrumentation")


def write(root, path, value, executable=False):
    target = root / path.lstrip("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(value)
    target.chmod(0o755 if executable else 0o644)


def payload(root, native, repo, revision, version):
    target = root / "usr/libexec/eh575-sleep-worker"
    target.parent.mkdir(parents=True)
    shutil.copyfile(native, target)
    target.chmod(0o755)
    write(root, "/usr/libexec/eh575-sleep", (repo / "scripts/eh575-sleep.py").read_text(), True)
    write(root, UNIT, SERVICE)
    write(root, HOOK, SLEEP_HOOK, True)
    doc = "/usr/share/doc/" + NAME
    write(root, doc + "/README", README)
    write(root, doc + "/copyright", (repo / "COPYING").read_text())
    write(root, doc + "/build.json", json.dumps({
        "source_revision": revision, "native_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        "authentication_changed": False, "enabled_by_install": False,
        "supports_awake_screen_wake": False, "physical_automatic_trial_pending": True,
    }, indent=2) + "\n")
    write(root, "/DEBIAN/control", f"""Package: {NAME}
Version: {version}
Architecture: amd64
Maintainer: EH575 experimental contributors <ivanov-gv@users.noreply.github.com>
Section: admin
Priority: optional
Depends: libfprint-eh575-experimental (>= 0.2+git.2094), python3, systemd, libgusb2a (>= 0.4.9), libglib2.0-0t64 (>= 2.88.0), libc6 (>= 2.42)
Description: Opt-in experimental EH575 automatic suspend contact wake
 Separate root sleep-cycle detector; not an authentication service.
 No PAM/GNOME/template changes. Manual wake and password fallback required.
 Source revision: {revision}
""")
    write(root, "/DEBIAN/postinst", """#!/bin/sh
set -e
if [ "$1" = configure ]; then
    systemctl daemon-reload || true
fi
# Never start or enable the detector service on installation.
exit 0
""", True)
    write(root, "/DEBIAN/prerm", """#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = deconfigure ] || [ "$1" = upgrade ]; then
    systemctl stop eh575-detector-sleep.service || true
fi
if [ "$1" = remove ] || [ "$1" = deconfigure ]; then
    systemctl disable eh575-detector-sleep.service || true
fi
# Do not stop fprintd or the separate wake-permission service.
exit 0
""", True)
    write(root, "/DEBIAN/postrm", """#!/bin/sh
set -e
systemctl daemon-reload || true
exit 0
""", True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--deps", type=Path, help="optional extracted pkg-config sysroot; no runtime search path is embedded")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    if command(["git", "-C", repo, "status", "--porcelain"]):
        parser.error("Commit the source before producing a revision-bound package")
    if command(["dpkg", "--print-architecture"]) != "amd64":
        parser.error("Reviewed trial is Ubuntu amd64 only")
    revision = command(["git", "-C", repo, "rev-parse", "HEAD"])
    count = command(["git", "-C", repo, "rev-list", "--count", "HEAD"])
    version = "0.1+git." + count + "." + revision[:12]
    args.output.mkdir(parents=True, exist_ok=True)
    destination = args.output / (NAME + "_" + version + "_amd64.deb")
    if destination.exists():
        parser.error("Do not overwrite an existing revision package; choose another output directory")
    with tempfile.TemporaryDirectory(prefix="eh575-sleep-package-") as temporary:
        # Compile from the committed source ourselves, so the revision is not
        # falsely attributed to an arbitrary external/old executable. Meson's
        # local test build can contain a sysroot RUNPATH and is not deployed.
        native = Path(temporary) / "eh575-sleep-worker"
        env = dict(os.environ)
        for key in ("PKG_CONFIG_PATH", "PKG_CONFIG_SYSROOT_DIR", "PKG_CONFIG_LIBDIR"):
            env.pop(key, None)
        if args.deps:
            deps = args.deps.resolve(strict=True)
            env["PKG_CONFIG_SYSROOT_DIR"] = str(deps)
            env["PKG_CONFIG_LIBDIR"] = str(deps / "usr/lib/x86_64-linux-gnu/pkgconfig") + ":" + str(deps / "usr/share/pkgconfig")
        flags = shlex.split(command(["pkg-config", "--cflags", "--libs", "gusb"], env=env))
        command(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra", "-Werror", "-Wmissing-prototypes",
                 "-Wl,-z,relro,-z,now", "-I" + str(repo / "libfprint/drivers"),
                 repo / "tests/eh575-sleep-worker.c", "-o", native, *flags, "-lm"])
        elf_policy(command(["readelf", "-d", native]))
        root = Path(temporary) / "root"
        root.mkdir()
        payload(root, native, repo, revision, version)
        command(["dpkg-deb", "--root-owner-group", "--build", root, destination])
        native_hash = hashlib.sha256(native.read_bytes()).hexdigest()
    print("Prepared only; NOT installed or enabled: " + str(destination))
    print("Native SHA256: " + native_hash)


if __name__ == "__main__":
    main()
