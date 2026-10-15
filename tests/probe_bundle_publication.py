"""Bounded original S800 bundle writers and display/filter decisions; no UI."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import pefile
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_EIP
from probe_gift_protocol import Machine, put, get, OBJECT, INPUT, STACK, BP, STREAM, VTABLE

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    sha = lambda data: hashlib.sha256(data).hexdigest()
    assert sha(raw) == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    path = ROOT/'docs/evidence/bundle_publication_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    addresses, blocks = set(), []
    for kind in ('functions', 'ranges', 'resources'):
        for row in evidence[kind]:
            data = bytes.fromhex(row['bytes'])
            assert len(data) == row['size'] and sha(data) == row['sha256']
            assert pe.get_data(int(row['rva'], 16), len(data)) == data
            blocks.append({k: row[k] for k in ('va', 'rva', 'size', 'sha256')})
            addresses.update(int(x['va'], 16) for x in row.get('instructions', []))
    cases_path = ROOT/'tests/bundle_publication_cases.json'
    cases = json.loads(cases_path.read_text('utf-8'))
    mapped = pe.get_memory_mapped_image()
    runs = []
    for identity in cases['native_records']:
        c = next(x for x in cases['cards'] if x['id'] == identity)
        m = Machine(mapped)
        for offset, value in [(4, identity), (8, 1), (36, c['order']), (40, 800), (44, 800),
                              (48, 1), (72, VTABLE+128), (76, identity), (80, identity),
                              (88, 0), (92, c['show']), (96, c['sell'])]:
            put(m.m, OBJECT+offset, value)
        put(m.m, VTABLE+128, 0x113eec0)
        m.m.mem_write(OBJECT+84, struct.pack('<BBH', 0, 0, 1))
        m.string(OBJECT+12, c['name'])
        m.m.reg_write(UC_X86_REG_ECX, OBJECT)
        put(m.m, STACK+4, STREAM)
        run = m.execute('bundle-card-'+str(identity), 0x1140210)
        assert set(m.instructions) <= addresses
        assert bytes(m.output).hex() == c['wire_hex']
        runs.append(run)

    def bounded(m, label, start, stops):
        instructions = []
        def hook(machine, address, size, _):
            if address in stops:
                machine.emu_stop()
                return
            assert address in addresses, hex(address)
            instructions.append(address)
        handle = m.m.hook_add(UC_HOOK_CODE, hook)
        m.m.emu_start(start, 0, count=1000)
        m.m.hook_del(handle)
        stop = m.m.reg_read(UC_X86_REG_EIP)
        assert stop in stops
        runs.append(dict(case=label, start=hex(start), stop=hex(stop), original_instructions=len(instructions), adapter_calls=[]))
        return stop

    for kind, options, expected in [(1, 0, 0x607bb2), (1, 1, 0x607bb2), (0, 0, 0x607bb2), (0, 1, 0x607460)]:
        m = Machine(mapped)
        put(m.m, OBJECT+8, kind)
        put(m.m, OBJECT+100, INPUT)
        put(m.m, OBJECT+104, INPUT+28*options)
        m.m.reg_write(UC_X86_REG_EDI, OBJECT)
        assert bounded(m, f'price-options-{kind}-{options}', 0x60744a, {0x607bb2, 0x607460}) == expected
    for currency, stop in [(1, 0x9fc3a9), (2, 0x9fc275), (3, 0x9fc416)]:
        m = Machine(mapped)
        # Deliberately distinct values: the main label must pass show_price,
        # not sell_price, to the original formatter boundary.
        put(m.m, OBJECT+0x604, 4500)
        put(m.m, OBJECT+0x608, 4000)
        m.m.reg_write(UC_X86_REG_EDI, OBJECT)
        m.m.reg_write(UC_X86_REG_EAX, currency)
        assert bounded(m, f'price-label-currency-{currency}', 0x9fc261, {stop}) == stop
        assert m.m.reg_read(UC_X86_REG_EDX) == 4500
    for index, c in enumerate(cases['native_filters']):
        m = Machine(mapped)
        put(m.m, INPUT+4, 2000001)
        put(m.m, INPUT+40, c['category'])
        put(m.m, INPUT+48, c['visible'])
        put(m.m, OBJECT+0x660, c['selected'])
        put(m.m, OBJECT+0x6d4, INPUT+0x200)
        put(m.m, OBJECT+0x6d8, INPUT+0x200+4*int(c['excluded']))
        put(m.m, INPUT+0x200, 2000001)
        put(m.m, BP-0x1c, OBJECT)
        m.m.reg_write(UC_X86_REG_EDI, INPUT)
        m.m.reg_write(UC_X86_REG_ESI, OBJECT)
        stop = bounded(m, 'main-category-filter-'+str(index), 0x64e750, {0x64e78b, 0x64e7b4, 0x64e7b7})
        assert (stop == 0x64e78b) == c['accepted']
    report = dict(passed=True, sample_sha256=sha(raw), evidence_sha256=sha(path.read_bytes()),
                  cases_sha256=sha(cases_path.read_bytes()), shared_machine_sha256=sha((ROOT/'tests/probe_gift_protocol.py').read_bytes()),
                  blocks=blocks, runs=runs, original_instructions=sum(x['original_instructions'] for x in runs),
                  boundary_adapters=sum(len(x['adapter_calls']) for x in runs), replacement_instructions=0,
                  original_client_started=False,
                  scope='Four synthetic native card writers and eleven bounded price/filter decisions. Formatting, full handlers, resource loader, dialogs and UI not executed.')
    (ROOT/'docs/evidence/bundle_publication_probe.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps({k: report[k] for k in ('passed', 'original_instructions', 'boundary_adapters')}))


if __name__ == '__main__':
    main()
