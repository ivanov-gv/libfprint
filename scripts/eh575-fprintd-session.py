#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Isolated REAL fprintd with a native EH575 build; never a login service.

Python supplies only test-bus supervision, scoped authorization and prompts.
Acquisition, templates and recognition remain in real fprintd/native libfprint.
"""
import argparse
import configparser
import fcntl
import json
import os
from pathlib import Path
import selectors
import signal
import stat
import subprocess
import tempfile
import threading
import time
import warnings
from xml.sax.saxutils import escape

from gi.repository import Gio, GLib

SERVICE = "net.reactivated.Fprint"
MANAGER = "/net/reactivated/Fprint/Manager"
DEVICE = SERVICE + ".Device"
POLKIT = "org.freedesktop.PolicyKit1"
AUTH_PATH = "/org/freedesktop/PolicyKit1/Authority"
AUTH_IFACE = POLKIT + ".Authority"
LOGIN = "org.freedesktop.login1"
LOGIN_PATH = "/org/freedesktop/login1"
LOGIN_IFACE = LOGIN + ".Manager"
ALLOW = {"net.reactivated.fprint.device.verify", "net.reactivated.fprint.device.enroll"}
XML = """<node>
<interface name="org.freedesktop.PolicyKit1.Authority">
 <method name="CheckAuthorization">
  <arg type="(sa{sv})" direction="in"/><arg type="s" direction="in"/>
  <arg type="a{ss}" direction="in"/><arg type="u" direction="in"/>
  <arg type="s" direction="in"/><arg type="(bba{ss})" direction="out"/>
 </method>
 <property name="BackendName" type="s" access="read"/>
 <property name="BackendVersion" type="s" access="read"/>
 <property name="BackendFeatures" type="u" access="read"/>
</interface>
<interface name="org.freedesktop.login1.Manager">
 <method name="Inhibit">
  <arg type="s" direction="in"/><arg type="s" direction="in"/>
  <arg type="s" direction="in"/><arg type="s" direction="in"/>
  <arg type="h" direction="out"/>
 </method>
 <signal name="PrepareForSleep"><arg type="b"/></signal>
</interface></node>"""


def connect(address):
    return Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT |
        Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)


def call(connection, name, path, interface, method, signature=None, args=()):
    parameters = GLib.Variant(signature, args) if signature else None
    return connection.call_sync(name, path, interface, method, parameters,
                                None, Gio.DBusCallFlags.NONE, 20000, None).unpack()


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=4)


class PrivateBus:
    """Own a fresh Unix bus with no activation directories or host bus access."""
    def __init__(self):
        self.runtime = tempfile.TemporaryDirectory(prefix="eh575-fprintd-")
        self.process = None
        root = Path(self.runtime.name)
        socket = root / "bus"
        config = root / "bus.conf"
        config.write_text(
            '<busconfig><type>session</type><listen>unix:path=' +
            escape(str(socket)) + '</listen><auth>EXTERNAL</auth>' +
            '<policy user="' + str(os.getuid()) + '">' +
            '<allow own="*"/><allow send_destination="*"/>' +
            '<allow receive_sender="*"/></policy></busconfig>')
        try:
            self.process = subprocess.Popen(
                ["dbus-daemon", "--nofork", "--config-file=" + str(config), "--print-address=1"],
                stdout=subprocess.PIPE, text=True)
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stdout, selectors.EVENT_READ)
                if not selector.select(5):
                    raise RuntimeError("Private bus startup timed out")
                self.address = self.process.stdout.readline().strip()
            if not self.address.startswith("unix:path=" + str(socket) + ",guid="):
                raise RuntimeError("Private bus did not return its owned socket address")
        except BaseException:
            self.close()
            raise

    def close(self):
        stop(self.process)
        if self.process and self.process.stdout:
            self.process.stdout.close()
        self.runtime.cleanup()


class Fixtures:
    """Test ONLY: own two services on our private bus, never on a host bus."""
    def __init__(self, bus, forward_sleep=False):
        self.connection = connect(bus.address)
        self.loop = GLib.MainLoop()
        self.thread = None
        self.real = None
        self.stage = 0
        self.properties = {}
        self.enrolling = False
        self.lifecycle_test = False
        self.verify_results = []
        self.registrations = []
        info = Gio.DBusNodeInfo.new_for_xml(XML)
        for name, path, interface in ((POLKIT, AUTH_PATH, AUTH_IFACE),
                                      (LOGIN, LOGIN_PATH, LOGIN_IFACE)):
            # Keep the API supported by Ubuntu 24.04 too. Recent PyGObject
            # deprecates it, but newer closure APIs are not universally present.
            with warnings.catch_warnings():
                warnings.filterwarnings("ignore", category=DeprecationWarning,
                                        message="Gio.DBusConnection.register_object is deprecated")
                self.registrations.append(self.connection.register_object(
                    path, info.lookup_interface(interface), self.method, self.property, None))
            result = call(self.connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "RequestName", "(su)", (name, 4))
            if result != (1,):
                raise RuntimeError("Could not own private fixture name")
        self.connection.signal_subscribe(
            SERVICE, "org.freedesktop.DBus.Properties", "PropertiesChanged", None,
            DEVICE, Gio.DBusSignalFlags.NONE, self.device_properties)
        self.connection.signal_subscribe(
            SERVICE, DEVICE, "EnrollStatus", None, None,
            Gio.DBusSignalFlags.NONE, self.enroll_status)
        self.connection.signal_subscribe(
            SERVICE, DEVICE, "VerifyStatus", None, None,
            Gio.DBusSignalFlags.NONE, self.verify_status)
        if forward_sleep:
            # This is the ONLY connection to the host bus: subscribe to genuine
            # logind notifications, never call a power/authentication method.
            address = os.environ.get("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket")
            self.real = connect(address)
            self.real.signal_subscribe(
                LOGIN, LOGIN_IFACE, "PrepareForSleep", LOGIN_PATH, None,
                Gio.DBusSignalFlags.NONE, self.forward_sleep)
        self.thread = threading.Thread(target=self.loop.run, daemon=True)
        self.thread.start()

    def property(self, connection, sender, path, interface, name):
        values = {"BackendName": ("s", "eh575-private-test-only"),
                  "BackendVersion": ("s", "1"), "BackendFeatures": ("u", 0)}
        signature, value = values[name]
        return GLib.Variant(signature, value)

    def method(self, connection, sender, path, interface, method, parameters, invocation):
        if interface == AUTH_IFACE and method == "CheckAuthorization":
            subject, action, details, flags, cancellation = parameters.unpack()
            allowed = False
            if subject[0] == "system-bus-name" and action in ALLOW:
                name = subject[1].get("name", "")
                if name.startswith(":"):
                    try:
                        uid, = call(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                    "org.freedesktop.DBus", "GetConnectionUnixUser", "(s)", (name,))
                        allowed = uid == os.getuid()
                    except GLib.Error:
                        pass
            invocation.return_value(GLib.Variant("((bba{ss}))", ((allowed, False, {}),)))
        elif interface == LOGIN_IFACE and method == "Inhibit":
            # A dummy FD on the PRIVATE bus. It cannot delay real system sleep.
            descriptor = os.open("/dev/null", os.O_RDONLY)
            try:
                descriptors = Gio.UnixFDList.new()
                index = descriptors.append(descriptor)
                invocation.return_value_with_unix_fd_list(GLib.Variant("(h)", (index,)), descriptors)
            finally:
                os.close(descriptor)
        else:
            invocation.return_dbus_error("org.freedesktop.DBus.Error.UnknownMethod",
                                         "Private fixture method not implemented")

    def emit_sleep(self, sleeping):
        self.connection.emit_signal(None, LOGIN_PATH, LOGIN_IFACE, "PrepareForSleep",
                                    GLib.Variant("(b)", (sleeping,)))
        self.connection.flush_sync(None)

    def forward_sleep(self, connection, sender, path, interface, signal_name, parameters):
        sleeping, = parameters.unpack()
        print("Host sleep notification forwarded to private fprintd: " + str(sleeping), flush=True)
        self.emit_sleep(sleeping)

    def enroll_status(self, connection, sender, path, interface, signal_name, parameters):
        result, done = parameters.unpack()
        if result == "enroll-stage-passed":
            self.stage += 1
            print("Captured. Lift fully before the next touch.", flush=True)
        elif not done:
            print("Retry: lift fully, then use a smaller flat-pad shift.", flush=True)

    def verify_status(self, connection, sender, path, interface, signal_name, parameters):
        self.verify_results.append(parameters.unpack())

    def device_properties(self, connection, sender, path, interface, signal_name, parameters):
        device_interface, changed, invalid = parameters.unpack()
        current = self.properties.setdefault(path, {})
        previous = (current.get("finger-needed", False), current.get("finger-present", False))
        current.update(changed)
        needed, present = current.get("finger-needed", False), current.get("finger-present", False)
        if needed and not present and previous != (True, False):
            if self.lifecycle_test:
                print("Reader calibrated. Keep EMPTY for the lifecycle test.", flush=True)
            elif self.enrolling:
                areas = ("center", "slightly tip-side", "slightly base-side",
                         "slightly left-side", "slightly right-side")
                area = areas[min(self.stage // 3, 4)]
                print(f"Reader ready: touch {self.stage + 1}/15 [{area}]. Hold flat and still; do not slide.", flush=True)
            else:
                print("Reader calibrated. Place your finger flat and hold still; do not slide.", flush=True)
        elif present and not needed and previous != (False, True):
            print("Captured. Lift your finger.", flush=True)

    def close(self):
        if self.real:
            self.real.close_sync(None)
        self.loop.quit()
        if self.thread:
            self.thread.join(timeout=3)
        for registration in self.registrations:
            self.connection.unregister_object(registration)
        self.connection.close_sync(None)


def private_directory(path):
    path = path.absolute()
    if ":" in str(path) or any(p.is_symlink() for p in (path, *path.parents)):
        raise ValueError("Private storage must not contain colons or symlink directories")
    path.mkdir(parents=True, mode=0o700, exist_ok=True)
    for item in (path, *path.rglob("*")):
        mode = item.lstat()
        if stat.S_ISLNK(mode.st_mode) or mode.st_uid != os.getuid() or mode.st_mode & 0o077:
            raise ValueError("Private state and all its contents must be user-owned and private")
        if not (stat.S_ISDIR(mode.st_mode) or stat.S_ISREG(mode.st_mode)):
            raise ValueError("Private state contains a non-regular artifact")
    return path


def validate_build(build, deps):
    build = build.resolve(strict=True)
    options = json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    options = {item["name"]: item["value"] for item in options}
    if options.get("drivers") != "egis0575" or options.get("egis0575_ridge") is not True:
        raise ValueError("Use an isolated -Ddrivers=egis0575 -Degis0575_ridge=true build")
    library = build / "libfprint/libfprint-2.so.2"
    if not library.is_file():
        raise ValueError("Compile the native ridge build first")
    directories = [str(library.parent)]
    if deps:
        root = deps.resolve(strict=True) / "usr/lib/x86_64-linux-gnu"
        directories += [str(root), str(root / "openblas-pthread")]
    return library.resolve(), ":".join(directories)


def client_env(address):
    env = os.environ.copy()
    # Neither user shell flags nor LD overrides may silently redirect this test.
    for key in list(env):
        if key.startswith("FP_") or key in ("G_MESSAGES_DEBUG", "LD_PRELOAD", "LD_LIBRARY_PATH", "STATE_DIRECTORY"):
            env.pop(key)
    env["DBUS_SYSTEM_BUS_ADDRESS"] = address
    env["DBUS_SESSION_BUS_ADDRESS"] = address
    return env


def discover(connection):
    paths, = call(connection, SERVICE, MANAGER, SERVICE + ".Manager", "GetDevices")
    if len(paths) != 1:
        raise RuntimeError(f"Expected one experimental EH575 device, found {len(paths)}")
    properties, = call(connection, SERVICE, paths[0], "org.freedesktop.DBus.Properties",
                       "GetAll", "(s)", (DEVICE,))
    if (properties.get("name") != "EgisTec EH575 (experimental stationary ridge)" or
            properties.get("scan-type") != "press" or properties.get("num-enroll-stages") != 15):
        raise RuntimeError("Daemon did not expose the native stationary ridge driver")
    return paths[0]


def sleep_lifecycle(bus, fixture):
    """Use the STOCK client's release on the terminal error, before wake.

    Waiting until after PrepareForSleep(false) to Release misses a real
    libfprint/fprintd lost-claim/open-device failure.
    """
    connection = connect(bus.address)
    path = discover(connection)
    child = None
    asleep = False
    try:
        child = subprocess.Popen(["fprintd-verify", "-f", "right-index-finger"],
                                 env=client_env(bus.address), stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 16
        while time.monotonic() < deadline:
            properties, = call(connection, SERVICE, path, "org.freedesktop.DBus.Properties",
                               "GetAll", "(s)", (DEVICE,))
            if properties.get("finger-needed"):
                break
            if child.poll() is not None:
                raise RuntimeError("Stock verification client exited before calibration was ready")
            time.sleep(.1)
        else:
            raise RuntimeError("Calibration did not reach the ready state")
        fixture.emit_sleep(True)
        asleep = True
        output, _ = child.communicate(timeout=10)
        print(output, end="", flush=True)
        if "ReleaseDevice failed" in output or not any(done for result, done in fixture.verify_results):
            raise RuntimeError("Stock client did not cleanly release the interrupted scan before resume")
        if any(result == "verify-match" for result, done in fixture.verify_results):
            raise RuntimeError("An empty-reader suspend test unexpectedly matched; do not deploy")
        fixture.emit_sleep(False)
        asleep = False
        deadline = time.monotonic() + 6
        while True:
            try:
                call(connection, SERVICE, path, DEVICE, "Claim", "(s)", ("",))
                break
            except GLib.Error:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.1)
        call(connection, SERVICE, path, DEVICE, "Release")
        print("Stock client released BEFORE resume; the same daemon can reopen/release after wake. Now verify with a real touch.", flush=True)
    finally:
        if asleep:
            fixture.emit_sleep(False)
        stop(child)
        connection.close_sync(None)


def lifecycle(bus, fixture, sleeping=False):
    print("Keep the reader EMPTY throughout this cancellation test.", flush=True)
    fixture.lifecycle_test = True
    fixture.verify_results.clear()
    if sleeping:
        return sleep_lifecycle(bus, fixture)
    connection = connect(bus.address)
    path = discover(connection)
    def operation(method, signature=None, args=()):
        return call(connection, SERVICE, path, DEVICE, method, signature, args)
    try:
        operation("Claim", "(s)", ("",))
        operation("VerifyStart", "(s)", ("right-index-finger",))
        deadline = time.monotonic() + 16
        while time.monotonic() < deadline:
            properties, = call(connection, SERVICE, path, "org.freedesktop.DBus.Properties",
                               "GetAll", "(s)", (DEVICE,))
            if properties.get("finger-needed"):
                break
            time.sleep(.1)
        else:
            raise RuntimeError("Calibration did not reach the ready state")
        operation("VerifyStop")
        operation("Release")
        operation("Claim", "(s)", ("",))
        operation("Release")
        if any(result == "verify-match" for result, done in fixture.verify_results):
            raise RuntimeError("An empty-reader lifecycle test unexpectedly matched; do not deploy")
        print("Private " + ("simulated sleep/resume" if sleeping else "VerifyStop") +
              " completed; device can be reclaimed. Now verify with a real touch.", flush=True)
        if not sleeping:
            operation("Claim", "(s)", ("",))
            operation("VerifyStart", "(s)", ("right-index-finger",))
            connection.close_sync(None)
            connection = connect(bus.address)
            deadline = time.monotonic() + 6
            while True:
                try:
                    operation("Claim", "(s)", ("",))
                    break
                except GLib.Error:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.1)
            operation("Release")
            if any(result == "verify-match" for result, done in fixture.verify_results):
                raise RuntimeError("A disconnect test unexpectedly matched; do not deploy")
            print("Disconnected client was released; device can be reclaimed.", flush=True)
    finally:
        if not connection.is_closed():
            connection.close_sync(None)


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--deps", type=Path)
    parser.add_argument("--state", type=Path, default=Path(__file__).resolve().parents[1] / ".state/eh575-fprintd")
    parser.add_argument("--forward-sleep", action="store_true",
                        help="Read real logind sleep signals and forward ONLY those to the private bus")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="-- check | cancel-test | sleep-test | fprintd-enroll/verify/list/delete [args]")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Run without sudo; this fixture must never serve system authentication")
    try:
        library, libraries = validate_build(args.build, args.deps)
        state = private_directory(args.state)
        prints = private_directory(state / "prints")
        config = configparser.ConfigParser()
        config.read("/etc/fprintd.conf")
        if config.get("storage", "type", fallback=None) != "file":
            raise ValueError("The installed real fprintd must use its file-storage backend")
        command = args.command
        if command and command[0] == "--":
            command = command[1:]
        allowed = ("check", "cancel-test", "sleep-test", "fprintd-enroll",
                   "fprintd-verify", "fprintd-list", "fprintd-delete")
        if command and command[0] not in allowed:
            raise ValueError("Choose a listed private test command; omit it for an isolated shell")
        if command and command[0] in ("check", "cancel-test", "sleep-test") and len(command) != 1:
            raise ValueError("Built-in lifecycle tests do not take additional arguments")
        daemon = Path("/usr/libexec/fprintd")
        if not daemon.is_file():
            raise ValueError("Ubuntu's real /usr/libexec/fprintd is required")
        with (state / ".session.lock").open("a+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return run(args, command, state, prints, library, libraries, daemon)
    except (ValueError, OSError, RuntimeError, GLib.Error) as error:
        print("Isolated fprintd test failed: " + str(error), flush=True)
        return 1


def run(args, command, state, prints, library, libraries, daemon):
    calibration = private_directory(state / "calibration")
    bus = PrivateBus()
    fixture = None
    process = None
    connection = None
    child = None
    try:
        fixture = Fixtures(bus, args.forward_sleep)
        env = client_env(bus.address)
        daemon_env = dict(env, LD_LIBRARY_PATH=libraries, FP_DRIVERS_ALLOWLIST="egis0575",
                          STATE_DIRECTORY=str(prints), FP_EH575_CALIBRATION_DIR=str(calibration))
        print("REAL fprintd on a fresh private bus. Templates: " + str(prints), flush=True)
        print("Test-only authorization. GNOME/PAM/system fprintd remain unchanged.", flush=True)
        process = subprocess.Popen([str(daemon), "--no-timeout"], env=daemon_env)
        connection = connect(bus.address)
        deadline = time.monotonic() + 10
        while True:
            if process.poll() is not None:
                raise RuntimeError("Real daemon exited before becoming ready")
            owned, = call(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "NameHasOwner", "(s)", (SERVICE,))
            if owned:
                break
            if time.monotonic() > deadline:
                raise RuntimeError("Real daemon startup timed out")
            time.sleep(.1)
        # Evidence that the STOCK daemon actually mapped our native library.
        maps = Path(f"/proc/{process.pid}/maps").read_text()
        if str(library) not in maps:
            raise RuntimeError("Real daemon did not load the selected native library")
        path = discover(connection)
        print("Native EH575 exposed as press, 15 enrollment stages: " + path, flush=True)
        if command == ["check"]:
            call(connection, SERVICE, path, DEVICE, "Claim", "(s)", ("",))
            call(connection, SERVICE, path, DEVICE, "Release")
            print("Discovery, authorization, native open/release passed; no scan or template saved.", flush=True)
            return 0
        if command in (["cancel-test"], ["sleep-test"]):
            lifecycle(bus, fixture, command == ["sleep-test"])
            return 0
        fixture.enrolling = bool(command and command[0] == "fprintd-enroll")
        if fixture.enrolling:
            print("Keep empty until Reader ready. Three flat stationary touches each at center/tip/base/left/right; move only BETWEEN touches.", flush=True)
            print("Enrollment/re-enrollment affects ONLY this separate private fprintd storage.", flush=True)
        if not command:
            print("Private shell: fprintd-enroll -f right-index-finger; fprintd-verify; exit.", flush=True)
            print("Keep the sensor empty until Reader calibrated. fprintd-delete removes ONLY private test prints.", flush=True)
            command = ["bash", "--norc", "-i"]
        child = subprocess.Popen(command, env=env)
        while child.poll() is None:
            if process.poll() is not None:
                raise RuntimeError("Real daemon exited while the client was running")
            time.sleep(.1)
        return child.returncode if child.returncode >= 0 else 128 - child.returncode
    finally:
        # Only processes created here and the owned temporary bus are stopped.
        stop(child)
        stop(process)
        if connection:
            connection.close_sync(None)
        if fixture:
            fixture.close()
        bus.close()
        print("Private session closed; its enrollment is preserved. System authentication unchanged.", flush=True)


if __name__ == "__main__":
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("Private test stopped.", flush=True)
        raise SystemExit(130)
