"""Verify mode labels and bounded original profile formatter argument paths."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
import pefile
from unicorn.x86_const import UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ESP
from probe_gift_protocol import Machine, put, get, OBJECT, INPUT, BP

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    sha = lambda data: hashlib.sha256(data).hexdigest()
    raw = args.sample.read_bytes()
    assert sha(raw) == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    path = ROOT/'docs/evidence/stat_reset_service_ida.json'
    capture = json.loads(path.read_text('utf-8'))
    rows = capture['functions'] + capture['ranges'] + capture['resources']
    addresses, blocks = set(), []
    for row in rows:
        data = bytes.fromhex(row['bytes'])
        assert len(data) == row['size'] and sha(data) == row['sha256']
        assert pe.get_data(row['rva'], len(data)) == data
        blocks.append({k: row[k] for k in ('va','rva','size','sha256')})
        addresses.update(x['va'] for x in row.get('assembly', []))
    assert all(any(b['va'] <= a < b['va']+b['size'] for b in blocks) for a in addresses)
    resources = {r['va']:r for r in capture['resources']}
    for binding in capture['label_bindings']:
        assert resources[binding['resource_va']]['text'].startswith(('lbl','lbTab'))
    mapped = pe.get_memory_mapped_image()
    runs = []
    # Offsets are independently read from original call-site instructions;
    # they are native object offsets, never wire offsets or mode IDs.
    paths = [
      ('tab1',0x5d80cf,0x5d9a20,[80,88,84,40]),
      ('tab9',0x5d8caf,0x5d9b00,[272,276,288]),
      ('tab10',0x5d8f0d,0x5d9a20,[404,412,408,428]),
      ('tab11',0x5d92a1,0x5d9a20,[464,472,468,476]),
    ]
    for label,start,stop,offsets in paths:
        vm = Machine(mapped)
        vm.m.reg_write(UC_X86_REG_ESI, OBJECT)
        vm.m.reg_write(UC_X86_REG_EDI, INPUT)
        put(vm.m,BP-0x9c,OBJECT+468)
        put(vm.m,BP-0x48,OBJECT+472)
        put(vm.m,BP-0x44,OBJECT+464)
        runs.append(vm.execute(label+'-counter-pointers',start,stop))
        assert set(vm.instructions) <= addresses
        sp=vm.m.reg_read(UC_X86_REG_ESP)
        assert [get(vm.m,sp+4+i*4)-OBJECT for i in range(len(offsets))] == offsets
    for label,start,wins,denominator in [
       ('tab6',0x5d8683,240,244),('tab7',0x5d899b,252,248)]:
        vm=Machine(mapped)
        vm.m.reg_write(UC_X86_REG_ESI,OBJECT)
        vm.m.reg_write(UC_X86_REG_EDI,INPUT)
        put(vm.m,OBJECT+wins,17);put(vm.m,OBJECT+denominator,29)
        runs.append(vm.execute(label+'-rate-arguments',start,0x5db5c0))
        assert set(vm.instructions) <= addresses
        sp=vm.m.reg_read(UC_X86_REG_ESP)
        assert [get(vm.m,sp+8),get(vm.m,sp+12)] == [17,29]
    report=dict(passed=True,sample_sha256=sha(raw),evidence_sha256=sha(path.read_bytes()),
        script_sha256=sha(Path(__file__).read_bytes()),blocks=blocks,runs=runs,
        label_bindings=len(capture['label_bindings']),original_client_started=False,
        replacement_instructions=0,scope='Bounded call sites only; no UI, full handler, formatting, or original-server reset algorithm executed.')
    print(json.dumps(runs))
    (ROOT/'docs/evidence/stat_reset_modes_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
if __name__ == '__main__':
    main()

