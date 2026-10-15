"""Verify retained native purchase refusal dispatch; no UI or policy inference."""
import argparse
import hashlib
import json
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    sha = lambda value: hashlib.sha256(value).hexdigest()
    raw = args.sample.read_bytes()
    assert sha(raw) == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    path = ROOT/'docs/evidence/delivery_capacity_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    blocks, addresses = [], set()
    for row in evidence['functions'] + evidence['ranges']:
        data = bytes.fromhex(row['bytes'])
        assert len(data) == row['size'] and sha(data) == row['sha256']
        assert pe.get_data(int(row['rva'], 16), len(data)) == data
        blocks.append({k: row[k] for k in ('va', 'rva', 'size', 'sha256')})
        addresses.update(int(x['va'], 16) for x in row.get('instructions', []))
    image = pe.get_memory_mapped_image()
    runs = []
    # Fixed native mapping read through IDA MCP; unrelated to server projections.
    for result, target in [(-200, 0x9355ae), (-13, 0x934e38),
                           (1, 0x935d3a), (9, 0x93508c), (36, 0x9353d0)]:
        vm = Uc(UC_ARCH_X86, UC_MODE_32)
        vm.mem_map(0x400000, (len(image)+4095)&~4095)
        vm.mem_write(0x400000, image)
        vm.reg_write(UC_X86_REG_EAX, result & 0xffffffff)
        executed = []

        def hook(machine, address, size, _):
            if address == target:
                machine.emu_stop()
                return
            assert address in addresses and 0x934e09 <= address <= 0x934e31
            executed.append(address)

        vm.hook_add(UC_HOOK_CODE, hook)
        vm.emu_start(0x934e09, 0, count=32)
        assert vm.reg_read(UC_X86_REG_EIP) == target
        runs.append(dict(result=result, start='0x934e09', stop=hex(target), original_instructions=len(executed)))
    report = dict(passed=True, sample_sha256=sha(raw), evidence_sha256=sha(path.read_bytes()),
                  blocks=blocks, runs=runs, original_instructions=sum(x['original_instructions'] for x in runs),
                  boundary_adapters=0, replacement_instructions=0, original_client_started=False,
                  scope='Five bounded original result-dispatch branches after existing conversion. No dialog, full response handler or original-server capacity policy execution.')
    (ROOT/'docs/evidence/delivery_capacity_probe.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8', newline='\n')
    print(json.dumps({k: report[k] for k in ('passed', 'original_instructions', 'boundary_adapters')}))


if __name__ == '__main__':
    main()
