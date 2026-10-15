"""Verify master-transfer fields through original writers and bounded parsers; UI is not executed."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_EDI, UC_X86_REG_ESI

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x20000000
OBJECT, HEAP, MANAGER, CHARACTER, TEXT, STACK, STOP = [DATA + v for v in (0x10000,0x30000,0x50000,0x60000,0x70000,0xe0000,0xff000)]


def put(m, address, value): m.mem_write(address, struct.pack('<I', value))
def get(m, address): return struct.unpack('<I', m.mem_read(address, 4))[0]


def text_object(m, address, value, heap):
    raw = value.encode('utf-16le') + b'\0\0'
    count = len(raw) // 2 - 1
    m.mem_write(address if count <= 7 else heap, raw)
    if count > 7: put(m, address, heap)
    put(m, address + 16, count); put(m, address + 20, max(7, count))


def text_value(m, address):
    count = get(m, address + 16)
    pointer = get(m, address) if get(m, address + 20) > 7 else address
    return bytes(m.mem_read(pointer, count * 2)).decode('utf-16le')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample',type=Path,required=True)
    args=parser.parse_args()
    raw=args.sample.read_bytes();digest=hashlib.sha256(raw).hexdigest()
    assert digest=='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe=pefile.PE(data=raw);assert pe.FILE_HEADER.Machine==0x14c and pe.OPTIONAL_HEADER.ImageBase==BASE
    path=ROOT/'docs/evidence/clan_training_ida.json';evidence=json.loads(path.read_text('utf-8'))
    blocks=list(evidence['ranges'])+list(evidence['resources'])
    for f in evidence['functions']:blocks.extend(f['chunks'])
    for b in blocks:
        blob=bytes.fromhex(b['bytes']);assert hashlib.sha256(blob).hexdigest()==b['sha256']
        assert pe.get_data(int(b['rva'],16),len(blob))==blob
    mapped=pe.get_memory_mapped_image();runs=[]

    def machine(mid, payload):
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095); m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x100000)
        m.mem_map(0, 0x1000)  # SEH registration only; packet fault stores are checked.
        m.reg_write(UC_X86_REG_EBP, STACK); m.reg_write(UC_X86_REG_ESP, STACK)
        put(m, STACK, STOP)
        put(m, OBJECT + 4, HEAP); put(m, OBJECT + 8, HEAP + 2)
        put(m, OBJECT + 65548, HEAP + 4); put(m, OBJECT + 65552, HEAP + 4)
        put(m, OBJECT + 65560, HEAP + 4 + len(payload))
        m.mem_write(HEAP, struct.pack('<HH',len(payload),mid) + payload)
        put(m, 0x18e5564, MANAGER); put(m, 0x18e56e8, CHARACTER)
        return m

    written = bytearray()
    WRITE = DATA + 0xb0000

    def execute(m, label, start, ends):
        if isinstance(ends,int): ends = {ends}
        steps, calls, next_heap = 0, [], TEXT + 0x10000

        def hook(uc,address,size,unused):
            nonlocal steps,next_heap
            if address in ends: uc.emu_stop(); return
            if address in (WRITE,0x10e3420):
                sp=uc.reg_read(UC_X86_REG_ESP)
                if address==WRITE:
                    ptr,count=get(uc,sp+4),get(uc,sp+8);assert count==4
                    written.extend(uc.mem_read(ptr,count));pop=12
                    kind='main virtual writer; field selection retained'
                else:
                    assert uc.reg_read(UC_X86_REG_ECX)==202;count=0;pop=4
                    kind='feature202 enabled at original check boundary'
                uc.reg_write(UC_X86_REG_EAX,1);uc.reg_write(UC_X86_REG_EIP,get(uc,sp));uc.reg_write(UC_X86_REG_ESP,sp+pop)
                calls.append(dict(va=hex(address),kind=kind,count=count));return
            if address in (0x1296140,0x12966c0,0x40c2e0):
                sp = uc.reg_read(UC_X86_REG_ESP)
                if address == 0x40c2e0:
                    target = uc.reg_read(UC_X86_REG_ECX)
                    source,count = get(uc,sp+4),get(uc,sp+8)
                    blob = bytes(uc.mem_read(source,count*2)) + b'\0\0'
                    if count > 7:
                        assert next_heap + len(blob) < STACK - 0x30000
                        uc.mem_write(next_heap,blob); put(uc,target,next_heap); next_heap += len(blob)+16
                    else: uc.mem_write(target,blob)
                    put(uc,target+16,count); put(uc,target+20,max(7,count))
                    pop,kind = 12,'wstring assignment; native UTF-16Z scan and cursor preserved'
                else:
                    target,source,count = (get(uc,sp+n) for n in (4,8,12))
                    assert DATA <= target and target+count <= DATA+0x100000
                    uc.mem_write(target,bytes(uc.mem_read(source,count)) if address==0x1296140 else bytes([source&255])*count)
                    pop,kind = 4,'memcpy' if address==0x1296140 else 'memset'
                uc.reg_write(UC_X86_REG_EAX,target)
                uc.reg_write(UC_X86_REG_EIP,get(uc,sp)); uc.reg_write(UC_X86_REG_ESP,sp+pop)
                calls.append(dict(va=hex(address),kind=kind,count=count)); return
            assert BASE <= address < BASE+len(mapped),(label,hex(address))
            assert bytes(uc.mem_read(address,size)) == mapped[address-BASE:address-BASE+size]
            steps += 1

        handle=m.hook_add(UC_HOOK_CODE,hook)
        m.emu_start(start,STOP,count=1000000); m.hook_del(handle)
        end=m.reg_read(UC_X86_REG_EIP); assert end in ends,(label,hex(end))
        assert get(m,0) != 100, label  # Native deliberate null-store packet fault.
        runs.append(dict(label=label,start_va=hex(start),end_va=hex(end),original_instructions=steps,adapter_calls=calls))

    fixtures=ROOT/'tests/clan_training_packets.txt'
    skipped=[]
    for line in fixtures.read_text('utf-8').splitlines():
        label,mid,plain=line.split();mid=int(mid);payload=b'' if plain=='-' else bytes.fromhex(plain)
        m=machine(mid,payload)
        if mid==1106 or (mid==1108 and label!='leave_unknown'):
            m.reg_write(UC_X86_REG_EBP,STACK);m.reg_write(UC_X86_REG_ESP,STACK-0x30000)
            start=0x528564 if mid==1106 else (0x527dfe if label=='leave_manual' else 0x527eea)
            execute(m,label,start,0x1114270)
            packet=get(m,m.reg_read(UC_X86_REG_ESP)+4)
            assert bytes(m.mem_read(get(m,packet+4),4+len(payload)))==struct.pack('<HH',len(payload),mid)+payload
        elif mid==1107:
            put(m,STACK+4,OBJECT);m.reg_write(UC_X86_REG_ECX,MANAGER)
            execute(m,label,0x533de0,{0x533fdf,0x533ecd})
            bp=m.reg_read(UC_X86_REG_EBP);clan=struct.unpack_from('<I',payload)[0]
            assert get(m,bp-0x68)==clan and get(m,OBJECT+65552)==HEAP+4+len(payload)
            if 0<clan<0x80000000:
                assert get(m,bp-0x6c)==struct.unpack_from('<I',payload,4)[0]
                assert text_value(m,bp-0x64)==payload[8:-2].decode('utf-16le')
            else:assert m.reg_read(UC_X86_REG_EIP)==0x533fdf
        elif mid==948:
            clan,flag,mark=struct.unpack('<III',payload);written.clear()
            put(m,STACK-0x3c,TEXT);put(m,TEXT+48,WRITE);put(m,STACK-0x24,HEAP)
            put(m,STACK+8,flag);m.reg_write(UC_X86_REG_ESI,clan);m.reg_write(UC_X86_REG_EDI,mark)
            m.reg_write(UC_X86_REG_ESP,STACK-0x100)
            execute(m,label,0x521385,0x5213cb)
            assert written==payload and m.mem_read(HEAP+4,2)==struct.pack('<H',948)
        elif mid==1515:
            for clan in (99,0):
                m=machine(mid,payload);put(m,MANAGER+44,clan);m.reg_write(UC_X86_REG_ESI,0)
                execute(m,label+'_'+str(clan),0x9d5d60,{0x527eb0,0x9d3c73})
                assert m.mem_read(MANAGER+380,1)==b'\1'
                assert m.reg_read(UC_X86_REG_EIP)==(0x527eb0 if clan==99 else 0x9d3c73)
        else:skipped.append(label)
    assert skipped==['leave_unknown']
    report=dict(passed=True,sample_sha256=digest,evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(fixtures.read_bytes()).hexdigest(),functions=len(evidence['functions']),chunks=sum(len(f['chunks']) for f in evidence['functions']),resources=len(evidence['resources']),fixture_count=13,native_fixture_count=12,codec_only_fixtures=skipped,runs=runs,original_ui=False,limits='C1106/C1108 writer bodies start after eligibility gates. S1107 reader stops before UI with original scalar/string scanning preserved. C948 virtual writing adapted. S1515 branch uses feature202 gate adapter. No full socket, UI, level progression or original server policy executed.')
    (ROOT/'docs/evidence/clan_training_probe.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(dict(passed=True,runs=len(runs),instructions=sum(r['original_instructions'] for r in runs),adapters=sum(len(r['adapter_calls']) for r in runs))))

if __name__=='__main__':main()
