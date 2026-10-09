#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic child/pipe tests. No USB, system bus, services, sleep or login."""
import importlib.util
from pathlib import Path
import socket
import subprocess
import sys
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "eh575_sleep", Path(__file__).resolve().parents[1] / "scripts/eh575-sleep.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class WorkerTests(unittest.TestCase):
    def worker(self, program):
        child = module.Worker((sys.executable, "-u", "-c", program), log=lambda *a, **kw: None)
        self.addCleanup(lambda: child.stop(timeout=0.1, grace=0.1) if
                        child.process is not None and child.process.poll() is None else None)
        return child

    def ready(self):
        return "print(" + repr(module.ARMED.decode()) + ", flush=True); "

    def restored(self):
        return "print(" + repr(module.RESTORED.decode()) + ", flush=True); "

    def test_ready_then_restore_requires_real_exit(self):
        worker = self.worker("import sys, time; " + self.ready() +
                             "assert sys.stdin.readline() == 'RESTORE\\n'; " +
                             self.restored() + "time.sleep(.15)")
        worker.start(timeout=1)
        self.assertIsNone(worker.process.poll())
        self.assertTrue(worker.stop(timeout=1, grace=.1))
        self.assertEqual(worker.process.poll(), 0)
        self.assertTrue(worker.process.stdout.closed)

    def test_missing_restore_or_nonzero_is_not_success(self):
        for suffix in ("", self.restored() + "sys.exit(1)"):
            with self.subTest(suffix=suffix):
                worker = self.worker("import sys; " + self.ready() + "sys.stdin.readline(); " + suffix)
                worker.start(timeout=1)
                self.assertFalse(worker.stop(timeout=1, grace=.1))
                self.assertIsNotNone(worker.process.poll())

    def test_duplicate_or_out_of_order_marker_is_rejected(self):
        for sequence in (self.ready() + self.ready(), self.restored() + self.ready()):
            worker = self.worker("import sys; " + sequence + "sys.stdin.readline()")
            with self.assertRaises(RuntimeError):
                worker.start(timeout=1)
            self.assertFalse(worker.stop(timeout=1, grace=.1))

    def test_output_bound_and_termination_reap(self):
        worker = self.worker("import time; print('x' * 20000, flush=True); time.sleep(30)")
        with self.assertRaises(RuntimeError):
            worker.start(timeout=1)
        self.assertFalse(worker.stop(timeout=.1, grace=.1))
        self.assertLessEqual(len(worker.output), module.LIMIT)
        self.assertIsNotNone(worker.process.poll())

    def test_marker_does_not_substitute_for_exit(self):
        worker = self.worker("import signal, time; signal.signal(signal.SIGTERM, signal.SIG_IGN); " +
                             self.ready() + self.restored() + "time.sleep(30)")
        worker.start(timeout=1)
        self.assertFalse(worker.stop(timeout=.1, grace=.1))
        self.assertEqual(worker.process.poll(), -9)

    def test_eof_without_exit_does_not_spin_or_succeed(self):
        worker = self.worker("import os, time; " + self.ready() + "os.close(1); os.close(2); time.sleep(30)")
        worker.start(timeout=1)
        self.assertFalse(worker.stop(timeout=.1, grace=.1))
        self.assertIsNotNone(worker.process.poll())

    def test_early_exit_and_reusing_worker_rejected(self):
        worker = self.worker("raise SystemExit(3)")
        with self.assertRaises(RuntimeError):
            worker.start(timeout=1)
        self.assertFalse(worker.stop(timeout=.1, grace=.1))
        with self.assertRaises(RuntimeError):
            worker.start(timeout=.1)


class SupervisorTests(unittest.TestCase):
    def test_ordered_pre_post_and_no_overlapping_arm(self):
        workers = []
        def factory():
            worker = module.Worker((sys.executable, "-u", "-c",
                "import sys; print(" + repr(module.ARMED.decode()) + ", flush=True); " +
                "sys.stdin.readline(); print(" + repr(module.RESTORED.decode()) + ", flush=True)"),
                log=lambda *a, **kw: None)
            workers.append(worker)
            return worker
        supervisor = module.SleepSupervisor(factory=factory)
        self.addCleanup(supervisor.cleanup)
        self.assertEqual(supervisor.command("POST suspend"), "OK idle")
        self.assertEqual(supervisor.command("PRE suspend"), "OK armed")
        self.assertTrue(supervisor.command("PRE suspend").startswith("ERROR"))
        self.assertEqual(len(workers), 1)
        self.assertEqual(supervisor.command("POST suspend"), "OK restored")
        self.assertIsNone(supervisor.worker)
        self.assertEqual(supervisor.command("PRE suspend"), "OK armed")
        self.assertEqual(supervisor.command("POST suspend"), "OK restored")
        self.assertEqual(len(workers), 2)

    def test_bad_request_and_failed_permission_never_start_child(self):
        def forbidden():
            self.fail("Must not launch worker")
        supervisor = module.SleepSupervisor(factory=forbidden, prepare=lambda: False)
        for command in ("PRE suspend", "PRE hibernate", "PRE suspend;evil", "RESTORE", ""):
            self.assertTrue(supervisor.command(command).startswith("ERROR"))
        self.assertIsNone(supervisor.worker)

    def test_failed_spawn_is_cleaned_up(self):
        supervisor = module.SleepSupervisor(factory=lambda: module.Worker(("/no-such-eh575-test-worker",)))
        self.assertTrue(supervisor.command("PRE suspend").startswith("ERROR"))
        self.assertIsNone(supervisor.worker)

    def test_unknown_live_child_keeps_lease_after_failed_cleanup(self):
        class Live:
            def poll(self):
                return None
        class Stuck:
            process = Live()
            def stop(self):
                raise subprocess.TimeoutExpired("synthetic child", 1)
        supervisor = module.SleepSupervisor()
        supervisor.worker = Stuck()
        with self.assertRaises(subprocess.TimeoutExpired):
            supervisor.cleanup()
        self.assertIsNotNone(supervisor.worker)
        self.assertTrue(supervisor.command("PRE suspend").startswith("ERROR"))

    def test_peer_credentials(self):
        import os
        first, second = socket.socketpair()
        with first, second:
            self.assertEqual(module.root_peer(first), os.getuid() == 0)


class HookClientTests(unittest.TestCase):
    def request(self, phase, reply):
        client = mock.MagicMock()
        client.__enter__.return_value = client
        client.recv.side_effect = [reply, b""]
        with mock.patch.object(module, "safe_runtime"), mock.patch.object(module, "root_peer", return_value=True), \
             mock.patch.object(module.socket, "socket", return_value=client), mock.patch("builtins.print"):
            module.request(phase, "suspend")
        client.sendall.assert_called_once_with((phase.upper() + " suspend\n").encode())

    def test_only_matching_complete_reply_is_success(self):
        self.request("pre", b"OK armed\n")
        self.request("post", b"OK restored\n")
        self.request("post", b"OK idle\n")
        for phase, reply in (("pre", b"OK idle\n"), ("post", b"OK armed\n"),
                             ("pre", b"ERROR failed\n"), ("post", b"OK restored"),
                             ("post", b"OK restored\nextra\n"), ("pre", b"")):
            with self.subTest(phase=phase, reply=reply), self.assertRaises(RuntimeError):
                self.request(phase, reply)

    def test_unsupported_sleep_never_connects(self):
        with mock.patch.object(module.socket, "socket") as connection, mock.patch("builtins.print"):
            module.request("pre", "hibernate")
        connection.assert_not_called()

    def test_private_runtime_ownership_type_and_mode(self):
        import stat
        directory = mock.Mock()
        for mode, uid, safe in ((stat.S_IFDIR | 0o700, 0, True),
                               (stat.S_IFDIR | 0o755, 0, False),
                               (stat.S_IFDIR | 0o700, 1000, False),
                               (stat.S_IFLNK | 0o700, 0, False),
                               (stat.S_IFREG | 0o700, 0, False)):
            directory.lstat.return_value = mock.Mock(st_mode=mode, st_uid=uid)
            if safe:
                module.safe_runtime(directory)
            else:
                with self.assertRaises(RuntimeError):
                    module.safe_runtime(directory)


if __name__ == "__main__":
    unittest.main()
