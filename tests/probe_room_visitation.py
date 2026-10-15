"""Bounded BFCLIENT02 visitation serialization, with packet I/O/string adapters.
Original x86 instructions execute offline; no UI, network or executable edits.
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
VT, IO, HEADER, OBJ, ARG, OUT = [DATA + n for n in (0x100, 0x200, 0x300, 0x400, 0x1000, 0x2000)]

def put(m, a, *v):
    m.mem_write(a, struct.pack('<' + 'I' * len(v), *v))

def get(m, a):
    return struct.unpack('<I', m.mem_read(a, 4))[0]

def string(m, a, s):
    b = s.encode('utf-16-le')
    n = len(b) // 2
    m.mem_write(a, b'\0' * 24)
    if n <= 7:
        m.mem_write(a, b + b'\0\0')
    else:
        put(m, a, DATA + 0x8000)
        m.mem_write(DATA + 0x8000, b + b'\0\0')
    put(m, a + 16, n, max(7, n))

def read_string(m, a):
    p = get(m, a) if get(m, a + 20) > 7 else a
    return bytes(m.mem_read(p, get(m, a + 16) * 2)).decode('utf-16-le')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sample', required=True, type=Path)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    mapped = pe.get_memory_mapped_image()
    evidence = json.loads((ROOT / 'docs/evidence/room_visitation_native.json').read_text('utf-8-sig'))
    for f in evidence['functions']:
        b = bytes.fromhex(f['bytes'])
        assert pe.get_data(int(f['rva'], 16), len(b)) == b
    for r in evidence['serialization_ranges']:
        b = bytes.fromhex(r['bytes'])
        assert pe.get_data(int(r['rva'], 16), len(b)) == b
    fixture = ROOT / 'tests/room_visitation_packets.txt'
    vectors = [line.split('|') for line in fixture.read_text('utf-8').splitlines() if line and not line.startswith('#')]
    runs = []
    for mid, text, encoded in vectors:
        mid = int(mid)
        expected = bytes.fromhex(encoded)
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x20000)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_ESP, STACK)
        m.reg_write(UC_X86_REG_EBX, ARG)
        put(m, VT + 44, IO)
        put(m, VT + 48, IO)
        put(m, OBJ, VT)
        put(m, OBJ + 24, HEADER)
        put(m, ARG + 8, OBJ, OBJ)
        incoming = expected if mid >= 123 else None
        output, offset, native, adapters = bytearray(), 0, 0, 0
        if mid == 112:
            start, end = 0x9398cd, 0x93990a
            put(m, BP - 0x58, VT)
            put(m, BP - 0x40, HEADER)
            string(m, BP - 0x2c, text)
        elif mid == 113:
            start, end = 0x9d7832, 0x9d7867
            m.reg_write(UC_X86_REG_EDI, OBJ)
            put(m, BP + 8, OUT)
            put(m, OUT, 0x12345678)
            m.mem_write(OUT + 4, expected[4:5])
        elif mid == 114:
            start, end = 0x9d78e1, 0x9d7902
            m.reg_write(UC_X86_REG_ESI, OBJ)
            put(m, BP + 8, OUT)
            put(m, OUT, 0x12345678)
        elif mid == 115:
            start, end = 0x940582, 0x9405f6
            m.reg_write(UC_X86_REG_ESI, OBJ)
            put(m, BP + 8, OUT)
            string(m, OUT, text)
            m.mem_write(OUT + 24, bytes.fromhex('04030201bc9a'))
            m.mem_write(OUT + 32, b'\x00')
        else:
            start, end, dest = {123: (0x9aa6f7, 0x9aa7c7, BP-0x7c),
                                124: (0x9ab968, 0x9aba4d, BP-0x58),
                                125: (0x9a9785, 0x9a9859, BP-0x60)}[mid]
            string(m, dest, '')
        def ret(uc, pop=0):
            sp = uc.reg_read(UC_X86_REG_ESP)
            uc.reg_write(UC_X86_REG_EIP, get(uc, sp))
            uc.reg_write(UC_X86_REG_ESP, sp + 4 + pop)
        def hook(uc, a, size, _):
            nonlocal offset, native, adapters
            sp = uc.reg_read(UC_X86_REG_ESP)
            if a == IO:
                adapters += 1
                p, n = get(uc, sp + 4), get(uc, sp + 8)
                if incoming is None:
                    output.extend(uc.mem_read(p, n))
                else:
                    assert offset + n <= len(incoming)
                    uc.mem_write(p, incoming[offset:offset+n])
                    offset += n
                ret(uc, 8)
                return
            if a == 0x1119c10:
                adapters += 1
                n = struct.unpack_from('<I', incoming, offset)[0]
                offset += 4
                s = incoming[offset:offset+2*n].decode('utf-16-le')
                offset += 2*n
                p = uc.reg_read(UC_X86_REG_ECX)
                string(uc, p, s)
                uc.reg_write(UC_X86_REG_EAX, p)
                ret(uc)
                return
            native += 1
            assert start <= a < end, hex(a)
            assert bytes(uc.mem_read(a, size)) == mapped[a-BASE:a-BASE+size]
        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, end, count=1000)
        assert m.reg_read(UC_X86_REG_EIP) == end
        if incoming is None:
            assert bytes(output) == expected, (mid, bytes(output).hex(), expected.hex())
            assert get(m, HEADER + 4) & 0xffff == mid
        else:
            assert offset == len(expected)
            assert read_string(m, dest) == text
            if mid == 124:
                assert get(m, BP - 0x5c) == 0x12345678
                assert m.mem_read(BP - 0x40, 1)[0] == int(struct.unpack('<I', expected[-4:])[0] != 0)
            elif mid == 125:
                assert get(m, BP - 0x48) == 0x01020304
                assert bytes(m.mem_read(BP - 0x44, 2)) == bytes.fromhex('bc9a')
            else:
                assert get(m, BP - 0x64) == 28
        runs.append(dict(message=mid, start_va=hex(start), end_va=hex(end), original_instructions=native, adapter_calls=adapters))
    report = dict(scope=__doc__, sample_sha256=digest, verified_functions=len(evidence['functions']), verified_serialization_ranges=len(evidence['serialization_ranges']), fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(), runs=runs)
    (ROOT / 'docs/evidence/room_visitation_probe.json').write_text(json.dumps(report, indent=2)+'\n', 'utf-8')
    print(json.dumps(dict(runs=len(runs), original_instructions=sum(r['original_instructions'] for r in runs), adapter_calls=sum(r['adapter_calls'] for r in runs))))

if __name__ == '__main__':
    main()
