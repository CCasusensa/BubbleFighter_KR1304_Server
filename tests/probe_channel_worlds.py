"""Verify native channel names and execute only the UI-index/category mapping."""
import argparse
import hashlib
import json
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x400000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', required=True, type=Path)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    evidence_path = ROOT / 'docs/evidence/channel_worlds_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    for key in ('functions', 'resources', 'data_ranges'):
        for row in evidence[key]:
            expected = bytes.fromhex(row['bytes'])
            assert pe.get_data(int(row['rva'], 16), len(expected)) == expected, (key, row['va'])
    mapped = pe.get_memory_mapped_image()
    runs = []
    for index, category in [(0, 60), (1, 70), (2, 80), (3, 20), (4, 60), (0xffffffff, 60)]:
        machine = Uc(UC_ARCH_X86, UC_MODE_32)
        machine.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        machine.mem_write(BASE, mapped)
        machine.reg_write(UC_X86_REG_EAX, index)
        count = 0

        def hook(uc, address, size, _):
            nonlocal count
            assert 0x56f6ad <= address < 0x56f6d3
            assert bytes(uc.mem_read(address, size)) == mapped[address - BASE:address - BASE + size]
            count += 1

        machine.hook_add(UC_HOOK_CODE, hook)
        machine.emu_start(0x56f6ad, 0x56f6d3, count=32)
        assert machine.reg_read(UC_X86_REG_EIP) == 0x56f6d3
        assert machine.reg_read(UC_X86_REG_EAX) == category
        runs.append(dict(ui_index=index, category=category, original_instructions=count,
                         adapted_calls=0, start_va='0x56f6ad', end_va='0x56f6d3'))
    result = dict(passed=True, sample_sha256=digest,
                  evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
                  verified_functions=len(evidence['functions']), verified_resources=len(evidence['resources']),
                  verified_data_ranges=len(evidence['data_ranges']), runs=runs,
                  full_ui=False, socket_execution=False, original_level_thresholds_proven=False)
    (ROOT / 'docs/evidence/channel_worlds_probe.json').write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
