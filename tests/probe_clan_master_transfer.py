"""Verify master-transfer fields through original writers and bounded parsers; UI is not executed."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_EDI

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
    path=ROOT/'docs/evidence/clan_master_transfer_ida.json';evidence=json.loads(path.read_text('utf-8'))
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

    def execute(m, label, start, ends):
        if isinstance(ends,int): ends = {ends}
        steps, calls, next_heap = 0, [], TEXT + 0x10000

        def hook(uc,address,size,unused):
            nonlocal steps,next_heap
            if address in ends: uc.emu_stop(); return
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

    fixtures=ROOT/'tests/clan_master_transfer_packets.txt'
    for line in fixtures.read_text('utf-8').splitlines():
        label,mid,plain,frame=line.split();mid=int(mid);payload=bytes.fromhex(plain)
        m=machine(mid,payload)
        if mid in (1054,1057):
            target=struct.unpack_from('<I',payload)[0];put(m,STACK+4,target)
            if mid==1054:
                name=payload[4:-2].decode('utf-16le');text_object(m,TEXT,name,TEXT+0x1000);put(m,STACK+8,TEXT)
            execute(m,label,0x528070 if mid==1054 else 0x527f60,0x1114270)
            packet=get(m,m.reg_read(UC_X86_REG_ESP)+4)
            assert bytes(m.mem_read(get(m,packet+4),4+len(payload)))==struct.pack('<HH',len(payload),mid)+payload
        elif mid==1060:
            # Dialog result supplied at the boundary; original branch writes
            # byte0/1, constructs C1060 and echoes the exact stored DWORD.
            m.reg_write(UC_X86_REG_EBP,STACK);m.reg_write(UC_X86_REG_ESP,STACK-0x20000)
            m.reg_write(UC_X86_REG_EAX,257 if payload[0] else 0)
            put(m,STACK-0x3c,struct.unpack_from('<I',payload,1)[0]);put(m,STACK-0x40,OBJECT)
            execute(m,label,0x52f9e2,0x1114270)
            packet=get(m,m.reg_read(UC_X86_REG_ESP)+4)
            assert bytes(m.mem_read(get(m,packet+4),4+len(payload)))==struct.pack('<HH',len(payload),mid)+payload
        else:
            put(m,STACK+4,OBJECT);m.reg_write(UC_X86_REG_ECX,MANAGER)
            ranges={1056:(0x52e430,0x51a830),1058:(0x52f320,{0x52f3e6,0x52f607}),1059:(0x52f620,0x52f6d3),1061:(0x5300c0,{0x530133,0x530335}),1062:(0x52fb70,0x52fc63)}
            execute(m,label,*ranges[mid])
            assert get(m,OBJECT+65552)==HEAP+4+len(payload)
            bp=m.reg_read(UC_X86_REG_EBP);sp=m.reg_read(UC_X86_REG_ESP)
            if mid==1056:
                opaque,old=struct.unpack_from('<II',payload)
                tail=payload[8:];end=next(i for i in range(0,len(tail),2) if tail[i:i+2]==b'\0\0')
                former=tail[:end].decode('utf-16le');target=struct.unpack_from('<I',tail,end+2)[0];name=tail[end+6:-2].decode('utf-16le')
                assert [get(m,sp+n) for n in (4,8)]==[old,target]
                assert text_value(m,get(m,sp+12))==name and text_value(m,bp-0x50)==former
            elif mid==1058:
                flag,value=struct.unpack('<BI',payload)
                assert m.mem_read(bp-0x11,1)[0]==flag and m.reg_read(UC_X86_REG_EAX)==value
                assert m.reg_read(UC_X86_REG_EIP)==(0x52f3e6 if not flag or value==2 else 0x52f607)
            elif mid==1059:
                assert text_value(m,bp-0x70)==payload[:-6].decode('utf-16le')
                assert get(m,bp-0x3c)==struct.unpack_from('<I',payload,len(payload)-4)[0]
            elif mid==1061:
                assert m.reg_read(UC_X86_REG_EAX)&255==payload[0]
                assert m.reg_read(UC_X86_REG_EIP)==(0x530335 if payload[0] else 0x530133)
            elif mid==1062:
                assert m.mem_read(bp-0x15,1)[0]==payload[0]
                assert m.reg_read(UC_X86_REG_EDI)==struct.unpack_from('<I',payload,1)[0]
                assert text_value(m,bp-0x6c)==payload[5:-2].decode('utf-16le')
    m=machine(1058,struct.pack('<BI',1,2));put(m,STACK+4,OBJECT)
    execute(m,'S1058_nonzero_flag_reason2_failure',0x52f320,{0x52f3e6,0x52f607})
    assert m.reg_read(UC_X86_REG_EIP)==0x52f3e6
    report=dict(passed=True,sample_sha256=digest,evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(fixtures.read_bytes()).hexdigest(),functions=len(evidence['functions']),chunks=sum(len(f['chunks']) for f in evidence['functions']),ranges=len(evidence['ranges']),resources=len(evidence['resources']),fixture_count=24,runs=runs,original_ui=False,full_handlers=False,limits='C1054/C1057 writers and C1060 dialog-result branch run to send boundary. Readers stop before UI/state consumers. Original UTF16Z scans/cursor/scalar access preserved; CRT memory and string assignment adapted. S1056 opaque first DWORD is skipped natively. Accepted S1062 -> C1054 and state updates are static evidence, not fully executed UI.')
    (ROOT/'docs/evidence/clan_master_transfer_probe.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(dict(passed=True,runs=len(runs),instructions=sum(r['original_instructions'] for r in runs),adapters=sum(len(r['adapter_calls']) for r in runs))))


if __name__=='__main__':main()

