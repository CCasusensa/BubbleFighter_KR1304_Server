"""Compare fixed clan profiles with bounded BFCLIENT02 readers and role branches."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x20000000
BP, STACK, OBJECT, HEAP, STOP = [DATA + n for n in (0x30000, 0x40000, 0x50000, 0x70000, 0xff000)]


def put(m, address, value):
    m.mem_write(address, struct.pack('<I', value))


def get(m, address):
    return struct.unpack('<I', m.mem_read(address, 4))[0]


def text_value(m, address):
    count = get(m, address + 16)
    pointer = get(m, address) if get(m, address + 20) > 7 else address
    return bytes(m.mem_read(pointer, count * 2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    path = ROOT / 'docs/evidence/clan_profile_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    for key in ('functions', 'data_ranges', 'resources'):
        for row in evidence[key]:
            blob = bytes.fromhex(row['bytes'])
            assert hashlib.sha256(blob).hexdigest() == row['sha256']
            assert pe.get_data(int(row['rva'], 16), len(blob)) == blob, (key, row['va'])
    mapped = pe.get_memory_mapped_image()
    runs = []

    def machine(payload=b'', mid=0):
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x100000)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_EBX, BP + 0x100)
        m.reg_write(UC_X86_REG_ESP, BP - 0x100)
        put(m, BP + 0x108, OBJECT)
        put(m, OBJECT + 4, HEAP)
        put(m, OBJECT + 65548, HEAP + 4)
        put(m, OBJECT + 65552, HEAP + 4)
        put(m, OBJECT + 65560, HEAP + 4 + len(payload))
        m.mem_write(HEAP, struct.pack('<HH', len(payload), mid) + payload)
        return m

    def execute(m, label, start, ends, adapters=False):
        if isinstance(ends, int):
            ends = {ends}
        steps, calls, next_heap = 0, [], HEAP + 0x20000

        def hook(uc, address, size, unused):
            nonlocal steps, next_heap
            if address in ends:
                uc.emu_stop()
                return
            if adapters and address in (0x1296140, 0x12966c0, 0x40c2e0):
                sp = uc.reg_read(UC_X86_REG_ESP)
                if address == 0x40c2e0:
                    target = uc.reg_read(UC_X86_REG_ECX)
                    source, count = get(uc, sp + 4), get(uc, sp + 8)
                    blob = bytes(uc.mem_read(source, count * 2)) + b'\0\0'
                    if count > 7:
                        assert next_heap + len(blob) < STOP
                        uc.mem_write(next_heap, blob)
                        put(uc, target, next_heap)
                        next_heap += len(blob) + 16
                    else:
                        uc.mem_write(target, blob)
                    put(uc, target + 16, count)
                    put(uc, target + 20, max(7, count))
                    pop, kind = 12, 'wstring assignment only; wire scan/read remains native'
                else:
                    target, source, count = (get(uc, sp + n) for n in (4, 8, 12))
                    assert DATA <= target and target + count <= DATA + 0x100000
                    blob = bytes(uc.mem_read(source, count)) if address == 0x1296140 else bytes([source & 255]) * count
                    uc.mem_write(target, blob)
                    pop, kind = 4, 'memcpy' if address == 0x1296140 else 'memset'
                uc.reg_write(UC_X86_REG_EAX, target)
                uc.reg_write(UC_X86_REG_EIP, get(uc, sp))
                uc.reg_write(UC_X86_REG_ESP, sp + pop)
                calls.append({'va': hex(address), 'kind': kind, 'count': count})
                return
            assert BASE <= address < BASE + len(mapped), hex(address)
            assert bytes(uc.mem_read(address, size)) == mapped[address - BASE:address - BASE + size]
            steps += 1

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, STOP, count=2000000)
        end = m.reg_read(UC_X86_REG_EIP)
        assert end in ends, (label, hex(end))
        runs.append(dict(label=label, start_va=hex(start), end_va=hex(end), original_instructions=steps, adapter_calls=calls))

    fixtures = {}
    fixture_path = ROOT / 'tests/clan_profile_packets.txt'
    for line in fixture_path.read_text().splitlines():
        label, mid, payload, frame = line.split()
        fixtures[label] = int(mid), bytes.fromhex(payload) if payload != '-' else b''

    for label, (mid, payload) in fixtures.items():
        m = machine(payload, mid)
        if mid == 1017:
            # Match this body's 64 KiB packet stack frame; its constructor
            # must not clear the harness return-address/call argument area.
            m.reg_write(UC_X86_REG_ESP, BP - 0x10048)
            execute(m, label, 0x526fbb, 0x526fdb, True)
            packet = get(m, m.reg_read(UC_X86_REG_ESP))
            assert bytes(m.mem_read(get(m, packet + 4), 4)) == struct.pack('<HH', 0, 1017)
            continue
        # Independent cursor consumes the already-frozen sample. No output or
        # expected bytes are regenerated from the production implementation.
        cursor = 0

        def take(n):
            nonlocal cursor
            value = payload[cursor:cursor + n]
            assert len(value) == n
            cursor += n
            return value

        def scalar():
            return int.from_bytes(take(4), 'little')

        def string():
            value = b''
            while (unit := take(2)) != b'\0\0':
                value += unit
            return value

        if mid == 1016:
            flag, clan, status = take(1), scalar(), scalar()
            name, date = string(), take(8)
            execute(m, label, 0x526363, 0x5264ab, True)
            sp = m.reg_read(UC_X86_REG_ESP)
            assert [get(m, sp), get(m, sp + 4)] == [clan, status]
            assert text_value(m, get(m, sp + 8)) == name
            assert bytes(m.mem_read(BP - 0x28, 8)) == date
            assert bytes(m.mem_read(BP - 0x15, 1)) == flag
        else:
            name, v124, t76, v128, t100 = string(), scalar(), string(), scalar(), string()
            v132, v136, v144, v152 = scalar(), scalar(), take(8), take(8)
            v160, v164, v172 = scalar(), scalar(), scalar()
            t176, t200, mark, v168 = string(), string(), scalar(), scalar()
            execute(m, label, 0x5268d2, 0x526c6d, True)
            sp = m.reg_read(UC_X86_REG_ESP)
            for offset, expected in [(0, name), (8, t76), (16, t100), (48, t176), (52, t200)]:
                assert text_value(m, get(m, sp + offset)) == expected, (label, offset)
            for offset, expected in [(4, v124), (12, v128), (20, v132), (24, v136), (28, v160), (32, v164), (36, v168), (40, v172), (44, mark)]:
                assert get(m, sp + offset) == expected, (label, offset)
            assert bytes(m.mem_read(sp + 56, 16)) == v144 + v152
        assert cursor == len(payload)
        assert get(m, OBJECT + 65552) == HEAP + 4 + len(payload)

    # Real member-status label selection and native officer predicate, with
    # invalid/high-bit values. Stop before localized-string/UI calls.
    for status in (0, 1, 2, 99, 100, 101, 102, 103, 0xffffffff, 0x80000000):
        m = machine()
        put(m, BP + 8, OBJECT)
        put(m, BP + 12, status)
        execute(m, f'role_label_{status}', 0x51aa2b, {0x51aa56, 0x51aae1, 0x51ab27, 0x51ab6d, 0x51abcb})
        assert get(m, m.reg_read(UC_X86_REG_ESP)) == {1: 0x1563f84, 100: 0x1563f94, 101: 0x1563f78, 102: 0x1563f68}.get(status, 0x154a904)
        m = machine()
        put(m, 0x18e5564, OBJECT)
        put(m, OBJECT + 48, status)
        m.reg_write(UC_X86_REG_ESP, STACK)
        put(m, STACK, STOP)
        execute(m, f'officer_predicate_{status}', 0x517e50, STOP)
        assert m.reg_read(UC_X86_REG_EAX) & 255 == int(status in (101, 102))

    result = dict(passed=True, sample_sha256=digest, evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  fixtures_sha256=hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
                  verified_functions=len(evidence['functions']), verified_data_ranges=len(evidence['data_ranges']),
                  verified_resources=len(evidence['resources']), fixture_count=len(fixtures), runs=runs,
                  full_handlers=False, original_ui=False, socket_execution=False,
                  limits='S1016/S1018 bodies stop before applying state. Original scalar reads, UTF-16Z scans, bounds/cursor updates and stack probing execute. CRT copy/clear and std::wstring assignment are adapted. C1017 body stops before send; role selection stops before localization. Native officer predicate executes completely. No original server authority, authentication or UI acceptance.')
    (ROOT / 'docs/evidence/clan_profile_probe.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'runs'}))
    print(len(runs), 'runs;', sum(r['original_instructions'] for r in runs), 'native instructions;', sum(len(r['adapter_calls']) for r in runs), 'adapted calls')


if __name__ == '__main__':
    main()
