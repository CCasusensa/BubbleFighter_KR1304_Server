"""Execute native Note dispatch including its empty callbacks; no original UI."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_ESI

ROOT = Path(__file__).resolve().parents[1]
BASE, ARENA, STOP = 0x400000, 0x20000000, 0x30000000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    path = ROOT/'docs/evidence/offline_messages_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    blocks = evidence['ranges'] + evidence['resources']
    for function in evidence['functions']:
        blocks.extend(function['chunks'])
    for block in blocks:
        data = bytes.fromhex(block['bytes'])
        assert hashlib.sha256(data).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'], 16), len(data)) == data
    mapped = pe.get_memory_mapped_image()
    cases_path = ROOT/'tests/offline_message_native_cases.json'
    cases = json.loads(cases_path.read_text('utf-8'))['cases']
    runs = []
    for case in cases:
        machine = Uc(UC_ARCH_X86, UC_MODE_32)
        machine.mem_map(BASE, (len(mapped)+4095) & ~4095)
        machine.mem_write(BASE, mapped)
        machine.mem_map(ARENA, 0x20000)
        machine.mem_map(STOP, 0x1000)
        owner, event, stack = ARENA+0x1000, ARENA+0x2000, ARENA+0x18000
        machine.mem_write(owner, struct.pack('<I', 0x160ea30))
        before = bytearray(b'\xa5'*8000)
        struct.pack_into('<II', before, 12, case['event_id'], 0)
        machine.mem_write(event, bytes(before))
        machine.mem_write(stack, struct.pack('<II', STOP, event))
        machine.reg_write(UC_X86_REG_ECX, owner)
        machine.reg_write(UC_X86_REG_ESP, stack)
        machine.reg_write(UC_X86_REG_ESI, 0x12345678)
        trace, callbacks, stopped = [], [], []

        def hook(uc, address, size, _):
            if address == STOP:
                stopped.append(True)
                uc.emu_stop()
                return
            assert 0xf8f6d0 <= address < 0xf8fab8 or address == 0x44e030, hex(address)
            assert bytes(uc.mem_read(address, size)) == mapped[address-BASE:address-BASE+size]
            trace.append(hex(address))
            if address == 0x44e030:
                callbacks.append(address)
                sp = uc.reg_read(UC_X86_REG_ESP)
                assert struct.unpack('<I', uc.mem_read(sp+4, 4))[0] == event

        machine.hook_add(UC_HOOK_CODE, hook)
        machine.emu_start(0xf8f6d0, 0, count=100)
        assert stopped == [True]
        assert callbacks == ([] if case['callback'] is None else [case['callback']])
        struct.pack_into('<I', before, 16, case['handled'])
        assert bytes(machine.mem_read(event, len(before))) == bytes(before)
        assert machine.reg_read(UC_X86_REG_ESP) == stack+8
        assert machine.reg_read(UC_X86_REG_ESI) == 0x12345678
        runs.append(dict(**case, original_instructions=len(trace), trace=trace, adapter_calls=[]))
    report = dict(passed=True, sample_sha256=digest,
                  evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  cases_sha256=hashlib.sha256(cases_path.read_bytes()).hexdigest(),
                  verified_blocks=len(blocks), runs=runs, original_ui=False,
                  limits='Original dispatcher and no-op callbacks execute without adapters. Event bodies are synthetic; the SDK envelope/decoder, platform, UI and actual delivery do not execute. Handled flag is not evidence of human read or visible receipt.')
    (ROOT/'docs/evidence/offline_messages_probe.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, blocks=len(blocks), runs=len(runs),
                         instructions=sum(x['original_instructions'] for x in runs), adapters=0)))


if __name__ == '__main__':
    main()
