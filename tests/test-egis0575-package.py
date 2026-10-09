#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Package payload and ELF guards only; never install or run maintainer scripts."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import subprocess

spec = importlib.util.spec_from_file_location(
    "eh575_package", Path(__file__).resolve().parents[1] / "scripts/eh575-package.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

class PackagePolicy(unittest.TestCase):
    def test_descendant_versions_increase_even_when_hashes_do_not(self):
        old = package.version_for_commit("f" * 40, 2086)
        new = package.version_for_commit("0" * 40, 2087)
        for first, second in (("0.1+git.4ab3b17e0b90", old), (old, new)):
            self.assertEqual(subprocess.run(["dpkg", "--compare-versions", first, "lt", second]).returncode, 0)
        for revision, count in (("invalid", 2086), ("0" * 40, 0), ("0" * 40, -1)):
            with self.assertRaises(ValueError):
                package.version_for_commit(revision, count)

    def dynamic(self):
        return "Library soname: [libfprint-2.so.2]\n" + "\n".join(
            f"Shared library: [libopencv_{name}.so.410]" for name in package.MODULES)

    def test_staged_library_must_be_relocatable(self):
        package.elf_policy(self.dynamic())
        for unsafe in ("Library runpath: [/tmp/user-library]", "Library rpath: [$ORIGIN]",
                       "Library runpath: [/home/user/lib]"):
            tag = "(RPATH)" if " rpath:" in unsafe else "(RUNPATH)"
            with self.assertRaises(ValueError):
                package.elf_policy(self.dynamic() + "\n" + tag + " " + unsafe)

    def test_other_abi_and_sanitizers_rejected(self):
        with self.assertRaises(ValueError):
            package.elf_policy(self.dynamic().replace(".so.410", ".so.406"))
        with self.assertRaises(ValueError):
            package.elf_policy(self.dynamic() + "\nShared library: [libasan.so.8]")

    def test_small_payload_does_not_bundle_test_authorization(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo = root / "repo"
            (repo / "libfprint/drivers").mkdir(parents=True)
            (repo / "COPYING").write_text("synthetic LGPL test fixture")
            (repo / "libfprint/drivers/egis0575-prototype-MIT.txt").write_text("synthetic MIT test fixture")
            (repo / "scripts").mkdir()
            (repo / "scripts/eh575-wakeup.py").write_text("#!/usr/bin/python3 -I\n# synthetic helper fixture\n")
            library = root / "library"
            library.write_bytes(b"synthetic ELF fixture, not biometric")
            payload = root / "payload"
            payload.mkdir()
            package.payload(payload, library, repo, "0" * 40, "0.1+git.test")
            files = {str(p.relative_to(payload)) for p in payload.rglob("*") if p.is_file() or p.is_symlink()}
            self.assertEqual(files, {
                "opt/eh575-libfprint/lib/libfprint-2.so.2.0.0",
                "opt/eh575-libfprint/lib/libfprint-2.so.2",
                "usr/lib/systemd/system/fprintd.service.d/60-eh575-native.conf",
                "usr/libexec/eh575-wakeup",
                "usr/lib/systemd/system/eh575-wakeup.service",
                "usr/lib/systemd/system-sleep/eh575-wakeup",
                "usr/share/doc/libfprint-eh575-experimental/README",
                "usr/share/doc/libfprint-eh575-experimental/copyright",
                "usr/share/doc/libfprint-eh575-experimental/build.json",
                "DEBIAN/control", "DEBIAN/postinst", "DEBIAN/prerm", "DEBIAN/postrm",
            })
            dropin = (payload / package.DROPIN.relative_to("/")).read_text()
            self.assertIn("/opt/eh575-libfprint/lib", dropin)
            self.assertNotIn("/tmp", dropin)
            self.assertNotIn("StateDirectory=", dropin)
            self.assertNotIn("STATE_DIRECTORY=", dropin)
            self.assertIn("FP_EH575_CALIBRATION_DIR=/var/lib/eh575-libfprint/calibration", dropin)
            self.assertIn("ReadWritePaths=/var/lib/eh575-libfprint/calibration", dropin)
            cache = payload / "var/lib/eh575-libfprint/calibration"
            self.assertEqual(cache.stat().st_mode & 0o777, 0o700)
            self.assertEqual(list(cache.iterdir()), [])
            service = (payload / package.WAKE_SERVICE.relative_to("/")).read_text()
            self.assertIn("ExecStop=/usr/libexec/eh575-wakeup disable", service)
            self.assertIn("RuntimeDirectoryMode=0700", service)
            hook = (payload / package.WAKE_HOOK.relative_to("/")).read_text()
            self.assertIn('[ "$1" = pre ]', hook)
            self.assertIn('/run/eh575-wakeup/state.json', hook)
            self.assertIn('eh575-wakeup reapply', hook)
            self.assertNotIn("systemctl enable", (payload / "DEBIAN/postinst").read_text())
            self.assertIn("disable --now eh575-wakeup.service", (payload / "DEBIAN/prerm").read_text())
            for path in (package.WAKE_HELPER, package.WAKE_HOOK):
                self.assertEqual((payload / path.relative_to("/")).stat().st_mode & 0o777, 0o755)
            for script in ("postinst", "postrm"):
                text = (payload / "DEBIAN" / script).read_text()
                self.assertNotIn("pam-auth-update", text)
                self.assertNotIn("gdm", text)
                self.assertNotIn("rm ", text)
                self.assertIn("try-restart fprintd.service", text)
                self.assertEqual((payload / "DEBIAN" / script).stat().st_mode & 0o777, 0o755)

if __name__ == "__main__":
    unittest.main()
