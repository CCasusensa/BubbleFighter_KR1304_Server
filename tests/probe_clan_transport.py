"""Verify BFCLIENT02 clan bytes and bounded native writers/cipher, without sockets."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x20000000
BP, STACK, OBJECT, HEAP, STOP = [DATA + n for n in (0x30000, 0x40000, 0x50000, 0x70000, 0x7f000)]


def put(m, address, value):
    m.mem_write(address, struct.pack('<I', value))


def get(m, address):
    return struct.unpack('<I', m.mem_read(address, 4))[0]


def text_object(m, address, value, heap):
    raw = value.encode('utf-16le') + b'\0\0'
    count = len(raw) // 2 - 1
    if count <= 7:
        m.mem_write(address, raw)
    else:
        m.mem_write(heap, raw)
        put(m, address, heap)
    put(m, address + 16, count)
    put(m, address + 20, max(7, count))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    path = ROOT / 'docs/evidence/clan_transport_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    for key in ('functions', 'data_ranges', 'resources'):
        for row in evidence[key]:
            blob = bytes.fromhex(row['bytes'])
            assert pe.get_data(int(row['rva'], 16), len(blob)) == blob, (key, row['va'])
    mapped = pe.get_memory_mapped_image()
    runs = []

    def machine():
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x80000)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_ESP, STACK)
        return m

    def execute(m, label, start, end, copy=False):
        steps, calls = 0, []

        def hook(uc, address, size, unused):
            nonlocal steps
            if copy and address in (0x1296140, 0x12966c0):
                sp = uc.reg_read(UC_X86_REG_ESP)
                target, source, count = (get(uc, sp + n) for n in (4, 8, 12))
                assert DATA <= target and target + count <= DATA + 0x80000
                value = bytes(uc.mem_read(source, count)) if address == 0x1296140 else bytes([source & 255]) * count
                uc.mem_write(target, value)
                uc.reg_write(UC_X86_REG_EAX, target)
                uc.reg_write(UC_X86_REG_EIP, get(uc, sp))
                uc.reg_write(UC_X86_REG_ESP, sp + 4)
                calls.append({'va': hex(address), 'kind': 'memcpy' if address == 0x1296140 else 'memset', 'size': count})
                return
            assert BASE <= address < BASE + len(mapped), hex(address)
            assert bytes(uc.mem_read(address, size)) == mapped[address - BASE:address - BASE + size]
            steps += 1

        handle = m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, end, count=1500000)
        m.hook_del(handle)
        assert m.reg_read(UC_X86_REG_EIP) == end, (label, hex(m.reg_read(UC_X86_REG_EIP)))
        runs.append(dict(label=label, start_va=hex(start), end_va=hex(end), original_instructions=steps, adapter_calls=calls))

    fixtures = {}
    for line in (ROOT / 'tests/clan_packets.txt').read_text().splitlines():
        label, message_id, payload, frame = line.split()
        fixtures[label] = (int(message_id), bytes.fromhex(payload) if payload != '-' else b'', bytes.fromhex(frame))

    for label in ('create_inline', 'create_heap', 'login_inline', 'login_heap'):
        m = machine()
        if label.startswith('create'):
            name, intro = ('Alpha', '联机队伍') if label.endswith('inline') else ('ABCDEFGHIJKL', '原版独立战队协议测试说明')
            text_object(m, BP + 8, name, HEAP)
            put(m, BP + 0x20, 0x01020304)
            text_object(m, BP + 0x24, intro, HEAP + 0x1000)
            put(m, BP + 0x3c, 0x11223344)
            put(m, BP + 0x40, 0xa1b2c3d4)
            m.reg_write(UC_X86_REG_ESP, BP - 0x1002c)
            execute(m, label, 0x5275d2, 0x527723, copy=True)
            packet = BP - 0x1002c
        else:
            text = '汉A' if label.endswith('inline') else 'ABCDEFGHIJKL'
            for offset, value in [(8, 0x12345678), (12, 0x11223344), (16, 0xa1b2c3d4), (20, HEAP + 0x2000)]:
                put(m, BP + offset, value)
            text_object(m, BP + 24, text, HEAP)
            proof = b'\0\xff\x10\x3a\0'
            put(m, HEAP + 0x2000, HEAP + 0x3000)
            put(m, HEAP + 0x2004, HEAP + 0x3000 + len(proof))
            m.mem_write(HEAP + 0x3000, proof)
            m.reg_write(UC_X86_REG_ESP, BP - 0x10030)
            execute(m, label, 0x525e12, 0x525fb9, copy=True)
            packet = BP - 0x10030
        header = get(m, packet + 4)
        size, message_id = struct.unpack('<HH', m.mem_read(header, 4))
        assert (message_id, bytes(m.mem_read(header + 4, size))) == fixtures[label][:2]
        m.reg_write(UC_X86_REG_ECX, packet)
        m.reg_write(UC_X86_REG_ESP, STACK)
        put(m, STACK, STOP)
        execute(m, label + '_cipher', 0x1114a80, STOP)
        assert bytes(m.mem_read(header, size + 4)) == fixtures[label][2]

    # Every reachable table index, including modulo-255 boundary cases, is
    # compared to a fixed fixture created before production implementation.
    for label, (message_id, payload, frame) in fixtures.items():
        if label in ('create_inline', 'create_heap', 'login_inline', 'login_heap'):
            continue
        m = machine()
        put(m, OBJECT + 4, HEAP)
        put(m, OBJECT + 8, HEAP + 2)
        put(m, OBJECT + 65548, HEAP + 4)
        m.mem_write(HEAP, struct.pack('<HH', len(payload), message_id) + payload)
        for direction in ('encode', 'decode'):
            m.reg_write(UC_X86_REG_ECX, OBJECT)
            m.reg_write(UC_X86_REG_ESP, STACK)
            put(m, STACK, STOP)
            execute(m, label + '_' + direction, 0x1114a80, STOP)
            expected = frame if direction == 'encode' else struct.pack('<HH', len(payload), message_id) + payload
            assert bytes(m.mem_read(HEAP, len(expected))) == expected, label

    result = dict(passed=True, sample_sha256=digest, evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  fixtures_sha256=hashlib.sha256((ROOT / 'tests/clan_packets.txt').read_bytes()).hexdigest(),
                  verified_functions=len(evidence['functions']), verified_data_ranges=len(evidence['data_ranges']),
                  verified_resources=len(evidence['resources']), fixture_count=len(fixtures), runs=runs,
                  original_ui=False, socket_execution=False, full_handlers=False,
                  limits='Writer bodies start after their SEH/stack prologues and stop before send. CRT memset/memcpy are adapted; original string loops and all scalar stores run. Cipher executes the complete native function. No authentication acceptance, clan persistence or original UI is claimed.')
    (ROOT / 'docs/evidence/clan_transport_probe.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'runs'}))
    print(len(runs), 'runs;', sum(r['original_instructions'] for r in runs), 'native instructions;', sum(len(r['adapter_calls']) for r in runs), 'adapted calls')


if __name__ == '__main__':
    main()
