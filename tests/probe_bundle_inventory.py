"""Bounded original bundle warehouse serializers/card selection; no client UI."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import pefile
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESI, UC_X86_REG_EIP
from probe_gift_protocol import Machine, put, get, OBJECT, STACK, STREAM

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    mapped = pe.get_memory_mapped_image()
    path = ROOT/'docs/evidence/bundle_inventory_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    blocks, addresses = [], set()
    for kind in ('functions', 'resources', 'ranges'):
        for row in evidence[kind]:
            data = bytes.fromhex(row['bytes'])
            assert pe.get_data(int(row['rva'], 16), len(data)) == data
            assert hashlib.sha256(data).hexdigest() == row['sha256']
            blocks.append({k: row[k] for k in ('va', 'rva', 'size', 'sha256')})
            addresses.update(int(x['va'], 16) for x in row.get('instructions', []))
    cases = json.loads((ROOT/'tests/bundle_inventory_cases.json').read_text('utf-8'))
    runs = []
    for case in cases['records']:
        m = Machine(mapped)
        slot, stock, item, quantity, days, flag = case['fields']
        for offset, value in [(4, slot), (8, 0), (16, stock), (20, item)]:
            put(m.m, OBJECT+offset, value)
        m.m.mem_write(OBJECT+12, struct.pack('<HH', quantity, days))
        m.string(OBJECT+24, '')
        m.m.mem_write(OBJECT+48, bytes([flag]))
        put(m.m, STACK+4, STREAM)
        m.m.reg_write(UC_X86_REG_ECX, OBJECT)
        run = m.execute(case['name'], 0x1142e40)
        assert set(m.instructions) <= addresses
        assert bytes(m.output).hex() == case['record_hex']
        runs.append(run)
    for case in cases['cards']:
        m = Machine(mapped)
        stock, item, quantity, days, flag = case['fields']
        put(m.m, OBJECT+112, stock)
        put(m.m, OBJECT+116, item)
        m.m.mem_write(OBJECT+108, struct.pack('<HH', quantity, days))
        m.m.mem_write(OBJECT+144, bytes([flag]))
        m.m.reg_write(UC_X86_REG_ESI, OBJECT)
        instructions = []
        def hook(machine, address, size, _):
            assert address in addresses, hex(address)
            instructions.append(address)
        handle = m.m.hook_add(UC_HOOK_CODE, hook)
        m.m.emu_start(0xd3ccd2, 0xd3cd3d, count=1000)
        m.m.hook_del(handle)
        assert m.m.reg_read(UC_X86_REG_EIP) == 0xd3cd3d
        actual = [get(m.m, OBJECT+56), m.m.mem_read(OBJECT+173, 1)[0],
                  struct.unpack('<H', m.m.mem_read(OBJECT+174, 2))[0],
                  m.m.mem_read(OBJECT+176, 1)[0]]
        assert actual == case['expected'], (case['name'], actual)
        runs.append({'case': case['name'], 'start': '0xd3ccd2', 'stop': '0xd3cd3d',
                     'original_instructions': len(instructions), 'adapter_calls': [], 'expected': actual})
    report = {'passed': True, 'sample_sha256': digest, 'blocks': blocks, 'runs': runs,
              'evidence_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
              'cases_sha256': hashlib.sha256((ROOT/'tests/bundle_inventory_cases.json').read_bytes()).hexdigest(),
              'shared_machine_sha256': hashlib.sha256((ROOT/'tests/probe_gift_protocol.py').read_bytes()).hexdigest(),
              'original_instructions': sum(x['original_instructions'] for x in runs),
              'boundary_adapters': sum(len(x['adapter_calls']) for x in runs),
              'replacement_instructions': 0, 'original_client_started': False,
              'scope': 'Synthetic original cash record writers and D3CCD2 card decision only. No full constructor, result dialog, transaction or UI execution.'}
    (ROOT/'docs/evidence/bundle_inventory_probe.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps({k: report[k] for k in ('passed', 'original_instructions', 'boundary_adapters')}))


if __name__ == '__main__':
    main()
