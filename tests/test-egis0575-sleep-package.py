#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Inspect a staged synthetic add-on only; never install or execute hooks."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "eh575_sleep_package", Path(__file__).resolve().parents[1] / "scripts/eh575-sleep-package.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PackageTests(unittest.TestCase):
    def test_payload_is_separate_disabled_and_no_auth_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            root = directory / "payload"
            repo = directory / "repo"
            root.mkdir()
            (repo / "scripts").mkdir(parents=True)
            (repo / "scripts/eh575-sleep.py").write_text("#!/usr/bin/python3 -I\n# synthetic fixture\n")
            (repo / "COPYING").write_text("synthetic LGPL fixture")
            native = directory / "native"
            native.write_bytes(b"synthetic native fixture")
            module.payload(root, native, repo, "0" * 40, "0.1+git.test")
            files = {str(p.relative_to(root)) for p in root.rglob("*") if p.is_file()}
            self.assertEqual(files, {
                "usr/libexec/eh575-sleep", "usr/libexec/eh575-sleep-worker",
                module.UNIT.lstrip("/"), module.HOOK.lstrip("/"),
                "usr/share/doc/eh575-touch-wake-experimental/README",
                "usr/share/doc/eh575-touch-wake-experimental/copyright",
                "usr/share/doc/eh575-touch-wake-experimental/build.json",
                "DEBIAN/control", "DEBIAN/postinst", "DEBIAN/prerm", "DEBIAN/postrm"})
            self.assertIn("Requires=eh575-wakeup.service", module.SERVICE)
            self.assertIn("RuntimeDirectoryMode=0700", module.SERVICE)
            self.assertIn("KillMode=control-group", module.SERVICE)
            self.assertIn("WantedBy=multi-user.target", module.SERVICE)
            for script in ("postinst", "prerm", "postrm"):
                text = (root / "DEBIAN" / script).read_text()
                for command in ("systemctl enable", "systemctl restart", "systemctl try-restart",
                                "pam-auth-update", "systemctl stop fprintd", "systemctl stop eh575-wakeup"):
                    self.assertNotIn(command, text)
            self.assertIn("stop eh575-detector-sleep.service", (root / "DEBIAN/prerm").read_text())
            for path in ("usr/libexec/eh575-sleep", "usr/libexec/eh575-sleep-worker", module.HOOK.lstrip("/")):
                self.assertEqual((root / path).stat().st_mode & 0o777, 0o755)
            self.assertNotIn("systemctl", module.SLEEP_HOOK)
            self.assertNotIn("hibernate\n", module.SLEEP_HOOK)

    def test_elf_relocation_and_runtime_dependencies(self):
        text = "\n".join("Shared library: [" + name + "]" for name in
                         ("libgusb.so.2", "libgio-2.0.so.0", "libglib-2.0.so.0", "libc.so.6"))
        module.elf_policy(text)
        for unsafe in ("(RUNPATH) [/tmp/unsafe]", "(RPATH) [$ORIGIN]", "libasan", "libubsan"):
            with self.assertRaises(ValueError):
                module.elf_policy(text + "\n" + unsafe)
        with self.assertRaises(ValueError):
            module.elf_policy(text.replace("libgusb.so.2", "libgusb.so.3"))


if __name__ == "__main__":
    unittest.main()
