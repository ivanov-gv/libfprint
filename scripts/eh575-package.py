#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Prepare (never install) a reversible Ubuntu 26.04 amd64 EH575 trial package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

PACKAGE = "libfprint-eh575-experimental"
PREFIX = Path("/opt/eh575-libfprint")
DROPIN = Path("/usr/lib/systemd/system/fprintd.service.d/60-eh575-native.conf")
MODULES = ("core", "imgproc", "features2d", "calib3d", "video")
README = """EXPERIMENTAL EH575 GNOME TRIAL -- NOT A CERTIFIED BIOMETRIC MATCHER

This package changes fprintd's native library selection and enables a protected
empty-reader calibration cache. It does not replace
Ubuntu's libfprint package, daemon, D-Bus/PolicyKit policies, or PAM configuration.
Only the native library, licenses/docs, service drop-in and empty cache directory ship.
No Python matcher, test authorization fixture, template or capture ships.

Install ONLY after private lifecycle tests and explicit approval. Review apt's
transaction; do not accept removals or unexpected upgrades. Keep password unlock
and a known-working terminal/TTY available. Do NOT run pam-auth-update or enable
fingerprint authentication in common-auth/sudo as part of this trial.

The real system daemon will then use real system PolicyKit and normal GNOME/PAM.
Create a NEW system enrollment with fprintd-enroll -f right-index-finger or GNOME
Settings. Existing PRIVATE test enrollments are never copied automatically.
Keep the sensor empty during initial calibration; use 15 stationary touches with
three each at center/tip/base/left/right, moving only between touches.
After a successful empty-start scan, a compatible cache permits early touch-and-hold.
It stores only an empty-sensor reference and DC metadata, not a finger/template,
in /var/lib/eh575-libfprint/calibration (0700 directory, 0600 files). Boot, physical
suspend, device changes and one-hour expiry invalidate reuse. Cold early contact
requests remove-and-retry: lift briefly, then touch again in the same client request.
Acquisition quality and matching thresholds are unchanged; test the update afresh.
Test fprintd-verify with enrolled and non-enrolled fingers BEFORE locking.

Rollback: sudo apt remove libfprint-eh575-experimental
The drop-in and /opt library are removed; fprintd is reloaded/restarted to select
Ubuntu's original library again. No GDM restart, reboot or PAM edit is needed.
Stored system prints are biometric data and are deliberately NOT erased by removal.
Private test prints and system package libfprint files are never touched.
Runtime calibration cache files may remain after removal; Ubuntu's library does
not use them. They are not an enrollment and cannot be used to import a print.
Do not run apt autoremove blindly; review any optional dependency cleanup.

GNOME/GDM fingerprint use may include both unlock AND login. This package does
not implement a lock-screen-only policy, liveness detection or security review.
Small wrong-finger audits do not establish population false-accept risk.
"""


def command(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, text=True,
                          capture_output=True, **kwargs).stdout.strip()


def elf_policy(text):
    if "Library soname: [libfprint-2.so.2]" not in text:
        raise ValueError("Expected the native libfprint SONAME")
    for line in text.splitlines():
        if "(RPATH)" in line or "(RUNPATH)" in line:
            value = re.search(r"\[([^]]*)\]", line)
            if value is None or value.group(1):
                raise ValueError("Deployment library must not retain build/runtime search paths")
    for module in MODULES:
        if f"Shared library: [libopencv_{module}.so.410]" not in text:
            raise ValueError("This trial package requires the reviewed OpenCV 4.10 runtime ABI")
    if "libasan" in text or "libubsan" in text:
        raise ValueError("Do not deploy a sanitizer-instrumented library")


def version_for_commit(revision, count):
    # Hash-only versions can sort backwards and make an update a downgrade.
    # Descendant checkpoints on this development branch have increasing counts.
    count = int(count)
    if count <= 0 or re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", revision) is None:
        raise ValueError("Expected a full Git revision and positive commit count")
    return "0.2+git." + str(count) + "." + revision[:12]


def control(version, architecture, revision):
    return f"""Package: {PACKAGE}
Version: {version}
Architecture: {architecture}
Maintainer: EH575 experimental contributors <ivanov-gv@users.noreply.github.com>
Section: admin
Priority: optional
Depends: fprintd (>= 1.94.5), libpam-fprintd, libglib2.0-0t64 (>= 2.88.0), libgusb2a (>= 0.4.9), libc6 (>= 2.42), libstdc++6 (>= 15), libopencv-core410, libopencv-imgproc410, libopencv-features2d410, libopencv-calib3d410, libopencv-video410
Homepage: https://github.com/ivanov-gv/libfprint
Description: EXPERIMENTAL stationary EH575 native fprintd trial
 Root-owned service-scoped libfprint override for Ubuntu 26.04 amd64.
 No PAM modifications, bundled biometric data or test authorization fixture.
 Not security-certified; password fallback and explicit trial consent required.
 Remove this package to restore the distribution fprintd library selection.
 Source revision: {revision}
"""


def write(root, path, content, executable=False):
    target = root / str(path).lstrip("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content)
    target.chmod(0o755 if executable else 0o644)


def payload(root, library, repo, revision, version):
    directory = root / PREFIX.relative_to("/") / "lib"
    directory.mkdir(parents=True)
    target = directory / "libfprint-2.so.2.0.0"
    shutil.copyfile(library, target)
    target.chmod(0o644)
    (directory / "libfprint-2.so.2").symlink_to(target.name)
    # Separate from fprintd's print store: its backend treats top-level entries
    # as usernames. Do not change StateDirectory or STATE_DIRECTORY.
    (root / "var/lib/eh575-libfprint/calibration").mkdir(parents=True, mode=0o700)
    write(root, DROPIN, """# Managed by libfprint-eh575-experimental; remove the package to revert.
[Service]
Environment="LD_LIBRARY_PATH=/opt/eh575-libfprint/lib"
Environment="FP_DRIVERS_ALLOWLIST=egis0575"
Environment="G_MESSAGES_DEBUG="
Environment="FP_EH575_CALIBRATION_DIR=/var/lib/eh575-libfprint/calibration"
ReadWritePaths=/var/lib/eh575-libfprint/calibration
UnsetEnvironment=LD_PRELOAD DBUS_SYSTEM_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS
""")
    doc = Path("/usr/share/doc") / PACKAGE
    write(root, doc / "README", README)
    write(root, doc / "copyright", (repo / "COPYING").read_text() + "\n\n" +
          (repo / "libfprint/drivers/egis0575-prototype-MIT.txt").read_text())
    write(root, doc / "build.json", json.dumps({
        "source_revision": revision, "version": version,
        "library_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        "template_schema": "eh575-ridge-v1", "security_certified": False,
        "system_pam_modified": False, "biometric_data_included": False,
        "calibration_cache": "empty-reader-v1; same boot/suspend/device; max age 1 hour",
    }, indent=2) + "\n")
    (root / "DEBIAN").mkdir(mode=0o755)
    write(root, "DEBIAN/control", control(version, "amd64", revision))
    # Runtime maintainer scripts, NOT run by this builder. dpkg executes them
    # only after a human-authorized installation/removal of this named package.
    write(root, "DEBIAN/postinst", """#!/bin/sh
set -eu
unset DBUS_SYSTEM_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS LD_LIBRARY_PATH LD_PRELOAD
if [ "$1" = configure ]; then
    /usr/bin/systemctl daemon-reload
    /usr/bin/systemctl try-restart fprintd.service
fi
""", True)
    write(root, "DEBIAN/postrm", """#!/bin/sh
set -eu
unset DBUS_SYSTEM_BUS_ADDRESS DBUS_SESSION_BUS_ADDRESS LD_LIBRARY_PATH LD_PRELOAD
case "$1" in
    remove|purge|abort-install)
        /usr/bin/systemctl daemon-reload
        /usr/bin/systemctl try-restart fprintd.service
        ;;
esac
""", True)


def main():
    os.umask(0o022)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New output directory; existing files are not overwritten")
    parser.add_argument("--meson", type=Path, help="Meson entry script for the current build environment")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Build without sudo; installation is a separate approved action")
    repo = Path(__file__).resolve().parents[1]
    try:
        release = Path("/etc/os-release").read_text()
        if 'ID=ubuntu' not in release or 'VERSION_ID="26.04"' not in release:
            raise ValueError("This prepared deployment is limited to the tested Ubuntu 26.04 laptop")
        if command(["dpkg", "--print-architecture"]) != "amd64":
            raise ValueError("This trial package is amd64-only")
        if command(["git", "-C", repo, "status", "--porcelain"]):
            raise ValueError("Commit the source checkpoint before preparing a deployment package")
        revision = command(["git", "-C", repo, "rev-parse", "HEAD"])
        version = version_for_commit(revision, command(["git", "-C", repo, "rev-list", "--count", "HEAD"]))
        build = args.build.resolve(strict=True)
        info = json.loads((build / "meson-info/meson-info.json").read_text())
        if Path(info["directories"]["source"]).resolve() != repo:
            raise ValueError("The build must come from this source checkout")
        options = {i["name"]: i["value"] for i in json.loads(
            (build / "meson-info/intro-buildoptions.json").read_text())}
        if options.get("drivers") != "egis0575" or options.get("egis0575_ridge") is not True:
            raise ValueError("Use the isolated stationary ridge build")
        meson = [sys.executable, str(args.meson)] if args.meson else ["meson"]
        command(meson + ["compile", "-C", str(build)])
        library = (build / "libfprint/libfprint-2.so.2.0.0").resolve(strict=True)
        plan = json.loads((build / "meson-info/intro-installed.json").read_text())
        install_path = Path(plan[str(library)])
        if not install_path.is_absolute():
            raise ValueError("Meson must provide an absolute staged library install path")
        output = args.output.absolute()
        if any(p.is_symlink() for p in (output, *output.parents)):
            raise ValueError("Output paths must not use symlinks")
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
        with tempfile.TemporaryDirectory(prefix="eh575-package-") as temporary:
            staging = Path(temporary) / "meson-stage"
            command(meson + ["install", "-C", str(build), "--no-rebuild",
                             "--tags", "runtime", "--destdir", str(staging)])
            installed = staging / install_path.relative_to("/")
            elf_policy(command(["readelf", "-d", installed]))
            root = Path(temporary) / "package"
            root.mkdir(mode=0o755)
            payload(root, installed, repo, revision, version)
            # Archive must contain only the deliberate root-owned trial payload.
            archive = output / f"{PACKAGE}_{version}_amd64.deb"
            command(["dpkg-deb", "--root-owner-group", "--build", root, archive])
            archive.chmod(0o644)
            manifest = command(["dpkg-deb", "--contents", archive])
            (output / "contents.txt").write_text(manifest + "\n")
            (output / "sha256.txt").write_text(hashlib.sha256(archive.read_bytes()).hexdigest() +
                                              "  " + archive.name + "\n")
        print("Prepared, NOT INSTALLED: " + str(archive))
        print("No system daemon, PAM, GNOME, dependencies or enrollment changed.")
        print("Review contents and apt's simulated transaction before approving installation.")
        print("Rollback after installation: sudo apt remove " + PACKAGE)
        return 0
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        print("Package preparation failed: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
