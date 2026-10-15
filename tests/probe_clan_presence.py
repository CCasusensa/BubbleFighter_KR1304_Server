"""Verify C1045 original bounded writers; no socket or complete UI execution."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA, STACK, STOP = 0x400000, 0x20000000, 0x200e0000, 0x200ff000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    source = ROOT / 'docs/evidence/clan_presence_ida.json'
    evidence = json.loads(source.read_text('utf-8'))
    blocks = list(evidence['resources']) + list(evidence['ranges'])
    for function in evidence['functions']:
        blocks.extend(function['chunks'])
    for block in blocks:
        content = bytes.fromhex(block['bytes'])
        assert hashlib.sha256(content).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'], 16), len(content)) == content
    mapped = pe.get_memory_mapped_image()
    fixtures = ROOT / 'tests/clan_presence_packets.txt'
    rows = [line.split() for line in fixtures.read_text().splitlines()]
    runs = []
    for label, mid, plain in rows + [('disabled', '1045', '0000000000000000')]:
        payload = bytes.fromhex(plain)
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x100000)
        m.mem_map(0, 0x1000)
        m.mem_write(STACK + 8, payload)
        m.mem_write(0x18911d2, bytes([int(label != 'disabled')]))
        m.reg_write(UC_X86_REG_EBP, STACK)
        m.reg_write(UC_X86_REG_ESP, STACK - 0x10030)
        count = 0
        ends = {0x1114270, 0x527b6c}

        def hook(uc, address, size, _):
            nonlocal count
            if address in ends:
                uc.emu_stop()
                return
            assert BASE <= address < BASE + len(mapped), hex(address)
            assert bytes(uc.mem_read(address, size)) == mapped[address-BASE:address-BASE+size]
            count += 1

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(0x527ad2, STOP, count=100000)
        end = m.reg_read(UC_X86_REG_EIP)
        assert end == (0x527b6c if label == 'disabled' else 0x1114270)
        assert bytes(m.mem_read(0, 4)) != struct.pack('<I', 100)
        if label != 'disabled':
            packet, = struct.unpack('<I', m.mem_read(m.reg_read(UC_X86_REG_ESP)+4, 4))
            wire, = struct.unpack('<I', m.mem_read(packet+4, 4))
            assert bytes(m.mem_read(wire, 12)) == struct.pack('<HH', 8, int(mid)) + payload
        runs.append(dict(label=label, start_va='0x527ad2', end_va=hex(end), original_instructions=count, adapter_calls=[]))
    report = dict(passed=True, sample_sha256=digest, evidence_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256(fixtures.read_bytes()).hexdigest(), functions=len(evidence['functions']),
                  resources=len(evidence['resources']), verified_blocks=len(blocks), fixture_count=len(rows), runs=runs,
                  original_ui=False, limits='Writer starts after stack/SEH setup and retains the native connection flag gate. Stops before socket send. No client level/scene cache, roster UI or channel admission branch executed.')
    (ROOT / 'docs/evidence/clan_presence_probe.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, fixtures=len(rows), runs=len(runs), instructions=sum(r['original_instructions'] for r in runs), adapters=0, verified_blocks=len(blocks))))


if __name__ == '__main__':
    main()
