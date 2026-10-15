"""BFCLIENT02 bounded channel packet instructions, with packet I/O adapters.
This does not execute full UI handlers, sockets, or a complete channel transfer.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import *

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x20000000
BP, STACK = DATA + 0x10000, DATA + 0xf000
VT, IO, HEADER, OBJ, ARG = [DATA + n for n in (0x100, 0x200, 0x300, 0x400, 0x1000)]


def put(m, a, *v):
    m.mem_write(a, struct.pack('<' + 'I' * len(v), *v))


def get(m, a):
    return struct.unpack('<I', m.mem_read(a, 4))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', required=True, type=Path)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    mapped = pe.get_memory_mapped_image()
    evidence_path = ROOT / 'docs/evidence/channel_transfer_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    for key in ('functions', 'serialization_ranges', 'resources', 'data_ranges'):
        for row in evidence[key]:
            b = bytes.fromhex(row['bytes'])
            assert pe.get_data(int(row['rva'], 16), len(b)) == b, (key, row['va'])
    fixture = ROOT / 'tests/channel_transfer_packets.txt'
    vectors = [line.split('|') for line in fixture.read_text('utf-8').splitlines()
               if line and not line.startswith('#')]
    ranges = {r['label']: (int(r['va'], 16), int(r['end'], 16))
              for r in evidence['serialization_ranges']}
    runs = []

    def machine():
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x20000)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_ESP, STACK)
        put(m, VT + 44, IO)
        put(m, VT + 48, IO)
        put(m, OBJ, VT)
        put(m, OBJ + 24, HEADER)
        return m

    for direction, mid, name, encoded in vectors:
        mid = int(mid)
        expected = bytes.fromhex(encoded)
        label = direction + str(mid)
        if label not in ranges:
            continue  # S9/S10 consume no payload; only their static paths are verified.
        m = machine()
        start, end = ranges[label]
        incoming = expected if direction == 'S' else None
        output, offset, native, adapters = bytearray(), 0, 0, 0
        if label == 'C53':
            m.reg_write(UC_X86_REG_ESI, OBJ)
            put(m, BP + 8, ARG)
            m.mem_write(ARG, expected)
        elif label == 'C54':
            m.reg_write(UC_X86_REG_ESI, struct.unpack_from('<I', expected)[0])
            put(m, BP - 0x3c, VT)
            put(m, BP - 0x24, HEADER)
            put(m, BP - 0x44, struct.unpack_from('<I', expected, 4)[0])
        elif label == 'C55':
            put(m, BP - 0x24, HEADER)
        elif label == 'S8':
            m.reg_write(UC_X86_REG_EBX, ARG)
            put(m, ARG + 12, OBJ)
        elif label == 'S11':
            m.reg_write(UC_X86_REG_ESI, OBJ)
        elif label == 'S163':
            m.mem_write(0x18e55cf, b'\0')

        def hook(uc, a, size, _):
            nonlocal offset, native, adapters
            if a == IO:
                adapters += 1
                sp = uc.reg_read(UC_X86_REG_ESP)
                p, n = get(uc, sp + 4), get(uc, sp + 8)
                assert n in (2, 4)
                if incoming is None:
                    output.extend(uc.mem_read(p, n))
                else:
                    assert offset + n <= len(incoming)
                    uc.mem_write(p, incoming[offset:offset + n])
                    offset += n
                uc.reg_write(UC_X86_REG_EIP, get(uc, sp))
                uc.reg_write(UC_X86_REG_ESP, sp + 12)
                return
            native += 1
            assert start <= a < end, hex(a)
            assert bytes(uc.mem_read(a, size)) == mapped[a - BASE:a - BASE + size]

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, end, count=500)
        assert m.reg_read(UC_X86_REG_EIP) == end
        if incoming is None:
            assert bytes(output) == expected, (label, name)
            assert get(m, HEADER + 4) & 0xffff == mid
        else:
            assert offset == len(expected)
            if label == 'S8':
                result, address, port, category = struct.unpack('<I4sHI', expected)
                assert get(m, BP - 0x40) == result
                assert bytes(m.mem_read(BP - 0x3c, 4)) == address
                assert int.from_bytes(m.mem_read(BP - 0x38, 2), 'little') == port
                assert get(m, BP - 0x34) == category
                assert bool(m.reg_read(UC_X86_REG_EFLAGS) & 0x40) == (result == 0)
            elif label == 'S11':
                accepted, category, unknown = struct.unpack('<III', expected)
                assert m.mem_read(BP - 0x24, 1)[0] == int(accepted != 0)
                assert get(m, BP - 0x20) == category
                assert get(m, BP - 0x1c) == unknown
            else:
                assert m.mem_read(0x18e55cf, 1) == b'\x01'
        runs.append(dict(label=label, name=name, start_va=hex(start), end_va=hex(end),
                         original_instructions=native, adapter_calls=adapters))

    # Execute the UI predicate separately from C54 serialization; its semantic
    # name remains unknown. The branch clears the global only for stages 11/15.
    for stage, flag, expected, after in ((3, 1, 1, 1), (2, 1, 0, 1),
                                       (11, 0, 1, 0), (11, 1, 0, 0),
                                       (15, 0, 1, 0), (15, 1, 0, 0)):
        m = machine()
        start, end = ranges['C54-ui']
        m.reg_write(UC_X86_REG_EDI, OBJ)
        m.mem_write(OBJ + 2505, b'\x01')
        put(m, 0x18e56e8, ARG)
        put(m, ARG + 48, 0x12345678)
        put(m, 0x18e6844, ARG + 0x100)
        put(m, ARG + 0x158, stage)
        m.mem_write(0x18e5686, bytes([flag]))
        executed = []

        def predicate_hook(uc, a, size, _):
            assert start <= a < end, hex(a)
            assert bytes(uc.mem_read(a, size)) == mapped[a - BASE:a - BASE + size]
            executed.append(a)

        m.hook_add(UC_HOOK_CODE, predicate_hook)
        m.emu_start(start, end, count=100)
        assert m.reg_read(UC_X86_REG_EIP) == end
        assert m.reg_read(UC_X86_REG_ESI) == 0x12345678
        assert get(m, BP - 0x44) == expected
        assert m.mem_read(0x18e5686, 1)[0] == after
        runs.append(dict(label='C54-ui', stage=stage, flag=flag, value=expected,
                         start_va=hex(start), end_va=hex(end),
                         original_instructions=len(executed), adapter_calls=0))

    report = dict(scope=__doc__, sample_sha256=digest,
                  evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
                  verified_functions=len(evidence['functions']),
                  verified_serialization_ranges=len(evidence['serialization_ranges']),
                  verified_resources=len(evidence['resources']),
                  verified_data_ranges=len(evidence['data_ranges']),
                  fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                  fixtures=len(vectors), runs=runs, passed=True)
    (ROOT / 'docs/evidence/channel_transfer_probe.json').write_text(
        json.dumps(report, indent=2) + '\n', 'utf-8')
    print(json.dumps(dict(runs=len(runs), original_instructions=sum(r['original_instructions'] for r in runs),
                         adapter_calls=sum(r['adapter_calls'] for r in runs))))


if __name__ == '__main__':
    main()
