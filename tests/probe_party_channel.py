"""Verify BFCLIENT02 party-channel evidence and execute its leader predicate."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--sample', type=Path, default=Path('F:/BubbleFighter1.3.16/BubbleFighterU.exe'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    evidence_path = root / 'docs/evidence/party_channel_ida.json'
    evidence = json.loads(evidence_path.read_text(encoding='utf-8'))
    sample = args.sample.read_bytes()
    expected = '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    assert hashlib.sha256(sample).hexdigest() == evidence['sample_sha256'] == expected
    pe = pefile.PE(data=sample, fast_load=True)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    ranges = []
    for category in ('functions',):
        for row in evidence[category]:
            address = int(row['va'], 16)
            raw = bytes.fromhex(row['bytes'])
            assert address - 0x400000 == int(row['rva'], 16)
            assert pe.get_data(address - 0x400000, len(raw)) == raw
            ranges.append({'kind': category, 'va': row['va'], 'rva': row['rva'],
                           'size': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
    leader = next(r for r in evidence['functions'] if r['va'] == '0x906490')
    code = bytes.fromhex(leader['bytes'])
    cases = [([], 100, False), ([100], 100, True), ([101], 100, False),
             ([100, 101, 102], 100, True), ([100, 101, 102], 101, False),
             ([102, 101, 100], 100, False), ([101, 100, 102], 101, True),
             ([0xffffffff, 100], 0xffffffff, True)]
    runs = []
    for members, own_id, expected_result in cases:
        vm = Uc(UC_ARCH_X86, UC_MODE_32)
        for page in (0x906000, 0x18e5000, 0x2000000, 0x2100000, 0x2200000,
                     0x3000000, 0x4000000):
            vm.mem_map(page, 0x1000)
        vm.mem_write(0x906490, code)
        u32 = lambda address, value: vm.mem_write(address, struct.pack('<I', value))
        u32(0x18e5660, 0x2000000)
        u32(0x18e56e8, 0x2100000)
        u32(0x2000008, 0x2200000)
        u32(0x200000c, 0x2200000 + 48 * len(members))
        u32(0x2100030, own_id)
        for i, identity in enumerate(members):
            u32(0x2200000 + 48 * i + 4, identity)
        u32(0x4000800, 0x3000000)
        vm.reg_write(UC_X86_REG_ESP, 0x4000800)
        count = [0]

        def instruction(_vm, address, size, _data):
            assert 0x906490 <= address < 0x906490 + len(code)
            count[0] += 1

        vm.hook_add(UC_HOOK_CODE, instruction)
        vm.emu_start(0x906490, 0x3000000, count=100)
        assert vm.reg_read(UC_X86_REG_ESP) == 0x4000804
        actual = bool(vm.reg_read(UC_X86_REG_EAX) & 0xff)
        assert actual == expected_result
        runs.append({'members': members, 'own_id': own_id, 'leader': actual,
                     'native_instructions': count[0], 'adapted_calls': 0})
    report = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'passed': True, 'sample_sha256': expected, 'image_base': '0x400000',
              'evidence_sha256': hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
              'ranges': ranges, 'runs': runs,
              'native_instructions': sum(r['native_instructions'] for r in runs),
              'adapted_calls': 0, 'new_codecs': 0, 'original_ui': False,
              'limits': 'Only original 906490 executes without helper substitution. UI gates, dialogs, C202 send, complete channel handlers and server policy are not executed.'}
    (root / 'docs/evidence/party_channel_probe.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f"PASS: {len(ranges)} PE ranges, {len(runs)} leader predicates, {report['native_instructions']} original instructions, no adapted calls.")


if __name__ == '__main__':
    main()
