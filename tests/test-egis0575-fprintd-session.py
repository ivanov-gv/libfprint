#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Private-bus guards and fixtures only: no USB or biometric recordings."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

from gi.repository import GLib

spec = importlib.util.spec_from_file_location(
    "eh575_session", Path(__file__).resolve().parents[1] / "scripts/eh575-fprintd-session.py")
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)


class Guards(unittest.TestCase):
    def test_build_requires_ridge(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "meson-info").mkdir()
            options = [{"name": "drivers", "value": "egis0575"},
                       {"name": "egis0575_ridge", "value": False}]
            (root / "meson-info/intro-buildoptions.json").write_text(json.dumps(options))
            with self.assertRaises(ValueError):
                session.validate_build(root, None)

    def test_unsafe_private_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "private"
            target.mkdir(mode=0o700)
            link = root / "link"
            link.symlink_to(target, target_is_directory=True)
            with self.assertRaises(ValueError):
                session.private_directory(link)
            (target / "public").write_text("not biometric")
            (target / "public").chmod(0o644)
            with self.assertRaises(ValueError):
                session.private_directory(target)
            with self.assertRaises(ValueError):
                session.private_directory(root / "colon:rejected")

    def test_inherited_routes_are_removed(self):
        old = os.environ.copy()
        try:
            os.environ.update(DBUS_SYSTEM_BUS_ADDRESS="unix:path=/host",
                              STATE_DIRECTORY="/host-storage", LD_PRELOAD="host.so",
                              FP_VIRTUAL_DEVICE="host", G_MESSAGES_DEBUG="all")
            env = session.client_env("unix:path=/owned-test")
            self.assertEqual(env["DBUS_SYSTEM_BUS_ADDRESS"], "unix:path=/owned-test")
            self.assertEqual(env["DBUS_SESSION_BUS_ADDRESS"], "unix:path=/owned-test")
            for key in ("STATE_DIRECTORY", "LD_PRELOAD", "FP_VIRTUAL_DEVICE", "G_MESSAGES_DEBUG"):
                self.assertNotIn(key, env)
        finally:
            os.environ.clear()
            os.environ.update(old)


class PrivateServices(unittest.TestCase):
    def setUp(self):
        self.bus = session.PrivateBus()
        self.fixture = session.Fixtures(self.bus)
        self.connection = session.connect(self.bus.address)

    def tearDown(self):
        self.connection.close_sync(None)
        self.fixture.close()
        self.bus.close()

    def authorize(self, action, subject=None):
        subject = subject or ("system-bus-name", {
            "name": GLib.Variant("s", self.connection.get_unique_name())})
        return session.call(self.connection, session.POLKIT, session.AUTH_PATH,
                            session.AUTH_IFACE, "CheckAuthorization",
                            "((sa{sv})sa{ss}us)", (subject, action, {}, 0, ""))[0][0]

    def test_only_two_current_uid_actions_are_allowed(self):
        for action in session.ALLOW:
            self.assertTrue(self.authorize(action))
        self.assertFalse(self.authorize("net.reactivated.fprint.device.setusername"))
        self.assertFalse(self.authorize("org.freedesktop.login1.suspend"))
        self.assertFalse(self.authorize(next(iter(session.ALLOW)),
                                       ("unix-process", {"pid": GLib.Variant("u", os.getpid())})))
        self.assertFalse(self.authorize(next(iter(session.ALLOW)),
                                       ("system-bus-name", {"name": GLib.Variant("s", ":99.999")})))

    def test_bus_is_fresh_and_has_no_activation_services(self):
        self.assertTrue(self.bus.address.startswith("unix:path=" + self.bus.runtime.name))
        services, = session.call(self.connection, "org.freedesktop.DBus",
                                 "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                 "ListActivatableNames")
        self.assertEqual(set(services), {"org.freedesktop.DBus"})

    def test_logind_fixture_fd_is_only_private(self):
        result, descriptors = self.connection.call_with_unix_fd_list_sync(
            session.LOGIN, session.LOGIN_PATH, session.LOGIN_IFACE, "Inhibit",
            GLib.Variant("(ssss)", ("sleep", "private-test", "test", "delay")),
            GLib.VariantType.new("(h)"), 0, 5000, None, None)
        offset, = result.unpack()
        descriptor = descriptors.get(offset)
        os.close(descriptor)
        self.assertIsNone(self.fixture.real)
        self.fixture.emit_sleep(True)
        self.fixture.emit_sleep(False)


if __name__ == "__main__":
    unittest.main()

