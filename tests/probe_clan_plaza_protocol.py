"""Execute bounded C2028 collection/writing, cached gates and S117 scene selection."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x30000000
BP, STACK, HEADER, VTABLE, TEXT, WRITE, READ, STAGE, MANAGER, PARTY, ROWS, STREAM, POINTER, OBJECT, CONTEXT, STOP = [DATA+n for n in (0x10000,0x8000,0x12000,0x14000,0x16000,0x18000,0x18100,0x18200,0x20000,0x22000,0x24000,0x26000,0x28000,0x2a000,0x2c000,0x3f000)]

def put(m, a, v): m.mem_write(a, struct.pack('<I', v & 0xffffffff))
def get(m, a): return struct.unpack('<I', m.mem_read(a, 4))[0]
def string(m, a, raw, heap=TEXT):
    count = len(raw)//2
    m.mem_write(a if count <= 7 else heap, raw+b'\0\0')
    if count > 7: put(m, a, heap)
    put(m, a+16, count); put(m, a+20, max(count, 7))
def value(m, a):
    return bytes(m.mem_read(get(m,a) if get(m,a+20)>7 else a, get(m,a+16)*2))

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample',type=Path,required=True)
    args=parser.parse_args()
    raw=args.sample.read_bytes(); digest=hashlib.sha256(raw).hexdigest()
    assert digest=='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe=pefile.PE(data=raw); assert pe.FILE_HEADER.Machine==0x14c and pe.OPTIONAL_HEADER.ImageBase==BASE
    path=ROOT/'docs/evidence/clan_plaza_protocol_ida.json'; evidence=json.loads(path.read_text('utf-8'))
    blocks=evidence['resources']+evidence['ranges']+[c for f in evidence['functions'] for c in f['chunks']]
    for b in blocks:
        blob=bytes.fromhex(b['bytes']);assert hashlib.sha256(blob).hexdigest()==b['sha256']
        assert pe.get_data(int(b['rva'],16),len(blob))==blob
    mapped=pe.get_memory_mapped_image()
    fixture=ROOT/'tests/clan_plaza_protocol_packets.txt'
    fixed={label:(int(mid),bytes.fromhex(body)) for label,mid,body in (l.split() for l in fixture.read_text('ascii').splitlines())}
    case_path=ROOT/'tests/clan_plaza_protocol_cases.json';cases=json.loads(case_path.read_text('utf-8'))
    runs=[]
    def machine():
        m=Uc(UC_ARCH_X86,UC_MODE_32)
        m.mem_map(BASE,(len(mapped)+4095)&~4095);m.mem_write(BASE,mapped)
        m.mem_map(DATA,0x40000);m.mem_map(0,0x1000)
        m.reg_write(UC_X86_REG_EBP,BP);m.reg_write(UC_X86_REG_ESP,STACK)
        put(m,STACK,STOP);put(m,VTABLE+48,WRITE);put(m,VTABLE+44,READ);put(m,VTABLE+28,STAGE)
        return m
    def execute(m,label,start,ends,payload=b'',cursor=0):
        if isinstance(ends,int): ends={ends}
        instructions=[];calls=[];output=bytearray();next_heap=DATA+0x30000
        def hook(uc,a,n,_):
            nonlocal cursor,next_heap
            if a in ends: uc.emu_stop();return
            if a in (WRITE,READ,0x40c2e0,0x40daa0,0x40dbf0):
                sp=uc.reg_read(UC_X86_REG_ESP);ret=get(uc,sp);pop=4
                if a==WRITE:
                    pointer,count=get(uc,sp+4),get(uc,sp+8)
                    assert count<1024;output.extend(uc.mem_read(pointer,count));pop=12
                    kind='virtual write';result=count
                elif a==READ:
                    pointer,count=get(uc,sp+4),get(uc,sp+8)
                    assert cursor+count<=len(payload),(label,cursor,count,len(payload))
                    uc.mem_write(pointer,payload[cursor:cursor+count]);cursor+=count;pop=12
                    kind='virtual read';result=count
                elif a==0x40dbf0:
                    kind='temporary string destruction';result=0
                else:
                    target=uc.reg_read(UC_X86_REG_ECX);source=get(uc,sp+4)
                    if a==0x40c2e0:
                        count=get(uc,sp+8);pop=12
                    else:
                        count=0
                        while uc.mem_read(source+2*count,2)!=b'\0\0': count+=1
                        pop=8
                    assert count<1024
                    blob=bytes(uc.mem_read(source,count*2));string(uc,target,blob,next_heap);next_heap+=len(blob)+32
                    kind='wstring assignment' if a==0x40c2e0 else 'wstring construction';result=target
                calls.append(dict(va=hex(a),return_va=hex(ret),kind=kind))
                uc.reg_write(UC_X86_REG_EAX,result);uc.reg_write(UC_X86_REG_ESP,sp+pop);uc.reg_write(UC_X86_REG_EIP,ret);return
            assert BASE<=a<BASE+len(mapped),(label,hex(a))
            assert bytes(uc.mem_read(a,n))==mapped[a-BASE:a-BASE+n]
            instructions.append(hex(a))
        handle=m.hook_add(UC_HOOK_CODE,hook)
        m.emu_start(start,0,count=10000);m.hook_del(handle)
        end=m.reg_read(UC_X86_REG_EIP);assert end in ends,(label,hex(end))
        assert get(m,0)!=100
        runs.append(dict(label=label,start_va=hex(start),end_va=hex(end),original_instructions=len(instructions),trace=instructions,adapter_calls=calls))
        return bytes(output),cursor,end
    for row in cases['requests']:
        if not row['native_writer']:continue
        label=row['label'];mid,body=fixed[label];m=machine()
        put(m,0x18e5564,MANAGER);put(m,MANAGER+44,row['clan_id'])
        string(m,MANAGER+52,row['name'].encode('utf-16-le'));string(m,BP-0x30,b'')
        execute(m,label+'_collect',0x5375c0,0x5375ef)
        assert bytes(m.mem_read(BP-0x38,8))==body[:8]
        put(m,BP-0x64,VTABLE);put(m,BP-0x4c,HEADER)
        out,_,_=execute(m,label+'_write',0x537625,0x53767d)
        assert out==body and bytes(m.mem_read(HEADER+4,2))==struct.pack('<H',mid)
    endings={'no_clan':0x537a63,'mixed':0x537837,'send':0x537a4f}
    for row in cases['gate_cases']:
        m=machine();put(m,0x18e5564,MANAGER);put(m,MANAGER+44,row['cached_clan']);put(m,MANAGER+68,row['name_length'])
        party=row['party_clans'];put(m,0x18e5660,0 if party is None else PARTY)
        if party is not None:
            put(m,PARTY+8,ROWS);put(m,PARTY+12,ROWS+48*len(party))
            for i,clan in enumerate(party):put(m,ROWS+48*i+44,clan)
        _,_,end=execute(m,'gate_'+row['label'],0x5377f3,set(endings.values()))
        assert end==endings[row['expected']]
    for row in cases['scenes']:
        label=row['label'];mid,body=fixed[label];assert mid==117
        m=machine();put(m,STREAM,VTABLE);put(m,POINTER,STREAM)
        m.reg_write(UC_X86_REG_ESI,OBJECT);m.reg_write(UC_X86_REG_EDI,POINTER)
        _,cursor,_=execute(m,label+'_read_tail',0x113bedb,0x113bf23,body,row['wide_offset'])
        name=row['text_at88'].encode('utf-16-le')
        assert cursor==row['wide_offset']+12+len(name)
        assert get(m,OBJECT+80)==row['wide_at80'] and value(m,OBJECT+88)==name
        # Copy the parsed object into the existing S117 handler's stack layout.
        m.mem_write(BP-0x318,bytes(m.mem_read(OBJECT+88,24)))
        string(m,BP-0x368,row['text_at8'].encode('utf-16-le'),TEXT+0x400)
        put(m,BP-0x330,row['session_type']);put(m,0x18e566c,CONTEXT);put(m,0x18e5608,MANAGER);put(m,MANAGER,VTABLE)
        m.reg_write(UC_X86_REG_ESI,MANAGER);m.reg_write(UC_X86_REG_ESP,STACK)
        execute(m,label+'_select_stage',0x9ac9cd,STAGE)
        assert get(m,m.reg_read(UC_X86_REG_ESP)+4)==row['stage']
        if row['session_type']==6:assert value(m,CONTEXT+356)==name
        if row['session_type']==3:assert value(m,CONTEXT+332)==row['text_at8'].encode('utf-16-le')
        stage=machine();put(stage,OBJECT,row['stage']);put(stage,BP+8,TEXT)
        stage.reg_write(UC_X86_REG_ECX,OBJECT)
        execute(stage,label+'_select_asset',0x931060,0x40daa0)
        resource=get(stage,stage.reg_read(UC_X86_REG_ESP)+4)
        assert resource=={2:0x15ab510,3:0x15ab388,15:0x15ab3f8}[row['stage']]
    report=dict(passed=True,sample_sha256=digest,evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(),cases_sha256=hashlib.sha256(case_path.read_bytes()).hexdigest(),functions=len(evidence['functions']),chunks=sum(len(f['chunks']) for f in evidence['functions']),resources=len(evidence['resources']),ranges=len(evidence['ranges']),verified_blocks=len(blocks),fixtures=len(fixed),runs=runs,original_ui=False,
      limits='C2028 manager collection and writer bodies are separate bounded runs; packet constructors/sockets are skipped. Virtual I/O and string allocation/destruction are adapted. Native cached gates and mismatch counter run without adapters. S117 starts at wide80/text88 reader suffix, then stage/asset branches; descriptor prefix, configuration and full UI are not executed. C2029 is statically classified as another request; no reply is inferred. wide80 semantics are unproven. Arbitrary 64-bit C2028 fixture is codec-only.')
    (ROOT/'docs/evidence/clan_plaza_protocol_probe.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=True,blocks=len(blocks),runs=len(runs),instructions=sum(x['original_instructions'] for x in runs),adapters=sum(len(x['adapter_calls']) for x in runs))))

if __name__=='__main__':main()
