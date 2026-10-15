"""Original bundle gift send/list codecs and Life refusal branch without UI."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import pefile
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP
from probe_gift_protocol import Machine, put, get, OBJECT, INPUT, STREAM, STACK, HEADER

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    sha = lambda value: hashlib.sha256(value).hexdigest()
    assert sha(raw) == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    path = ROOT/'docs/evidence/bundle_gifts_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    addresses, blocks = set(), []
    for kind in ('functions', 'ranges', 'resources'):
        for row in evidence[kind]:
            data = bytes.fromhex(row['bytes'])
            assert sha(data) == row['sha256'] and len(data) == row['size']
            assert pe.get_data(int(row['rva'], 16), len(data)) == data
            blocks.append({k: row[k] for k in ('va', 'rva', 'size', 'sha256')})
            addresses.update(int(x['va'], 16) for x in row.get('instructions', []))
    cases_path = ROOT/'tests/bundle_gifts_cases.json'
    cases = json.loads(cases_path.read_text('utf-8'))
    mapped = pe.get_memory_mapped_image()
    runs = []
    for case in cases['samples']:
        vm = Machine(mapped)
        vm.string(INPUT, 'GiftReceiver')
        vm.string(INPUT+24, '')
        put(vm.m, INPUT+48, case['id'])
        vm.string(INPUT+52, 'Bundle')
        vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
        put(vm.m, STACK+4, INPUT)
        runs.append(vm.execute('bundle-send-'+str(case['id']), 0x650770))
        assert set(vm.instructions) <= addresses
        assert bytes(vm.output).hex() == case['request_hex']
        assert int.from_bytes(vm.m.mem_read(HEADER+4, 2), 'little') == 818
        for received, start in [(True, 0x114dba0), (False, 0x114def0)]:
            payload = bytes.fromhex(case['received_record_hex'] if received else case['sent_record_hex'])
            vm = Machine(mapped, payload)
            vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
            put(vm.m, STACK+4, STREAM)
            runs.append(vm.execute(('received-' if received else 'sent-')+str(case['id']), start))
            assert set(vm.instructions) <= addresses
            assert get(vm.m, OBJECT+4) == case['gift_id']
            assert get(vm.m, OBJECT+(44 if received else 52)) == case['id']
            assert vm.string_value(OBJECT+(20 if received else 28)) == ('GiftSender' if received else 'GiftReceiver')
            assert vm.string_value(OBJECT+(52 if received else 60)) == 'Bundle'
            assert int.from_bytes(vm.m.mem_read(OBJECT+(48 if received else 56), 2), 'little') == 0
    for usage in (1, 2, 3):
        vm = Machine(mapped)
        put(vm.m, OBJECT+208, usage)
        vm.m.reg_write(UC_X86_REG_EAX, OBJECT)
        instructions = []
        def hook(machine, address, size, _):
            if address in (0xe208e2, 0xe209a3):
                machine.emu_stop()
                return
            assert address in addresses
            instructions.append(address)
        handle = vm.m.hook_add(UC_HOOK_CODE, hook)
        vm.m.emu_start(0xe208d5, 0, count=10)
        vm.m.hook_del(handle)
        stop = vm.m.reg_read(UC_X86_REG_EIP)
        assert stop == (0xe209a3 if usage == 3 else 0xe208e2)
        runs.append(dict(case='gift-Life-gate-'+str(usage), start='0xe208d5', stop=hex(stop), original_instructions=len(instructions), adapter_calls=[]))
    report = dict(passed=True, sample_sha256=sha(raw), evidence_sha256=sha(path.read_bytes()),
                  cases_sha256=sha(cases_path.read_bytes()), shared_machine_sha256=sha((ROOT/'tests/probe_gift_protocol.py').read_bytes()),
                  blocks=blocks, runs=runs, original_instructions=sum(x['original_instructions'] for x in runs),
                  boundary_adapters=sum(len(x['adapter_calls']) for x in runs), replacement_instructions=0,
                  original_client_started=False,
                  scope='Three native C818 writers, six gift record readers, three bounded Life decisions. No full dialog, rendering, platform callback or server timing claim.')
    (ROOT/'docs/evidence/bundle_gifts_probe.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps({k: report[k] for k in ('passed', 'original_instructions', 'boundary_adapters')}))


if __name__ == '__main__':
    main()
