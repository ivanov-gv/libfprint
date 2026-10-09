#!/usr/bin/python3 -I
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Root-only opt-in sleep-hook supervisor. No authentication or power actions.

PRE/POST hooks synchronize a retained native USB handle. They never stop
fprintd, emit input, unlock, suspend or arm a wake timer. An error skips wake.
No native worker starts at daemon startup; only a validated PRE command does.
"""
import argparse
import os
from pathlib import Path
import selectors
import signal
import socket
import stat
import struct
import subprocess
import time

RUNTIME = Path("/run/eh575-detector-sleep")
SOCKET = RUNTIME / "control"
NATIVE = "/usr/libexec/eh575-sleep-worker"
ARMED = b"EH575_SLEEP_ARMED: interface released; USB handle retained."
RESTORED = b"EH575_SLEEP_RESTORED: capture restored; USB released/closed."
LIMIT = 8192


def root_peer(sock):
    return struct.unpack("3i", sock.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[1] == 0


def safe_runtime(directory):
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
        raise RuntimeError("Require a root-owned private runtime directory")


class Worker:
    """Only wait()/poll() proves exit. Signals and cancelled waits do not."""
    def __init__(self, argv=(NATIVE, "--system-sleep-worker"), log=print):
        self.argv = tuple(argv)
        self.log = log
        self.process = None
        self.output = b""
        self.partial = b""
        self.armed = self.restored = False
        self.invalid = False
        self.eof = False

    def read(self, timeout):
        if self.eof:
            time.sleep(max(0, min(timeout, 0.1)))
            return
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            if not selector.select(max(0, timeout)):
                return
            block = os.read(self.process.stdout.fileno(), 1024)
        if not block:
            self.eof = True
            return
        if self.invalid:
            return  # Drain, without logging or growing untrusted output.
        self.output += block
        self.partial += block
        if len(self.output) > LIMIT:
            self.invalid = True
            self.output = self.output[:LIMIT]
            self.partial = b""
            return
        while b"\n" in self.partial:
            line, self.partial = self.partial.split(b"\n", 1)
            self.log("eh575-sleep: " + line.decode("utf-8", errors="replace"), flush=True)
            if line == ARMED:
                if self.armed:
                    self.invalid = True
                    return
                self.armed = True
            if line == RESTORED:
                if self.restored or not self.armed:
                    self.invalid = True
                    return
                self.restored = True

    def start(self, timeout=15):
        if self.process is not None:
            raise RuntimeError("Worker instance cannot be restarted")
        self.process = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, bufsize=0, close_fds=True,
                                        env={"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C.UTF-8"})
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.read(min(0.1, end - time.monotonic()))
            if self.invalid:
                raise RuntimeError("Native worker output invalid or exceeded bound")
            if self.process.poll() is not None:
                raise RuntimeError("Native worker exited before arming")
            if self.armed:
                return
        raise RuntimeError("Native arming timed out")

    def await_exit(self, timeout):
        end = time.monotonic() + timeout
        while self.process.poll() is None and time.monotonic() < end:
            self.read(min(0.1, end - time.monotonic()))
        if self.process.poll() is None:
            return False
        # Reap and drain this exact child; never infer exit from a PID file.
        self.process.wait()
        while True:
            before = len(self.output)
            self.read(0)
            if len(self.output) == before:
                break
        return True

    def stop(self, timeout=7, grace=6):
        if self.process is None:
            return False
        try:
            if self.process.poll() is None:
                try:
                    self.process.stdin.write(b"RESTORE\n")
                except (BrokenPipeError, OSError):
                    pass
            if not self.await_exit(timeout):
                self.invalid = True
                self.process.terminate()
                if not self.await_exit(grace):
                    self.process.kill()
                    self.process.wait(timeout=3)
            return (not self.invalid and self.armed and self.restored and
                    self.process.returncode == 0)
        finally:
            # Closing pipes is not a substitute for reaping the live child.
            if self.process.poll() is not None:
                self.process.stdin.close()
                self.process.stdout.close()


class SleepSupervisor:
    def __init__(self, factory=Worker, prepare=lambda: True):
        self.factory = factory
        self.prepare = prepare
        self.worker = None

    def command(self, command):
        if command == "PRE suspend":
            if self.worker is not None:
                return "ERROR previous worker still retained; POST required"
            if not self.prepare():
                return "ERROR wake permission unavailable; detector not armed"
            worker = self.factory()
            self.worker = worker
            try:
                worker.start()
                return "OK armed"
            except Exception as error:
                self.cleanup()
                return "ERROR arming failed: " + str(error)[:160]
        if command == "POST suspend":
            if self.worker is None:
                return "OK idle"
            return "OK restored" if self.cleanup() else "ERROR cleanup failed; test normal verification"
        return "ERROR unsupported sleep command"

    def cleanup(self):
        worker = self.worker
        if worker is None:
            return True
        try:
            return worker.stop()
        finally:
            # Preserve the lease if a pathological uninterruptible child is
            # still live. A new PRE must never overlap it.
            if worker.process is None or worker.process.poll() is not None:
                self.worker = None


def permission():
    """Existing opted-in helper owns wake sysfs policy; this code never does."""
    try:
        info = Path("/run/eh575-wakeup/state.json").lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o077:
            return False
        subprocess.run(["/usr/libexec/eh575-wakeup", "reapply"], check=True, timeout=8,
                       env={"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LANG": "C.UTF-8"})
        return True
    except (OSError, subprocess.SubprocessError):
        return False


def serve():
    safe_runtime(RUNTIME)
    # Never remove a pre-existing path: fail startup rather than unlink an
    # unknown socket. systemd owns/removes RuntimeDirectory on normal stop.
    running = True
    def stop(signum, frame):
        nonlocal running
        running = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    supervisor = SleepSupervisor(prepare=permission)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(SOCKET))
        os.chmod(SOCKET, 0o600)
        server.listen(2)
        server.settimeout(0.5)
        try:
            while running:
                try:
                    client, _ = server.accept()
                except TimeoutError:
                    continue
                with client:
                    client.settimeout(2)  # A root hook sends its short request immediately.
                    if not root_peer(client):
                        continue
                    try:
                        request = b""
                        while b"\n" not in request and len(request) < 32:
                            part = client.recv(32 - len(request))
                            if not part:
                                break
                            request += part
                        command = request.decode("ascii")
                        if command not in ("PRE suspend\n", "POST suspend\n"):
                            response = "ERROR malformed sleep request"
                        else:
                            response = supervisor.command(command[:-1])
                        print("eh575-sleep: " + response, flush=True)
                        client.sendall(response.encode("ascii", errors="replace") + b"\n")
                    except (OSError, UnicodeError) as error:
                        print("eh575-sleep: control error: " + str(error), flush=True)
        finally:
            supervisor.cleanup()
            SOCKET.unlink(missing_ok=True)  # Only our own successfully bound socket.


def request(phase, action):
    if action != "suspend":
        print("eh575-sleep: unsupported sleep type; no detector arming", flush=True)
        return
    safe_runtime(RUNTIME)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(35)
        client.connect(str(SOCKET))
        if not root_peer(client):
            raise RuntimeError("Sleep control peer is not root")
        client.sendall((phase.upper() + " suspend\n").encode("ascii"))
        reply = b""
        while b"\n" not in reply and len(reply) < 256:
            block = client.recv(256 - len(reply))
            if not block:
                break
            reply += block
        if not reply.endswith(b"\n"):
            raise RuntimeError("Sleep supervisor reply incomplete")
        print("eh575-sleep: " + reply.decode("ascii", errors="replace").strip(), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("serve", "pre", "post"))
    parser.add_argument("action", nargs="?", choices=("suspend", "hibernate", "hybrid-sleep", "suspend-then-hibernate"))
    args = parser.parse_args()
    if os.getuid() != 0 or os.geteuid() != 0:
        parser.error("Root-only system-sleep hook; use the unprivileged manual probe for research")
    if (args.mode == "serve") != (args.action is None):
        parser.error("serve accepts no action; pre/post require an action")
    try:
        if args.mode == "serve":
            serve()
        else:
            request(args.mode, args.action)
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("eh575-sleep: skipped/failed: " + str(error), flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
