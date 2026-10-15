"""Execute bounded original party serialization; UI and network remain inactive."""
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


def put(m, address, *values):
    m.mem_write(address, struct.pack('<' + 'I' * len(values), *values))


def get(m, address):
    return struct.unpack('<I', m.mem_read(address, 4))[0]


def read_string(m, address):
    start = get(m, address) if get(m, address + 20) > 7 else address
    return bytes(m.mem_read(start, get(m, address + 16) * 2)).decode('utf-16-le')


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
    evidence = json.loads((ROOT / 'docs/evidence/party_operations_native.json').read_text('utf-8'))
    for item in evidence['functions'] + evidence['serialization_ranges']:
        expected = bytes.fromhex(item['bytes'])
        assert pe.get_data(int(item['rva'], 16), len(expected)) == expected
    ranges = {r['messages']: (int(r['start_va'], 16), int(r['end_va'], 16))
              for r in evidence['serialization_ranges']}
    fixture = ROOT / 'tests/party_packets.txt'
    vectors = [line.split('|') for line in fixture.read_text('utf-8').splitlines()
               if line and not line.startswith('#')]
    runs, static_empty = [], 0
    for kind, first, second, number, encoded in vectors:
        expected, number = bytes.fromhex(encoded), int(number)
        if kind == 's204':
            assert expected == b''
            static_empty += 1
            continue
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
        heap = DATA + 0x5000

        def string(address, value):
            nonlocal heap
            raw = value.encode('utf-16-le')
            n = len(raw) // 2
            m.mem_write(address, bytes(24))
            if n <= 7:
                m.mem_write(address, raw + bytes(2))
            else:
                put(m, address, heap)
                m.mem_write(heap, raw + bytes(2))
                heap += (len(raw) + 31) & ~15
                assert heap < DATA + 0x9000
            put(m, address + 16, n, max(7, n))

        start, end = ranges['s203_s307' if kind in ('s203', 's307') else kind]
        destination = None
        if kind == 'c202':
            m.reg_write(UC_X86_REG_ESI, OBJ)
        elif kind in ('c302', 'c303'):
            put(m, BP - 0x54, VT)
            put(m, BP - 0x3c, HEADER)
            string(BP - 0x28, first)
        elif kind in ('s203', 's303', 's307'):
            destination = OUT
            m.reg_write(UC_X86_REG_ESI, OUT)
            m.reg_write(UC_X86_REG_EDI, OUT + 24)
            string(OUT, '')
            string(OUT + 24, '')
        else:
            destination = {'s205': BP-0x4c, 's206': BP-0x4c,
                           's302': BP-0x90, 's304': BP-0x30}[kind]
            string(destination, '')
        incoming = expected if kind.startswith('s') else None
        output, offset, instructions, adapters = bytearray(), 0, 0, 0

        def ret(uc, pop=0):
            sp = uc.reg_read(UC_X86_REG_ESP)
            uc.reg_write(UC_X86_REG_EIP, get(uc, sp))
            uc.reg_write(UC_X86_REG_ESP, sp + 4 + pop)

        def hook(uc, address, size, _):
            nonlocal offset, instructions, adapters
            sp = uc.reg_read(UC_X86_REG_ESP)
            if address == IO:
                adapters += 1
                target, length = get(uc, sp + 4), get(uc, sp + 8)
                if incoming is None:
                    output.extend(uc.mem_read(target, length))
                else:
                    assert offset + length <= len(incoming)
                    uc.mem_write(target, incoming[offset:offset+length])
                    offset += length
                ret(uc, 8)
                return
            if address == 0x1119c10:
                adapters += 1
                length = struct.unpack_from('<I', incoming, offset)[0]
                offset += 4
                value = incoming[offset:offset+2*length].decode('utf-16-le')
                offset += 2*length
                target = uc.reg_read(UC_X86_REG_ECX)
                string(target, value)
                uc.reg_write(UC_X86_REG_EAX, target)
                ret(uc)
                return
            instructions += 1
            assert start <= address < end, hex(address)
            assert bytes(uc.mem_read(address, size)) == mapped[address-BASE:address-BASE+size]

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, end, count=2000)
        assert m.reg_read(UC_X86_REG_EIP) == end
        if incoming is None:
            assert bytes(output) == expected, (kind, bytes(output).hex(), encoded)
            assert get(m, HEADER + 4) & 0xffff == int(kind[1:])
        else:
            assert offset == len(expected), (kind, offset, len(expected))
            assert read_string(m, destination) == first
            if kind in ('s203', 's303', 's307'):
                assert read_string(m, destination + 24) == second
            if kind == 's303':
                assert get(m, OUT + 48) == number
            if kind == 's302':
                assert get(m, BP - 0x94) == number
        runs.append(dict(message=kind, start_va=hex(start), end_va=hex(end),
                         original_instructions=instructions, adapter_calls=adapters))
    report = dict(scope=__doc__, sample_sha256=digest,
                  verified_functions=len(evidence['functions']),
                  verified_serialization_ranges=len(evidence['serialization_ranges']),
                  fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                  static_empty_fixtures=static_empty, runs=runs)
    (ROOT / 'docs/evidence/party_operations_probe.json').write_text(json.dumps(report, indent=2)+'\n', 'utf-8')
    print(json.dumps(dict(runs=len(runs), static_empty_fixtures=static_empty,
                          original_instructions=sum(r['original_instructions'] for r in runs),
                          adapter_calls=sum(r['adapter_calls'] for r in runs))))


if __name__ == '__main__':
    main()
