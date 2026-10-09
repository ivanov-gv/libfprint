#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Mocked runner guards; never discover or communicate with a real reader."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "touch_runner", Path(__file__).resolve().parents[1] / "scripts/eh575-touch.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerPolicy(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        (self.build / "meson-info").mkdir()
        (self.build / "meson-info/intro-buildoptions.json").write_text(
            json.dumps([{"name": "drivers", "value": "egis0575"}]))
        (self.build / "tests").mkdir()
        (self.build / "tests/eh575-touch-probe").touch()

    def invoke(self, mode="touch", uid=1000, status=3):
        with patch.object(sys, "argv", ["eh575-touch.py", mode, "--build", str(self.build)]), \
                patch.object(runner.os, "geteuid", return_value=uid), \
                patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], status)), \
                patch.object(sys, "stderr", new_callable=io.StringIO), \
                patch.object(runner.os, "execve") as execute:
            try:
                runner.main()
                return None, execute.call_args
            except SystemExit as error:
                self.assertIsNone(execute.call_args)
                return error.code, None

    def test_root_active_service_and_bus_failure_rejected(self):
        for arguments in ({"uid": 0}, {"status": 0}, {"status": 1}, {"status": 4}):
            self.assertEqual(self.invoke(**arguments)[0], 2)

    def test_wrong_build_and_missing_binary_rejected(self):
        (self.build / "meson-info/intro-buildoptions.json").write_text(
            json.dumps([{"name": "drivers", "value": "all"}]))
        self.assertEqual(self.invoke()[0], 2)
        (self.build / "meson-info/intro-buildoptions.json").write_text(
            json.dumps([{"name": "drivers", "value": "egis0575"}]))
        (self.build / "tests/eh575-touch-probe").unlink()
        self.assertEqual(self.invoke()[0], 2)

    def test_exact_modes_and_clean_environment(self):
        with patch.dict(runner.os.environ, {"LD_PRELOAD": "untrusted", "LD_LIBRARY_PATH": "untrusted", "LIBUSB_DEBUG": "4"}):
            for mode in ("open", "interrupt", "interrupt-initialized", "touch", "detector"):
                code, call = self.invoke(mode=mode)
                self.assertIsNone(code)
                self.assertEqual(call.args[1], [str(self.build / "tests/eh575-touch-probe"), mode])
                self.assertNotIn("LD_PRELOAD", call.args[2])
                self.assertNotIn("LIBUSB_DEBUG", call.args[2])
                self.assertEqual(call.args[2]["LD_LIBRARY_PATH"], "")
        self.assertEqual({item.name for item in self.build.iterdir()}, {"meson-info", "tests"})


if __name__ == "__main__":
    unittest.main()
