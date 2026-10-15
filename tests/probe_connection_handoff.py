"""Bounded original pointer/readiness branches; no sockets, full UI or authentication."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_EDI

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x20000000
OLD, NEW, PRIOR, BP, STACK = [DATA + n for n in (0x1000, 0x3000, 0x5000, 0xa000, 0xf000)]


def put(m, address, value):
    m.mem_write(address, struct.pack('<I', value))


def get(m, address):
    return struct.unpack('<I', m.mem_read(address, 4))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', required=True, type=Path)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    evidence_path = ROOT / 'docs/evidence/connection_handoff_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    for row in evidence['functions']:
        expected = bytes.fromhex(row['bytes'])
        assert pe.get_data(int(row['rva'], 16), len(expected)) == expected
    profiles = json.loads((ROOT / 'tools/client_runtime/profiles.json').read_text('utf-8'))
    for point in profiles['Handoff']['Points']:
        expected = bytes.fromhex(point['Bytes'])
        assert pe.get_data(point['Va'] - BASE, len(expected)) == expected
    mapped = pe.get_memory_mapped_image()
    runs = []

    def machine():
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x20000)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_ESP, STACK)
        for obj in (OLD, NEW, PRIOR):
            put(m, obj + 4, 2)  # Live retained objects; no destructor branch adapted.
        return m

    def execute(m, label, start, end, adapters=None):
        adapters = adapters or {}
        count, calls = 0, []

        def hook(uc, address, size, unused):
            nonlocal count
            if address in adapters:
                calls.append({'va': hex(address), 'scope': adapters[address],
                              'receiver': hex(uc.reg_read(UC_X86_REG_ECX))})
                stack = uc.reg_read(UC_X86_REG_ESP)
                uc.reg_write(UC_X86_REG_EIP, get(uc, stack))
                uc.reg_write(UC_X86_REG_ESP, stack + 4)
            else:
                assert BASE <= address < BASE + len(mapped)
                count += 1

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, end, count=300)
        assert m.reg_read(UC_X86_REG_EIP) == end
        runs.append({'label': label, 'start_va': hex(start), 'end_va': hex(end),
                     'original_instructions': count, 'adapter_calls': calls})

    for saved in (0, PRIOR, OLD):
        m = machine()
        put(m, 0x18e7e9c, saved)
        put(m, 0x18e7f48, OLD)
        put(m, 0x18e7f4c, NEW)
        execute(m, 'S9 promote, saved=' + hex(saved), 0x9a6bbb, 0x9a6c51)
        assert get(m, 0x18e7e9c) == OLD
        assert get(m, 0x18e7f48) == NEW and get(m, 0x18e7f4c) == 0
        assert get(m, NEW + 4) == 2

    for saved in (0, OLD):
        m = machine()
        put(m, 0x18e7e9c, saved)
        put(m, 0x18e7f48, NEW)
        m.mem_write(NEW + 2504, b'\x01')
        m.reg_write(UC_X86_REG_ESI, NEW)
        execute(m, 'S10 retire, saved=' + hex(saved), 0x9d3503, 0x9d3c73,
                {0x10ba630: 'socket close skipped', 0x9389c0: 'UI refresh skipped',
                 0x99e860: 'configuration request skipped'})
        assert get(m, 0x18e7e9c) == 0 and get(m, 0x18e7f48) == NEW
        assert bytes(m.mem_read(NEW + 2504, 1)) == b'\x00'
        closes = [c for c in runs[-1]['adapter_calls'] if c['va'] == '0x10ba630']
        assert len(closes) == bool(saved)
        if saved:
            assert closes[0]['receiver'] == hex(OLD)
            assert bytes(m.mem_read(OLD + 325, 1)) == b'\x01'

    for pending in (0, NEW):
        m = machine()
        put(m, 0x18e7f4c, pending)
        put(m, 0x18e7f48, NEW if not pending else OLD)
        put(m, BP - 0x20, 60)
        put(m, BP - 0x1c, 0)
        m.reg_write(UC_X86_REG_EDI, NEW)
        execute(m, 'S11 accepted stores, pending=' + hex(pending), 0x9a700b, 0x9a704a)
        assert get(m, NEW + 2508) == 60 and get(m, NEW + 2512) == 0
        assert bytes(m.mem_read(NEW + 2504, 2)) == b'\x01\x00'
    result = dict(passed=True, sample_sha256=digest, image_base=hex(BASE),
                  evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
                  verified_functions=len(evidence['functions']),
                  verified_adapter_points=len(profiles['Handoff']['Points']), runs=runs,
                  full_handlers=False, original_ui=False, original_server_order_proven=False,
                  limits='S9 pointer branch and S11 accepted stores only. S10 close/UI/config calls adapted. C55 serialization is covered by prior channel probe. Companion authentication is project policy.')
    (ROOT / 'docs/evidence/connection_handoff_probe.json').write_text(
        json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'runs'}))
    print(f'{len(runs)} bounded runs, {sum(r["original_instructions"] for r in runs)} original instructions')


if __name__ == '__main__':
    main()
