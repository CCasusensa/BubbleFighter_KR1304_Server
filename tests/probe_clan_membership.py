"""Run original clan membership writers/readers up to their network/UI boundaries."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

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
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    path = ROOT / 'docs/evidence/clan_membership_ida.json'
    evidence = json.loads(path.read_text('utf-8'))
    for f in evidence['functions']:
        for block in f['chunks']:
            blob = bytes.fromhex(block['bytes'])
            assert hashlib.sha256(blob).hexdigest() == block['sha256']
            assert pe.get_data(int(block['rva'], 16), len(blob)) == blob
    for block in evidence['data_ranges'] + evidence['resources']:
        blob = bytes.fromhex(block['bytes'])
        assert hashlib.sha256(blob).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'], 16), len(blob)) == blob
    mapped = pe.get_memory_mapped_image()
    runs = []

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

    fixture_path=ROOT/'tests/clan_membership_packets.txt'
    fixtures=[]
    for line in fixture_path.read_text().splitlines():
        label,mid,body,frame=line.split(); fixtures.append((label,int(mid),b'' if body=='-' else bytes.fromhex(body),bytes.fromhex(frame)))
    cases={'zero':(0,0,0,0,'',''),'normal':(1,71,1,100,'ClanName','Fighter'),
           'upper':(0xffffffff,0x87654321,0xffffffec,0x12345678,'水泡🚀','玩家B')}
    writers={1032:0x527010,1033:0x527130,1039:0x527b80,1063:0x527250}
    readers={1034:(0x52e2a0,{0x51f920}),1037:(0x52e640,{0x51d330,0x51ea80}),
             1040:(0x52d090,{0x51e2c0}),1041:(0x52ce70,{0x51d330,0x51ea80}),
             1064:(0x525170,{0x5201c0}),1065:(0x525170,{0x51e2c0}),1035:(0x525170,{0x51c630})}
    for label,mid,body,frame in fixtures:
        values=cases.get(label.split('_')[0],(0,0,0,0,'',''))
        result,target,old,new,clan,name=values
        if mid in writers:
            m=machine(mid,b'')
            text_object(m,TEXT,clan,TEXT+0x1000);text_object(m,TEXT+0x100,name,TEXT+0x2000)
            args=[target,TEXT]
            if mid==1033:args.append(TEXT+0x100)
            if mid==1039:
                args=[target,TEXT+0x100,old,new]
                m.reg_write(UC_X86_REG_ECX,OBJECT);m.mem_write(OBJECT+65626,b'\1')
            if mid==1063:args=[target]
            for i,v in enumerate(args):put(m,STACK+4+i*4,v)
            execute(m,label,writers[mid],0x1114270)
            packet=get(m,m.reg_read(UC_X86_REG_ESP)+4); header=get(m,packet+4)
            assert bytes(m.mem_read(header,len(body)+4))==struct.pack('<HH',len(body),mid)+body,label
        elif mid==1036:
            m=machine(mid,b'');m.reg_write(UC_X86_REG_EBP,STACK);m.reg_write(UC_X86_REG_ESP,STACK-0x2004c)
            execute(m,label,0x527e62,0x1114270)
            packet=get(m,m.reg_read(UC_X86_REG_ESP)+4)
            assert bytes(m.mem_read(get(m,packet+4),4))==struct.pack('<HH',0,1036)
        else:
            for own in ([True,False] if mid in (1037,1041) else [True]):
                m=machine(mid,body);put(m,STACK+4,OBJECT);m.reg_write(UC_X86_REG_ECX,MANAGER)
                put(m,CHARACTER+48,target if own else target^1)
                start,ends=readers[mid];execute(m,label+('_self' if own else '_other'),start,ends)
                assert get(m,OBJECT+65552)==HEAP+4+len(body),label
                sp=m.reg_read(UC_X86_REG_ESP); end=m.reg_read(UC_X86_REG_EIP)
                if mid==1034:
                    assert [get(m,sp+n) for n in (4,8,12)]==[result,old,target]
                    assert text_value(m,get(m,sp+16))==clan
                elif mid in (1037,1041):
                    assert end==(0x51d330 if own else 0x51ea80)
                    assert text_value(m,get(m,sp+4))==(clan if own else name)
                    assert [get(m,sp+n) for n in (8,12)]==[new,old]
                elif mid in (1040,1065):
                    assert get(m,sp+4)==result and text_value(m,get(m,sp+8))==name
                    assert [get(m,sp+n) for n in (12,16)]==([new,old] if mid==1040 else [0xffffffce,1])
                elif mid==1064:assert get(m,sp+4)==result
    report=dict(passed=True,sample_sha256=digest,evidence_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),fixtures_sha256=hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
      functions=len(evidence['functions']),chunks=sum(len(f['chunks']) for f in evidence['functions']),data_ranges=len(evidence['data_ranges']),resources=len(evidence['resources']),fixture_count=len(fixtures),runs=runs,
      original_ui=False,socket_execution=False,full_handlers=False,limits='Writers stop before network send; parsers stop before UI/state consumers. C1036 begins after the training-clan predicate. CRT memory/string assignment adapters do not replace wire field reads or writes. S1037 and S1041 old/new order both verified for own and other characters.')
    (ROOT/'docs/evidence/clan_membership_probe.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=True,runs=len(runs),instructions=sum(r['original_instructions'] for r in runs),adapters=sum(len(r['adapter_calls']) for r in runs))))


if __name__=='__main__':main()
