#!/usr/bin/python3 -I
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Opt-in EH575 USB wake permission; no sensor commands or authentication changes."""
import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import time

SYSFS = Path("/sys")
STATE = Path("/run/eh575-wakeup")
IDENTITY = ("1c7a", "0575", "1072")


def read(path):
    return path.read_text().strip()


def identity(node):
    return tuple(read(node / key) for key in ("idVendor", "idProduct", "bcdDevice"))


def targets(sysfs=SYSFS, check_platform=False):
    """Resolve only the tested reader and its USB hub ancestors, never PCI policy."""
    devices = (sysfs / "devices").resolve(strict=True)
    found = {}
    readers = []
    for link in sorted((sysfs / "bus/usb/devices").iterdir()):
        try:
            ids = identity(link)
        except FileNotFoundError:
            continue  # Non-device entry or an unplug during identification.
        if ids != IDENTITY:
            continue
        node = link.resolve(strict=True)
        if not node.is_relative_to(devices):
            raise ValueError("USB node is outside sysfs devices")
        readers.append(node)
        # An unplug/missing attribute AFTER identification must fail closed,
        # rather than enabling only part of the intended wake path.
        found[node] = read(node / "power/wakeup")
        for parent in node.parents:
            if parent == devices:
                break
            if (parent / "bDeviceClass").is_file():
                if read(parent / "bDeviceClass") != "09":
                    raise ValueError("Unexpected non-hub USB ancestor")
                found[parent] = read(parent / "power/wakeup")
            elif (parent / "power/wakeup").is_file():
                # Do not broaden PCI/platform wake policy. It must already
                # permit wake; changing it needs a separate platform trial.
                if check_platform and read(parent / "power/wakeup") == "disabled":
                    raise ValueError(f"Platform wake is disabled: {parent}")
    if not readers:
        raise ValueError("No tested EH575 (1c7a:0575 revision 1072) found")
    if any(value not in ("enabled", "disabled") for value in found.values()):
        raise ValueError("Reader/hub does not expose usable remote wake permission")
    return readers, found


def state_directory(directory):
    directory.mkdir(mode=0o700, exist_ok=True)
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError("Wake state must be an owned, non-symlink 0700 directory")


@contextmanager
def locked(directory):
    state_directory(directory)
    fd = os.open(directory / "lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600:
            raise ValueError("Unsafe wake state lock")
        # Do not delay sleep behind another invocation. Log contention instead.
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def load(directory):
    path = directory / "state.json"
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return {}, False
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o600 or info.st_size > 65536:
            raise ValueError("Unsafe wake state file")
        value = json.load(stream)
    if not isinstance(value, dict) or value.get("schema") != 1 or not isinstance(value.get("original"), dict) or type(value.get("active")) is not bool:
        raise ValueError("Invalid wake state schema")
    original = value["original"]
    if len(original) > 128 or any(not isinstance(key, str) or val not in ("enabled", "disabled") for key, val in original.items()):
        raise ValueError("Invalid wake state entries")
    return original, value["active"]


def save(directory, original, active=True):
    fd, name = tempfile.mkstemp(prefix=".state-", dir=directory)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump({"schema": 1, "original": original, "active": active}, stream)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, directory / "state.json")
    finally:
        if os.path.exists(name):
            os.unlink(name)


def write_wakeup(node, value):
    # Nodes are freshly derived from the tested reader's actual ancestor chain,
    # never taken as write destinations from the saved JSON.
    (node / "power/wakeup").write_text(value + "\n")


def enable(sysfs=SYSFS, directory=STATE, reapply=False):
    with locked(directory):
        _enable(sysfs, directory, reapply)


def _enable(sysfs, directory, reapply):
    original, active = load(directory)
    if reapply and not active:
        return  # No opt-in; never enable merely because a sleep hook ran.
    _, nodes = targets(sysfs, check_platform=True)
    for node, current in nodes.items():
        original.setdefault(str(node), current)
    save(directory, original)  # Journal before the first sysfs mutation.
    try:
        for node in nodes:
            write_wakeup(node, "enabled")
            if read(node / "power/wakeup") != "enabled":
                raise ValueError(f"Wake permission did not stick: {node}")
    except (OSError, ValueError):
        _disable(sysfs, directory)
        raise


def disable(sysfs=SYSFS, directory=STATE):
    with locked(directory):
        _disable(sysfs, directory)


def _disable(sysfs, directory):
    original, _ = load(directory)
    if not original:
        return
    save(directory, original, active=False)  # Disarm even if restoration fails.
    try:
        _, nodes = targets(sysfs)
    except ValueError:
        # A missing reader cannot be restored safely; retain the journal and
        # report failure rather than writing arbitrary paths from the snapshot.
        raise ValueError("Cannot safely restore wake policy: reader/path unavailable")
    for node in nodes:
        if str(node) in original:
            write_wakeup(node, original[str(node)])
            if read(node / "power/wakeup") != original[str(node)]:
                raise ValueError(f"Wake policy restoration failed: {node}")
    # Keep unrelated entries if the device moved, rather than silently losing
    # a restore record. A reboot resets these transient kernel policies.
    remaining = {key: value for key, value in original.items() if key not in {str(node) for node in nodes}}
    if remaining:
        save(directory, remaining, active=False)
        raise ValueError("Old USB path could not be restored; reboot clears transient wake policy")
    (directory / "state.json").unlink()


def observation(sysfs=SYSFS):
    """Read metadata only. No USB opens, serials, packets or sensor commands."""
    readers, nodes = targets(sysfs)
    result = {"devices": [], "platform": []}

    def optional(path):
        try:
            value = read(path)
        except OSError:
            return None
        return value if value and len(value) <= 128 and value.isascii() and not any(ord(c) < 32 for c in value) else None

    for node, wake in nodes.items():
        item = {"node": node.name, "role": "reader" if node in readers else "hub", "wakeup": wake}
        for key in ("runtime_status", "control", "persist", "wakeup_count", "wakeup_active_count", "wakeup_abort_count"):
            item[key] = optional(node / "power" / key)
        result["devices"].append(item)
    platform = set()
    for reader in readers:
        for parent in reader.parents:
            if parent == (sysfs / "devices").resolve(strict=True):
                break
            if parent not in nodes and (parent / "power/wakeup").is_file():
                platform.add(parent)
    for node in sorted(platform):
        result["platform"].append({"node": node.name, "wakeup": optional(node / "power/wakeup")})
    result["mem_sleep"] = optional(sysfs / "power/mem_sleep")
    result["last_wakeup_irq"] = optional(sysfs / "power/pm_wakeup_irq")
    return result


def sleep_observation(phase, sysfs=SYSFS, directory=STATE):
    """Journal an opted-in pre/post snapshot; only pre reapplies USB policy."""
    if phase not in ("pre", "post"):
        raise ValueError("Invalid sleep phase")
    with locked(directory):
        _, active = load(directory)
        if not active:
            return  # Inactive restore journals must not rearm or log a trial.
        if phase == "pre":
            _enable(sysfs, directory, reapply=True)
        snapshot = observation(sysfs)
        snapshot.update(phase=phase, opt_in=True, monotonic_ns=time.monotonic_ns(),
                        boottime_ns=time.clock_gettime_ns(time.CLOCK_BOOTTIME))
        if phase == "pre" and any(item["wakeup"] != "enabled" for item in snapshot["devices"]):
            raise ValueError("USB wake permission changed before the pre-sleep snapshot")
        print("EH575 sleep snapshot: " + json.dumps(snapshot, sort_keys=True), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("status", "enable", "reapply", "disable", "sleep-pre", "sleep-post"))
    args = parser.parse_args()
    try:
        if args.action == "status":
            readers, nodes = targets()
            for node, value in nodes.items():
                label = "EH575" if node in readers else "USB hub"
                print(f"{label}: {node.name}: wake {value}")
            print("Permission is not proof of wake-on-touch; a physical suspend test is required.")
        else:
            if os.geteuid() != 0:
                parser.error("Wake changes require root; status is read-only")
            if args.action.startswith("sleep-"):
                sleep_observation(args.action.removeprefix("sleep-"))
            elif args.action == "disable":
                disable()
            else:
                enable(reapply=args.action == "reapply")
        return 0
    except (OSError, ValueError) as error:
        print(f"EH575 wake: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
