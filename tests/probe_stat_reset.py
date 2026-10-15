"""Bounded original reset-card requests, result branches and profile copies."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import pefile
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX,
    UC_X86_REG_ESI, UC_X86_REG_ESP, UC_X86_REG_EIP,
)
from probe_gift_protocol import Machine, put, get, OBJECT, INPUT, STACK, BP, HEADER

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', required=True, type=Path)
    args = parser.parse_args()
    sha = lambda data: hashlib.sha256(data).hexdigest()
    raw = args.sample.read_bytes()
    assert sha(raw) == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    path = ROOT/'docs/evidence/stat_reset_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    blocks, addresses = [], set()
    for row in evidence['functions'] + evidence['ranges'] + evidence['resources']:
        data = bytes.fromhex(row['bytes'])
        assert sha(data) == row['sha256'] and len(data) == row['size']
        assert pe.get_data(row['rva'], len(data)) == data
        blocks.append({k: row[k] for k in ('va', 'rva', 'size', 'sha256')})
        addresses.update(x['va'] for x in row.get('assembly', []))
    for address in addresses:
        assert any(x['va'] <= address < x['va']+x['size'] for x in blocks)
    mapped = pe.get_memory_mapped_image()
    cases_path = ROOT/'tests/stat_reset_cases.json'
    cases = json.loads(cases_path.read_text('utf-8'))
    runs = []

    def bounded(vm, label, start, stop):
        run = vm.execute(label, start, stop)
        assert set(vm.instructions) <= addresses
        runs.append(run)

    for case in cases['cases']:
        vm = Machine(mapped)
        if case['kind'] == 'request':
            # Native confirmation tail passes two item IDs and an empty vector.
            put(vm.m, BP-0x48, OBJECT)
            put(vm.m, OBJECT+0x60, case['item_id'])
            bounded(vm, case['label']+'-caller', 0xd41593, 0x5f5ff0)
            sp = vm.m.reg_read(UC_X86_REG_ESP)
            assert [get(vm.m, sp+4+x*4) for x in range(5)] == [case['item_id'], case['item_id'], 0, 0, 0]
            # Shared helper forces the second serialized scalar to one.
            vm = Machine(mapped)
            put(vm.m, STACK+4, case['item_id'])
            put(vm.m, STACK+8, case['item_id'])
            bounded(vm, case['label']+'-helper', 0x5f5ff0, 0x805c50)
            sp = vm.m.reg_read(UC_X86_REG_ESP)
            assert [get(vm.m, sp+4+x*4) for x in range(5)] == [case['item_id'], 1, 0, 0, 0]
            # Writer executed separately; the obfuscated send wrapper is not run.
            vm = Machine(mapped)
            put(vm.m, INPUT, case['item_id'])
            put(vm.m, INPUT+4, 1)
            vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
            put(vm.m, STACK+4, INPUT)
            run = vm.execute(case['label']+'-writer', 0x80da80)
            assert set(vm.instructions) <= addresses
            assert bytes(vm.output).hex() == case['payload']
            assert int.from_bytes(vm.m.mem_read(HEADER+4, 2), 'little') == 806
            runs.append(run)
        else:
            # Stop before any string, dialog, renderer or inventory callback.
            result, parameter = case['result'], case['parameter']
            vm.m.reg_write(UC_X86_REG_ESI, OBJECT)
            put(vm.m, OBJECT+0xfc, 2)
            put(vm.m, OBJECT+0x104, parameter)
            put(vm.m, BP-0x48, result)
            stops = {1: {0: 0x9b44b5, 9: 0x9b457e, 1: 0x9b4785},
                     2: {0: 0x9b49a4, 9: 0x9b4a4f, 1: 0x9b4c74}}
            target = 0x9b60d2 if result == 22 else stops[parameter][result]
            bounded(vm, case['label']+'-dispatch', 0x9b41a5, target)
            payload = bytes.fromhex(case['payload'])
            assert [int.from_bytes(payload[x:x+4], 'little') for x in (0,4,8)] == [result, case['item_id'], 0]

    # Original S105 assignments from an already-decoded FighterInfo. The other
    # three adjacent counters are copied but intentionally remain unnamed.
    vm = Machine(mapped)
    vm.m.reg_write(UC_X86_REG_ESI, OBJECT)
    for index, value in enumerate([11, 13, 17, 23, 29, 31]):
        put(vm.m, BP-0x260+index*4, value)
    bounded(vm, 'profile-win-loss-draw-copy', 0x9a5dbe, 0x9a5e06)
    assert [get(vm.m, OBJECT+172+i*4) for i in range(6)] == [11, 13, 17, 23, 29, 31]
    vm = Machine(mapped)
    vm.m.reg_write(UC_X86_REG_EAX, INPUT)
    put(vm.m, BP-0x288, 19)
    put(vm.m, 0x18e56e8, OBJECT)
    bounded(vm, 'profile-disconnect-copy', 0x9a5eaa, 0x9a5ec3)
    assert get(vm.m, OBJECT+196) == 19

    # Native telemetry formatter proves ordering and explicit field names.
    # Stop on entry to formatting, so no CRT output is treated as native work.
    vm = Machine(mapped)
    vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
    for index, value in enumerate([11, 13, 17, 19]):
        put(vm.m, INPUT+index*4, value)
        put(vm.m, STACK+4+index*4, INPUT+index*4)
    bounded(vm, 'named-statistics-format-arguments', 0x9a6100, 0x1125240)
    sp = vm.m.reg_read(UC_X86_REG_ESP)
    assert get(vm.m, sp+8) == 0x15b3868
    assert [get(vm.m, sp+12+i*4) for i in range(4)] == [11, 13, 17, 19]

    for start, text, values in [(0x5d9a20, 0x1570e74, [11, 17, 13, 19]),
                                (0x5d9b00, 0x1570e58, [11, 13, 19])]:
        vm = Machine(mapped)
        vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
        for index, value in enumerate(values):
            put(vm.m, INPUT+index*4, value)
            put(vm.m, STACK+4+index*4, INPUT+index*4)
        bounded(vm, 'profile-display-format-'+hex(start), start, 0x1125240)
        sp = vm.m.reg_read(UC_X86_REG_ESP)
        assert get(vm.m, sp+8) == text
        assert [get(vm.m, sp+12+i*4) for i in range(len(values))] == values

    report = dict(passed=True, sample_sha256=sha(raw), evidence_sha256=sha(path.read_bytes()),
                  cases_sha256=sha(cases_path.read_bytes()), blocks=blocks, runs=runs,
                  shared_machine_sha256=sha((ROOT/'tests/probe_gift_protocol.py').read_bytes()),
                  original_instructions=sum(x['original_instructions'] for x in runs),
                  boundary_adapters=sum(len(x['adapter_calls']) for x in runs),
                  replacement_instructions=0, original_client_started=False,
                  scope='Two native caller tails, two empty-vector helpers, two C806 writers, eight S813 result branches, two S105 field-copy ranges and three formatting-argument paths. No full S813/S105 handler, obfuscated send wrapper, CRT formatter, UI, persistence or business reset execution.')
    (ROOT/'docs/evidence/stat_reset_probe.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps({k: report[k] for k in ('passed', 'original_instructions', 'boundary_adapters')}))


if __name__ == '__main__':
    main()
