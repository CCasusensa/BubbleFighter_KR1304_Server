"""Ordinary-team winner selection using unchanged BFCLIENT02 instructions."""
import argparse, hashlib, json, sys
from pathlib import Path
sys.dont_write_bytecode = True
import pefile
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EDX
from probe_gift_protocol import Machine, put, get, OBJECT, INPUT, STREAM, VTABLE, HEADER, STACK
ROOT=Path(__file__).resolve().parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sample',type=Path,required=True);args=p.parse_args()
    raw=args.sample.read_bytes();sha=lambda b:hashlib.sha256(b).hexdigest()
    assert sha(raw)=='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe=pefile.PE(data=raw)
    evidence=ROOT/'docs/evidence/battle_awards_ida.json'
    capture=json.loads(evidence.read_text('utf-8'))
    addresses=set();blocks=[]
    for row in capture['functions']:
        data=bytes.fromhex(row['bytes']);assert sha(data)==row['sha256'] and len(data)==row['size']
        assert pe.get_data(row['rva'],len(data))==data
        addresses.update(a['va'] for a in row['assembly'])
        blocks.append({k:row[k] for k in ('va','rva','size','sha256')})
    runs=[]
    for first,second,winner in [(0,0,0),(9,0,1),(0,9,2),(7,7,0),(65535,65534,1),(65534,65535,2)]:
        vm=Machine(pe.get_memory_mapped_image());m=vm.m
        m.reg_write(UC_X86_REG_ECX,OBJECT);m.reg_write(UC_X86_REG_EDX,INPUT)
        put(m,INPUT+0x94,VTABLE);put(m,VTABLE,0)
        put(m,INPUT+0x90,HEADER);put(m,HEADER,1)
        put(m,STACK+4,STREAM);put(m,STREAM+4,2);put(m,STREAM+0x1c4,0xffffffff)
        put(m,STREAM+12,first);put(m,STREAM+16,second)
        run=vm.execute(f'team-{first}-{second}',0x42a340)
        assert get(m,OBJECT)==winner and not vm.calls and set(vm.instructions)<=addresses
        runs.append(dict(**run,first=first,second=second,expected_winner=winner))
    report=dict(passed=True,sample_sha256=sha(raw),evidence_sha256=sha(evidence.read_bytes()),script_sha256=sha(Path(__file__).read_bytes()),blocks=blocks,runs=runs,replacement_instructions=0,original_client_started=False,scope='Full ordinary-mode winner helper, without rendering/network/server algorithm; S105/profile bindings statically rechecked.')
    (ROOT/'docs/evidence/battle_awards_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(runs))
if __name__=='__main__':main()
