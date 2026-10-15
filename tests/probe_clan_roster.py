"""Verify clan roster/recruitment wire operations using bounded original x86 code."""
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
OBJECT, HEAP, MANAGER, TEXT, STACK, STOP = [DATA + v for v in (0x10000, 0x30000, 0x50000, 0x70000, 0xe0000, 0xff000)]


def put(m, address, value): m.mem_write(address, struct.pack('<I', value))
def get(m, address): return struct.unpack('<I', m.mem_read(address, 4))[0]
def wide(m, address):
    size = get(m, address+16)
    pointer = get(m, address) if get(m, address+20) > 7 else address
    return bytes(m.mem_read(pointer, size*2))+b'\0\0'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    evidence_path = ROOT/'docs/evidence/clan_roster_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    for function in evidence['functions']:
        for block in function['chunks']:
            blob = bytes.fromhex(block['bytes'])
            assert hashlib.sha256(blob).hexdigest() == block['sha256']
            assert pe.get_data(int(block['rva'],16),len(blob)) == blob
    for block in evidence['resources']:
        blob = bytes.fromhex(block['bytes'])
        assert hashlib.sha256(blob).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'],16),len(blob)) == blob
    mapped = pe.get_memory_mapped_image()
    runs = []

    def machine(mid, body):
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped)+4095)&~4095); m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x100000); m.mem_map(0,0x1000)
        m.reg_write(UC_X86_REG_EBP,STACK); m.reg_write(UC_X86_REG_ESP,STACK)
        put(m,STACK,STOP); put(m,STACK+4,OBJECT)
        put(m,OBJECT+4,HEAP); put(m,OBJECT+8,HEAP+2)
        put(m,OBJECT+65548,HEAP+4); put(m,OBJECT+65552,HEAP+4)
        put(m,OBJECT+65560,HEAP+4+len(body))
        m.mem_write(HEAP,struct.pack('<HH',len(body),mid)+body)
        put(m,0x18e5564,MANAGER); put(m,0x18e6d38,0)
        m.mem_write(0x18911d2,b'\1'); m.mem_write(OBJECT+65626,b'\1')
        m.reg_write(UC_X86_REG_ECX,OBJECT)
        return m

    # Hooks only omit container construction/destruction AFTER all row reads.
    # Native count checks, row field reads, UTF-16Z scans and loop counters run.
    row_edges = {1012:(0x52bdbb,0x52bf5e),1014:(0x52c3be,0x52c6df),1070:(0x52c9c9,0x52cccc)}
    def execute(m,label,mid,start,end):
        steps,next_heap = 0,TEXT
        calls,rows = [],[]
        def hook(uc,address,size,unused):
            nonlocal steps,next_heap
            if address == end:
                uc.emu_stop(); return
            bp = uc.reg_read(UC_X86_REG_EBP)
            sp = uc.reg_read(UC_X86_REG_ESP)
            if mid in row_edges and address == row_edges[mid][0]:
                if mid == 1012:
                    row = struct.pack('<I',get(uc,bp-0x28))+wide(uc,bp-0x60)
                    for off in (0x68,0x70,0x6c): row += bytes(uc.mem_read(bp-off,4))
                elif mid == 1014:
                    row = struct.pack('<I',get(uc,bp-0x6c))+wide(uc,bp-0x68)
                    for off in (0x70,0x74,0x78,0x7c): row += bytes(uc.mem_read(bp-off,4))
                    for off in (0xc0,0xc8,0xd0): row += bytes(uc.mem_read(bp-off,8))
                    for off in (0x80,0x84,0x88,0x8c): row += bytes(uc.mem_read(bp-off,4))
                else:
                    row = struct.pack('<I',get(uc,bp-0x6c))+wide(uc,bp-0x60)
                    for off in (0x70,0x74): row += bytes(uc.mem_read(bp-off,4))
                    row += bytes(uc.mem_read(bp-0xc0,8))
                    for off in (0x78,0x7c,0x80,0x84): row += bytes(uc.mem_read(bp-off,4))
                rows.append(row)
                calls.append(dict(va=hex(address),kind='omit row container construction/destruction',resume_va=hex(row_edges[mid][1])))
                uc.reg_write(UC_X86_REG_EIP,row_edges[mid][1]); return
            if address in (0x12966c0,0x40c2e0,0x51f770,0x5189f0,0x5279b0) and not (address==start):
                if address == 0x40c2e0:
                    target=uc.reg_read(UC_X86_REG_ECX); source,count=get(uc,sp+4),get(uc,sp+8)
                    blob=bytes(uc.mem_read(source,count*2))+b'\0\0'
                    if count>7:
                        assert next_heap+len(blob)<STACK-0x20000
                        uc.mem_write(next_heap,blob);put(uc,target,next_heap);next_heap+=len(blob)+16
                    else:uc.mem_write(target,blob)
                    put(uc,target+16,count);put(uc,target+20,max(7,count))
                    uc.reg_write(UC_X86_REG_EAX,target);pop=12;kind='wstring assignment after native Z scan'
                elif address == 0x12966c0:
                    target,value,count=(get(uc,sp+n) for n in (4,8,12))
                    assert DATA<=target and target+count<=DATA+0x100000
                    uc.mem_write(target,bytes([value&255])*count)
                    uc.reg_write(UC_X86_REG_EAX,target);pop=4;kind='memset'
                else:
                    pop=8 if address==0x51f770 else 4
                    kind='UI refresh' if address!=0x5279b0 else 'applicant query observation; no network'
                calls.append(dict(va=hex(address),kind=kind))
                uc.reg_write(UC_X86_REG_EIP,get(uc,sp));uc.reg_write(UC_X86_REG_ESP,sp+pop);return
            assert BASE<=address<BASE+len(mapped),(label,hex(address))
            assert bytes(uc.mem_read(address,size))==mapped[address-BASE:address-BASE+size]
            steps+=1
        handle=m.hook_add(UC_HOOK_CODE,hook)
        m.emu_start(start,STOP,count=1000000);m.hook_del(handle)
        assert m.reg_read(UC_X86_REG_EIP)==end,(label,hex(m.reg_read(UC_X86_REG_EIP)))
        assert get(m,0)!=100,label
        runs.append(dict(label=label,start_va=hex(start),end_va=hex(end),original_instructions=steps,adapter_calls=calls,rows=len(rows)))
        return rows,calls

    fixtures_path=ROOT/'tests/clan_roster_packets.txt'
    fixtures=[line.split() for line in fixtures_path.read_text('utf-8').splitlines()]
    writers={1011:0x527890,1013:0x527910,1069:0x5279b0,1102:0x528460}
    readers={1012:(0x52b4b0,0x52bf8c),1014:(0x52c010,0x52c70a),1070:(0x52c750,0x52ccf4)}
    for label,mid,body,frame in fixtures:
        mid=int(mid);body=b'' if body=='-' else bytes.fromhex(body)
        m=machine(mid,body)
        if mid in writers:
            if mid==1102:
                clan,mode=struct.unpack('<II',body);put(m,STACK+4,clan);put(m,STACK+8,mode)
            execute(m,label,mid,writers[mid],0x1114270)
            packet=get(m,m.reg_read(UC_X86_REG_ESP)+4)
            assert bytes(m.mem_read(get(m,packet+4),len(body)+4))==struct.pack('<HH',len(body),mid)+body,label
        elif mid in readers:
            rows,_=execute(m,label,mid,*readers[mid])
            prefix=8 if mid==1014 else 4
            assert len(rows)==struct.unpack_from('<I',body,prefix-4)[0]
            assert b''.join(rows)==body[prefix:],label
            assert get(m,OBJECT+65552)==HEAP+4+len(body),label
        else:
            assert mid==1103
            value,result,mode=struct.unpack('<III',body)
            put(m,MANAGER+160,0x12345678)
            _,calls=execute(m,label,mid,0x525170,0x525c81)
            assert get(m,OBJECT+65552)==HEAP+4+len(body)
            assert get(m,MANAGER+160)==(mode if result else 0x12345678)
            assert sum(c['va']=='0x5279b0' for c in calls)==int(result!=0 and mode==0)
    report=dict(passed=True,sample_sha256=digest,evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
        fixture_sha256=hashlib.sha256(fixtures_path.read_bytes()).hexdigest(),fixture_count=len(fixtures),
        resources=len(evidence['resources']),functions=len(evidence['functions']),chunks=sum(len(f['chunks']) for f in evidence['functions']),runs=runs,
        limits='List entry/field/cursor/count-loop instructions execute, but post-read row container construction/destruction is skipped at documented instruction boundaries. Ordinary S1012 branch only. S1103 applies original manager state; UI refresh and nested query calls are observed adapters. No full containers, UI, native sockets or malformed native packet execution.')
    (ROOT/'docs/evidence/clan_roster_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=True,runs=len(runs),instructions=sum(r['original_instructions'] for r in runs),
        adapters=sum(len(r['adapter_calls']) for r in runs),rows=sum(r['rows'] for r in runs))))


if __name__=='__main__':main()
