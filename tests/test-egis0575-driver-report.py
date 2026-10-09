#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Synthetic metadata tests: no vendor binary, USB or biometric fixture."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'scripts/eh575-driver-report.py'
if not SCRIPT.exists():
    SCRIPT = Path(__file__).with_name('eh575-driver-report.py')
spec = importlib.util.spec_from_file_location('driver_report', SCRIPT)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)
BASE = 0x180000000


def fixture():
    data = bytearray(0x800)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    struct.pack_into('<4sHH', data, 0x80, b'PE\0\0', 0x8664, 4)
    struct.pack_into('<H', data, 0x94, 0xf0)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<Q', data, 0xb0, BASE)
    sections = ((b'.text', 0x40, 0x1000, 0x100, 0x400),
                (b'.rdata', 0x40, 0x2000, 0x100, 0x500),
                (b'.pdata', 24, 0x3000, 0x100, 0x600),
                (b'.data', 0x500, 0x4000, 0x100, 0x700))
    for i, section in enumerate(sections):
        struct.pack_into('<8sIIII', data, 0x188 + 40 * i, *section)
    data[0x500:0x506] = b'alpha\0'
    struct.pack_into('<IIIIII', data, 0x600, 0x1000, 0x1010, 0,
                     0x1020, 0x1040, 0)
    return data


class MetadataTests(unittest.TestCase):
    def test_landmark_and_bss(self):
        pe = report.PE64(fixture())
        self.assertEqual(pe.label('alpha'), BASE + 0x2000)
        self.assertEqual(pe.base, BASE)

    def test_function_boundaries_and_gaps(self):
        pe = report.PE64(fixture())
        for offset, expected in ((0xfff, None), (0x1000, BASE + 0x1000),
                                 (0x100f, BASE + 0x1000), (0x1010, None),
                                 (0x101f, None), (0x1020, BASE + 0x1020),
                                 (0x103f, BASE + 0x1020), (0x1040, None)):
            self.assertEqual(pe.function(BASE + offset), expected)

    def test_truncated_headers(self):
        for length in (0, 2, 0x3f, 0x84, 0x189, 0x7ff):
            with self.subTest(length=length), self.assertRaises(ValueError):
                report.PE64(fixture()[:length])

    def test_header_validation(self):
        for offset, fmt, value in ((0x84, '<H', 0x14c), (0x86, '<H', 97),
                                   (0x94, '<H', 32), (0x98, '<H', 0x10b),
                                   (0x3c, '<I', 0xffffffff)):
            data = fixture()
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                report.PE64(data)

    def test_section_validation(self):
        for offset, fmt, value in ((0x188, '<8s', b'.data'),
                                   (0x188 + 8, '<I', 0x101),
                                   (0x188 + 20, '<I', 0x780),
                                   (0x188 + 80 + 8, '<I', 23)):
            data = fixture()
            struct.pack_into(fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                report.PE64(data)

    def test_invalid_function_ranges(self):
        for start, end in ((0xff0, 0x1010), (0x1030, 0x1020),
                           (0x1008, 0x1030), (0x1020, 0x1050)):
            data = fixture()
            struct.pack_into('<II', data, 0x60c, start, end)
            with self.subTest(start=start, end=end), self.assertRaises(ValueError):
                report.PE64(data)

    def test_missing_and_duplicate_landmarks(self):
        with self.assertRaises(ValueError):
            report.PE64(fixture()).label('absent')
        data = fixture()
        data[0x510:0x516] = b'alpha\0'
        with self.assertRaises(ValueError):
            report.PE64(data).label('alpha')

    def test_bounded_input(self):
        with self.assertRaises(ValueError):
            report.PE64(b'MZ' + bytes(report.MAX_FILE))

    def test_static_reference_call_mapping(self):
        assembly = '''
   180001000: 48 8d 05 00 00 00 00 lea rax,[rip+0x0] # 0x180002000
   180001007: ff d0 call rax
   180001009: e8 00 00 00 00 call 0x180001020
   180001010: e8 00 00 00 00 call 0x180001000
   180001020: e8 00 00 00 00 call 0x180001000
'''
        item, = report.summarize(report.PE64(fixture()), assembly, ('alpha',))
        self.assertEqual(item['string_va'], '0x180002000')
        fn, = item['functions']
        self.assertEqual(fn['indirect_call_count'], 1)
        self.assertEqual(fn['direct_calls'][0]['target_va'], '0x180001020')
        self.assertEqual(len(fn['direct_callers']), 1)
        self.assertEqual(fn['direct_callers'][0]['function_va'], '0x180001020')


class InspectionTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'sample.dll'
        self.data = fixture()
        self.path.write_bytes(self.data)
        self.hash_patch = patch.object(report, 'DRIVER_SHA256', hashlib.sha256(self.data).hexdigest())

    def test_unknown_hash_never_disassembled(self):
        with patch.object(report.subprocess, 'run') as run, self.assertRaises(ValueError):
            report.inspect(self.path)
        run.assert_not_called()

    def test_success_does_not_load_dll(self):
        with self.hash_patch, \
                patch.object(report.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, '')) as run:
            with patch.object(report, 'summarize', return_value=[]) as summary:
                result = report.inspect(self.path)
        self.assertEqual(result['image_base'], hex(BASE))
        self.assertEqual(self.path.read_bytes(), self.data)
        self.assertEqual(run.call_args.args[0], ['/usr/bin/objdump', '-d', '-M', 'intel', str(self.path)])
        self.assertTrue(run.call_args.kwargs['check'])
        self.assertEqual(run.call_args.kwargs['timeout'], 30)
        summary.assert_called_once()

    def test_changed_dll_rejected(self):
        def changed(*args, **kwargs):
            self.path.write_bytes(b'changed')
            return subprocess.CompletedProcess([], 0, '')
        with self.hash_patch, patch.object(report.subprocess, 'run', side_effect=changed), \
                self.assertRaisesRegex(ValueError, 'changed during'):
            report.inspect(self.path)

    def test_disassembler_failure_not_hidden(self):
        with self.hash_patch, patch.object(report.subprocess, 'run', side_effect=subprocess.TimeoutExpired('objdump', 30)), \
                self.assertRaises(subprocess.TimeoutExpired):
            report.inspect(self.path)


if __name__ == '__main__':
    unittest.main()
