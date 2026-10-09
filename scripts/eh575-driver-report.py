#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Read-only, hash-pinned OEM driver landmarks; never loads a DLL or opens USB."""
import argparse
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

DRIVER_SHA256 = "25704878eb4b15b41bf9389d2af0c6e44e830ad9c890f4fef2e710110c7b2631"
MAX_FILE = 2 * 1024 * 1024
LABELS = (
    "fp_tz_secure_set_detect_mode",
    "fp_tz_secure_set_detect_mode_exit",
    "fp_tz_secure_set_sensor_mode",
    "calibrate_detect_mode_5_series",
    "finger_detect",
    "get_image send EGIS_WAIT_INTERRUPT",
    "SetDetectModeParameters",
    "ResumeFromSuspend",
)


class PE64:
    """Only the PE metadata needed to locate landmarks and x64 function ranges."""

    def unpack(self, fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(self.data):
            raise ValueError("Truncated PE metadata")
        return struct.unpack_from(fmt, self.data, offset)

    def __init__(self, data):
        self.data = data
        if len(data) > MAX_FILE or data[:2] != b"MZ":
            raise ValueError("Expected a bounded PE file")
        pe, = self.unpack("<I", 0x3c)
        signature, machine, count = self.unpack("<4sHH", pe)
        optional_size, = self.unpack("<H", pe + 20)
        if signature != b"PE\0\0" or machine != 0x8664 or not 1 <= count <= 96:
            raise ValueError("Expected x64 PE")
        if optional_size < 112:
            raise ValueError("Truncated PE32+ optional header")
        magic, = self.unpack("<H", pe + 24)
        self.base, = self.unpack("<Q", pe + 48)
        if magic != 0x20b:
            raise ValueError("Expected PE32+")
        self.sections = {}
        for index in range(count):
            at = pe + 24 + optional_size + 40 * index
            name, virtual, rva, raw_size, raw = self.unpack("<8sIIII", at)
            name = name.rstrip(b"\0")
            if name in self.sections or raw + raw_size > len(data):
                raise ValueError("Invalid section extent or duplicate name")
            if name in (b".text", b".rdata", b".pdata") and virtual > raw_size:
                raise ValueError("Required section has unbacked bytes")
            self.sections[name] = (rva, virtual, raw)
        if not {b".text", b".rdata", b".pdata"} <= self.sections.keys():
            raise ValueError("Missing required PE sections")
        rva, size, raw = self.sections[b".text"]
        self.code = (self.base + rva, self.base + rva + size)
        rva, size, raw = self.sections[b".pdata"]
        if size % 12:
            raise ValueError("Invalid x64 function table length")
        self.functions = []
        for at in range(raw, raw + size, 12):
            start, end, _ = self.unpack("<III", at)
            start, end = self.base + start, self.base + end
            if not self.code[0] <= start < end <= self.code[1]:
                raise ValueError("Function outside code section")
            if self.functions and start < self.functions[-1][1]:
                raise ValueError("Unsorted or overlapping function ranges")
            self.functions.append((start, end))
        self.starts = [start for start, end in self.functions]

    def function(self, address):
        index = bisect_right(self.starts, address) - 1
        if index >= 0:
            start, end = self.functions[index]
            if start <= address < end:
                return start
        return None

    def label(self, text):
        rva, size, raw = self.sections[b".rdata"]
        blob = self.data[raw:raw + size]
        needle = text.encode("ascii") + b"\0"
        position = blob.find(needle)
        if position < 0 or blob.find(needle, position + 1) >= 0:
            raise ValueError("Missing or ambiguous expected landmark: " + text)
        return self.base + rva + position


INSTRUCTION = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+([a-z].*)$")
REFERENCE = re.compile(r"# 0x([0-9a-f]+)\b")
DIRECT_CALL = re.compile(r"^call\s+0x([0-9a-f]+)$")


def summarize(pe, disassembly, labels=LABELS):
    addresses = {text: pe.label(text) for text in labels}
    references = {address: [] for address in addresses.values()}
    calls = []
    indirect = {}
    for line in disassembly.splitlines():
        match = INSTRUCTION.match(line)
        if not match:
            continue
        instruction = int(match[1], 16)
        function = pe.function(instruction)
        if function is None:
            continue
        assembly = match[2].strip()
        reference = REFERENCE.search(assembly)
        if reference and int(reference[1], 16) in references:
            references[int(reference[1], 16)].append((instruction, function))
        call = DIRECT_CALL.fullmatch(assembly)
        if call:
            calls.append((instruction, function, int(call[1], 16)))
        elif assembly.startswith("call "):
            indirect[function] = indirect.get(function, 0) + 1
    result = []
    for text, address in addresses.items():
        related = sorted({function for _, function in references[address]})
        result.append({
            "label": text,
            "string_va": hex(address),
            "references": [{"instruction_va": hex(at), "function_va": hex(fn)}
                           for at, fn in references[address]],
            "functions": [{
                "va": hex(fn),
                "direct_calls": [{"instruction_va": hex(at), "target_va": hex(target)}
                                 for at, caller, target in calls if caller == fn],
                "direct_callers": [{"instruction_va": hex(at), "function_va": hex(caller)}
                                   for at, caller, target in calls if target == fn],
                "indirect_call_count": indirect.get(fn, 0),
            } for fn in related],
        })
    return result


def inspect(path):
    # Use a local, trusted copy. The after-read detects ordinary changes, not
    # adversarial replace-and-restore races. objdump parses bytes, not DLL code.
    path = path.resolve(strict=True)
    if not path.is_file() or path.stat().st_size > MAX_FILE:
        raise ValueError("Expected a small regular OEM DLL")
    with path.open("rb") as stream:
        data = stream.read(MAX_FILE + 1)
    digest = hashlib.sha256(data).hexdigest()
    if digest != DRIVER_SHA256:
        raise ValueError("Unsupported DLL hash; addresses are pinned to Acer 3.7.1.1")
    pe = PE64(data)
    output = subprocess.run(
        ["/usr/bin/objdump", "-d", "-M", "intel", str(path)],
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
        capture_output=True, text=True, check=True, timeout=30)
    with path.open("rb") as stream:
        after = stream.read(MAX_FILE + 1)
    if after != data:
        raise ValueError("DLL changed during inspection; discard the result")
    return {
        "sha256": digest, "image_base": hex(pe.base),
        "landmarks": summarize(pe, output.stdout),
        "limitations": "Static references, not an executed call graph or wake specification. "
                       "Indirect dispatch and hardware applicability require separate analysis. "
                       "No DLL executed, USB opened, firmware extracted or files written.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dll", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.dll), indent=2))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.exit(1, "eh575-driver-report: " + str(error) + "\n")


if __name__ == "__main__":
    main()
