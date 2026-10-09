#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic sysfs tests only: never change the host's wake or sleep state."""
import importlib.util
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "eh575_wakeup", Path(__file__).resolve().parents[1] / "scripts/eh575-wakeup.py")
wake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wake)


class WakePolicy(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sys = self.root / "sys"
        self.bus = self.sys / "bus/usb/devices"
        self.bus.mkdir(parents=True)
        self.controller = self.sys / "devices/pci/0000:03:00.4"
        self.hub = self.controller / "usb3"
        self.reader = self.hub / "3-4"
        self.other = self.hub / "3-5"
        self.state = self.root / "state"
        for node, value in ((self.controller, "enabled"), (self.hub, "disabled"),
                            (self.reader, "disabled"), (self.other, "disabled")):
            (node / "power").mkdir(parents=True)
            (node / "power/wakeup").write_text(value + "\n")
        for node, ids in ((self.hub, ("1d6b", "0002", "0700")),
                          (self.reader, wake.IDENTITY),
                          (self.other, ("abcd", "1234", "0001"))):
            for key, value in zip(("idVendor", "idProduct", "bcdDevice"), ids):
                (node / key).write_text(value + "\n")
            (self.bus / node.name).symlink_to(node)
        (self.hub / "bDeviceClass").write_text("09\n")

    def value(self, node):
        return wake.read(node / "power/wakeup")

    def enable(self):
        wake.enable(self.sys, self.state)

    def test_only_reader_and_its_hub_are_changed_and_restored(self):
        self.enable()
        self.assertEqual(self.value(self.reader), "enabled")
        self.assertEqual(self.value(self.hub), "enabled")
        self.assertEqual(self.value(self.other), "disabled")
        self.assertEqual(self.value(self.controller), "enabled")
        self.assertEqual(self.state.stat().st_mode & 0o777, 0o700)
        self.assertEqual((self.state / "state.json").stat().st_mode & 0o777, 0o600)
        wake.disable(self.sys, self.state)
        self.assertEqual(self.value(self.reader), "disabled")
        self.assertEqual(self.value(self.hub), "disabled")
        self.assertFalse((self.state / "state.json").exists())

    def test_reapply_after_libfprint_policy_reset_retains_originals(self):
        (self.hub / "power/wakeup").write_text("enabled\n")
        self.enable()
        saved = (self.state / "state.json").read_bytes()
        (self.reader / "power/wakeup").write_text("disabled\n")
        wake.enable(self.sys, self.state, reapply=True)
        self.assertEqual(self.value(self.reader), "enabled")
        self.assertEqual((self.state / "state.json").read_bytes(), saved)
        wake.disable(self.sys, self.state)
        self.assertEqual(self.value(self.hub), "enabled")

    def test_hook_without_opt_in_does_not_enable(self):
        wake.enable(self.sys, self.state, reapply=True)
        self.assertEqual(self.value(self.reader), "disabled")
        self.assertFalse((self.state / "state.json").exists())

    def test_sleep_snapshots_verify_pre_and_never_rearm_post(self):
        (self.sys / "power").mkdir()
        (self.sys / "power/mem_sleep").write_text("[s2idle]\n")
        (self.sys / "power/pm_wakeup_irq").write_text("7\n")
        (self.reader / "power/runtime_status").write_text("suspended\n")
        self.enable()
        original = (self.state / "state.json").read_bytes()
        (self.reader / "power/wakeup").write_text("disabled\n")
        output = io.StringIO()
        with redirect_stdout(output):
            wake.sleep_observation("pre", self.sys, self.state)
        snapshot = json.loads(output.getvalue().split(": ", 1)[1])
        self.assertEqual(snapshot["phase"], "pre")
        self.assertTrue(all(item["wakeup"] == "enabled" for item in snapshot["devices"]))
        self.assertEqual(snapshot["devices"][0]["runtime_status"], "suspended")
        self.assertEqual(snapshot["mem_sleep"], "[s2idle]")
        self.assertEqual(snapshot["last_wakeup_irq"], "7")
        self.assertEqual(snapshot["platform"], [{"node": self.controller.name, "wakeup": "enabled"}])
        self.assertEqual((self.state / "state.json").read_bytes(), original)
        (self.reader / "power/wakeup").write_text("disabled\n")
        output = io.StringIO()
        with redirect_stdout(output):
            wake.sleep_observation("post", self.sys, self.state)
        snapshot = json.loads(output.getvalue().split(": ", 1)[1])
        self.assertEqual(snapshot["phase"], "post")
        self.assertEqual(snapshot["devices"][0]["wakeup"], "disabled")
        self.assertEqual(self.value(self.reader), "disabled")
        self.assertEqual((self.state / "state.json").read_bytes(), original)
        self.assertNotIn("serial", output.getvalue())

    def test_sleep_snapshot_without_active_opt_in_does_nothing(self):
        for phase in ("pre", "post"):
            output = io.StringIO()
            with redirect_stdout(output):
                wake.sleep_observation(phase, self.sys, self.state)
            self.assertEqual(output.getvalue(), "")
            self.assertEqual(self.value(self.reader), "disabled")
        self.enable()
        wake.save(self.state, wake.load(self.state)[0], active=False)
        with redirect_stdout(io.StringIO()) as output:
            wake.sleep_observation("pre", self.sys, self.state)
        self.assertEqual(output.getvalue(), "")
        with self.assertRaises(ValueError):
            wake.sleep_observation("invalid", self.sys, self.state)

    def test_sleep_snapshot_does_not_touch_usb_on_missing_platform_permission(self):
        self.enable()
        (self.reader / "power/wakeup").write_text("disabled\n")
        (self.controller / "power/wakeup").write_text("disabled\n")
        with redirect_stdout(io.StringIO()) as output, self.assertRaises(ValueError):
            wake.sleep_observation("pre", self.sys, self.state)
        self.assertEqual(output.getvalue(), "")
        self.assertEqual(self.value(self.reader), "disabled")

    def test_empty_or_malformed_metadata_is_unavailable_not_zero(self):
        (self.reader / "power/wakeup_count").write_text("\n")
        (self.reader / "power/control").write_text("auto\nextra\n")
        (self.reader / "power/persist").write_text("x" * 129)
        (self.reader / "serial").write_text("private synthetic serial")
        snapshot = wake.observation(self.sys)
        reader = snapshot["devices"][0]
        self.assertIsNone(reader["wakeup_count"])
        self.assertIsNone(reader["control"])
        self.assertIsNone(reader["persist"])
        self.assertIsNone(snapshot["last_wakeup_irq"])
        self.assertNotIn("private synthetic serial", json.dumps(snapshot))

    def test_revision_mismatch_does_not_change_wake(self):
        (self.reader / "bcdDevice").write_text("9999\n")
        with self.assertRaises(ValueError):
            self.enable()
        self.assertEqual(self.value(self.reader), "disabled")
        self.assertFalse((self.state / "state.json").exists())

    def test_platform_disabled_is_not_overridden(self):
        (self.controller / "power/wakeup").write_text("disabled\n")
        with self.assertRaisesRegex(ValueError, "Platform wake"):
            self.enable()
        self.assertEqual(self.value(self.controller), "disabled")
        self.assertEqual(self.value(self.reader), "disabled")

    def test_empty_wakeup_rejected_before_any_write(self):
        (self.hub / "power/wakeup").write_text("\n")
        with self.assertRaises(ValueError):
            self.enable()
        self.assertEqual(self.value(self.reader), "disabled")

    def test_missing_wakeup_fails_instead_of_partial_enable(self):
        (self.reader / "power/wakeup").unlink()
        with self.assertRaises(OSError):
            self.enable()
        self.assertEqual(self.value(self.hub), "disabled")
        self.assertFalse((self.state / "state.json").exists())

    def test_concurrent_mutation_fails_without_changing_wake(self):
        with wake.locked(self.state), self.assertRaises(BlockingIOError):
            self.enable()
        self.assertEqual(self.value(self.reader), "disabled")

    def test_unsafe_lock_rejected(self):
        self.state.mkdir(mode=0o700)
        (self.state / "lock").write_text("unsafe fixture")
        (self.state / "lock").chmod(0o644)
        with self.assertRaises(ValueError):
            self.enable()
        self.assertEqual(self.value(self.reader), "disabled")

    def test_write_failure_restores_partial_changes(self):
        writer = wake.write_wakeup
        failed = False

        def fail_once(node, value):
            nonlocal failed
            if node == self.hub and value == "enabled" and not failed:
                failed = True
                raise OSError("synthetic sysfs failure")
            writer(node, value)

        with patch.object(wake, "write_wakeup", fail_once), self.assertRaises(OSError):
            self.enable()
        self.assertEqual(self.value(self.reader), "disabled")
        self.assertEqual(self.value(self.hub), "disabled")
        self.assertFalse((self.state / "state.json").exists())

    def test_saved_path_cannot_choose_write_destination(self):
        self.enable()
        unrelated = self.other / "power/wakeup"
        snapshot = json.loads((self.state / "state.json").read_text())
        snapshot["original"][str(self.other)] = "enabled"
        (self.state / "state.json").write_text(json.dumps(snapshot))
        with self.assertRaisesRegex(ValueError, "Old USB path"):
            wake.disable(self.sys, self.state)
        self.assertEqual(wake.read(unrelated), "disabled")

    def test_missing_reader_does_not_restore_arbitrary_paths(self):
        self.enable()
        (self.reader / "bcdDevice").write_text("9999\n")
        with self.assertRaisesRegex(ValueError, "Cannot safely restore"):
            wake.disable(self.sys, self.state)
        self.assertTrue((self.state / "state.json").exists())
        self.assertFalse(json.loads((self.state / "state.json").read_text())["active"])
        # A failed stop must not arm wake again on the next sleep hook.
        wake.enable(self.sys, self.state, reapply=True)

    def test_symlinked_state_directory_rejected(self):
        target = self.root / "unsafe"
        target.mkdir(mode=0o700)
        self.state.symlink_to(target)
        with self.assertRaises(ValueError):
            self.enable()

    def test_unsafe_state_file_rejected(self):
        self.enable()
        path = self.state / "state.json"
        saved = path.read_text()
        path.chmod(0o644)
        with self.assertRaises(ValueError):
            wake.enable(self.sys, self.state, reapply=True)
        path.unlink()
        outside = self.root / "outside"
        outside.write_text(saved)
        outside.chmod(0o600)
        path.symlink_to(outside)
        with self.assertRaises(OSError):
            wake.enable(self.sys, self.state, reapply=True)

    def test_non_hub_parent_rejected(self):
        (self.hub / "bDeviceClass").write_text("ff\n")
        with self.assertRaises(ValueError):
            self.enable()
        self.assertEqual(self.value(self.reader), "disabled")


if __name__ == "__main__":
    unittest.main()
