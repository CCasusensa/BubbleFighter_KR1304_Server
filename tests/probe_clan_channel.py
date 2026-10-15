"""Bounded original S8 error dispatch; no UI, sockets or server-policy inference."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]
BASE, STACK = 0x400000, 0x20000000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    path = ROOT/'docs/evidence/clan_channel_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    blocks = list(evidence['resources']) + list(evidence['ranges'])
    for function in evidence['functions']:
        blocks.extend(function['chunks'])
    for block in blocks:
        content = bytes.fromhex(block['bytes'])
        assert hashlib.sha256(content).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'], 16), len(content)) == content
    mapped = pe.get_memory_mapped_image()
    fixture = ROOT/'tests/clan_channel_packets.txt'
    fixed = {label: (int(mid), bytes.fromhex(wire)) for label, mid, wire in
             (line.split() for line in fixture.read_text().splitlines())}
    runs = []
    for label in ('requires_clan', 'too_many_mercenaries'):
        mid, payload = fixed[label]
        assert mid == 8 and len(payload) == 14
        result, = struct.unpack_from('<I', payload)
        assert struct.unpack_from('<I', payload, 10) == (80,)
        for flag in (0, 1):
            machine = Uc(UC_ARCH_X86, UC_MODE_32)
            machine.mem_map(BASE, (len(mapped)+4095) & ~4095)
            machine.mem_write(BASE, mapped)
            machine.mem_map(STACK, 0x10000)
            machine.reg_write(UC_X86_REG_ESP, STACK+0x8000)
            machine.reg_write(UC_X86_REG_EBP, STACK+0x9000)
            machine.reg_write(UC_X86_REG_EDX, result)
            instructions, adapters, selected = [], [], []

            def hook(uc, address, size, _):
                if address == 0x40daa0:
                    sp = uc.reg_read(UC_X86_REG_ESP)
                    selected.append(struct.unpack('<I', uc.mem_read(sp+4, 4))[0])
                    uc.emu_stop()
                    return
                if address == 0x10e3420:
                    assert uc.reg_read(UC_X86_REG_ECX) == 304
                    adapters.append(dict(address=hex(address), feature=304, returned=flag))
                    sp = uc.reg_read(UC_X86_REG_ESP)
                    ret, = struct.unpack('<I', uc.mem_read(sp, 4))
                    uc.reg_write(UC_X86_REG_EAX, flag)
                    uc.reg_write(UC_X86_REG_ESP, sp+4)
                    uc.reg_write(UC_X86_REG_EIP, ret)
                    return
                assert 0x945853 <= address < 0x945a00, hex(address)
                assert bytes(uc.mem_read(address, size)) == mapped[address-BASE:address-BASE+size]
                instructions.append(hex(address))

            machine.hook_add(UC_HOOK_CODE, hook)
            machine.emu_start(0x945853, 0, count=200)
            expected = 0x15ac600 if result == 3 else (0x15ac724 if flag else 0x15ac75c)
            assert selected == [expected]
            assert len(adapters) == (1 if result == 4 else 0)
            runs.append(dict(label=label, result=result, feature304=flag,
                             resource=hex(expected), original_instructions=len(instructions),
                             trace=instructions, adapter_calls=adapters))
    report = dict(passed=True, sample_sha256=digest,
                  evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),
                  functions=len(evidence['functions']), resources=len(evidence['resources']),
                  verified_blocks=len(blocks), runs=runs, original_ui=False,
                  limits='Starts after S8 error-mapper prologue/SEH. Stops before string construction. Only feature304 lookup is adapted. No full handler/UI, clan-battle readiness, network or original-server policy executed.')
    (ROOT/'docs/evidence/clan_channel_probe.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, runs=len(runs), instructions=sum(r['original_instructions'] for r in runs),
                         adapters=sum(len(r['adapter_calls']) for r in runs), verified_blocks=len(blocks))))


if __name__ == '__main__':
    main()
