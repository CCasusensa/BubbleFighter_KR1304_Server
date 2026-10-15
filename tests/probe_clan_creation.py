"""Bounded native S1028 -> C806 preparation and Function=22 response paths."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x30000000
BP, STACK, ARGS, MANAGER, PACKET, VTABLE, HEADER = [DATA+n for n in (0x20000,0x18000,0x22000,0x24000,0x28000,0x2a000,0x2c000)]
WRITE, STOP = DATA+0x2e000, DATA+0x2f000


def put(m, address, value): m.mem_write(address,struct.pack('<I',value & 0xffffffff))
def get(m, address): return struct.unpack('<I',m.mem_read(address,4))[0]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample',type=Path,required=True)
    args=parser.parse_args()
    raw=args.sample.read_bytes()
    evidence_path=ROOT/'docs/evidence/clan_creation_ida.json'
    evidence=json.loads(evidence_path.read_text('utf-8'))
    assert hashlib.sha256(raw).hexdigest()==evidence['sample_sha256']
    pe=pefile.PE(data=raw)
    assert pe.OPTIONAL_HEADER.ImageBase==BASE and pe.FILE_HEADER.Machine==0x14c
    chunks=[chunk for row in evidence['functions'] for chunk in row['chunks']]
    for chunk in chunks:
        blob=bytes.fromhex(chunk['bytes'])
        assert hashlib.sha256(blob).hexdigest()==chunk['sha256']
        assert pe.get_data(int(chunk['rva'],16),len(blob))==blob
    mapped=pe.get_memory_mapped_image()
    runs=[]
    def machine():
        m=Uc(UC_ARCH_X86,UC_MODE_32)
        m.mem_map(BASE,(len(mapped)+4095)&~4095);m.mem_write(BASE,mapped)
        m.mem_map(DATA,0x40000)
        m.mem_map(0,0x1000)  # Synthetic x86 SEH chain, not a live process TEB.
        for reg,value in ((UC_X86_REG_EBP,BP),(UC_X86_REG_ESP,STACK),(UC_X86_REG_EBX,ARGS),(UC_X86_REG_ESI,MANAGER)):
            m.reg_write(reg,value)
        put(m,0x18e55d4,MANAGER);put(m,0x18e7f48,MANAGER)
        return m

    def execute(m,label,start,end,adapter=None):
        steps=0;calls=[]
        def hook(uc,address,size,unused):
            nonlocal steps
            if adapter and adapter(uc,address,calls): return
            assert BASE<=address<BASE+len(mapped),(label,hex(address))
            assert bytes(uc.mem_read(address,size))==mapped[address-BASE:address-BASE+size]
            steps+=1
        handle=m.hook_add(UC_HOOK_CODE,hook)
        m.emu_start(start,end,count=100000)
        m.hook_del(handle)
        assert m.reg_read(UC_X86_REG_EIP)==end,(label,hex(m.reg_read(UC_X86_REG_EIP)))
        runs.append(dict(label=label,start_va=hex(start),end_va=hex(end),original_instructions=steps,adapter_calls=calls))

    fixtures={name:(int(mid),bytes.fromhex(body)) for name,mid,body in (line.split() for line in (ROOT/'tests/clan_creation_packets.txt').read_text('ascii').splitlines())}
    for label in ('creation_card','opaque_item','upper_item'):
        _, response=fixtures[label+'_create'];result,item,opaque=struct.unpack('<III',response)
        m=machine();put(m,ARGS+12,item);put(m,ARGS+16,opaque)
        # Execute the original success call site AND obfuscated 805C50 chunks.
        # Stop at the packet constructor before allocation or socket operations.
        execute(m,label+'_prepare',0x51811a,0x80da80)
        record=get(m,m.reg_read(UC_X86_REG_ESP)+4)
        assert [get(m,record+n) for n in (0,4,8,12,16)]==[item,1,0,0,0]
        # Native packet constructor's fixed writer body with virtual stream I/O
        # adapter; the empty vector writer executes original instructions.
        output=bytearray()
        put(m,PACKET,VTABLE);put(m,PACKET+24,HEADER);put(m,VTABLE+48,WRITE)
        put(m,BP+8,record)
        m.reg_write(UC_X86_REG_EBP,BP)
        m.reg_write(UC_X86_REG_EBX,ARGS);m.reg_write(UC_X86_REG_EDI,PACKET)
        m.reg_write(UC_X86_REG_ESP,STACK-0x2000)
        def write(uc,address,calls):
            if address!=WRITE:return False
            sp=uc.reg_read(UC_X86_REG_ESP);ptr=get(uc,sp+4);count=get(uc,sp+8)
            assert count<=64
            output.extend(uc.mem_read(ptr,count))
            calls.append(dict(kind='virtual packet write',return_va=hex(get(uc,sp)),bytes=count))
            uc.reg_write(UC_X86_REG_EAX,count);uc.reg_write(UC_X86_REG_EIP,get(uc,sp));uc.reg_write(UC_X86_REG_ESP,sp+12)
            return True
        execute(m,label+'_writer',0x80dad2,0x80db15,write)
        assert bytes(output)==fixtures[label+'_confirm'][1]
        assert bytes(m.mem_read(HEADER+4,2))==struct.pack('<H',806)
    # Function=22 has no dedicated UI branch. Result zero reaches generic item
    # telemetry when its manager exists; all other tested results skip it.
    for result in (0,1,22,0xffffffff):
        for telemetry in (0,1):
            m=machine();put(m,MANAGER+252,22);put(m,BP-72,result);put(m,BP-68,100819)
            put(m,0x18e5798,MANAGER if telemetry else 0)
            m.reg_write(UC_X86_REG_EAX,MANAGER)
            end=0xd93b90 if result==0 and telemetry else 0x9b60d2
            execute(m,f'function22_result{result}_telemetry{telemetry}',0x9b419b,end)
            if end==0xd93b90:assert get(m,m.reg_read(UC_X86_REG_ESP)+4)==100819
    report=dict(passed=True,sample_sha256=evidence['sample_sha256'],evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
                functions=len(evidence['functions']),chunks=len(chunks),fixture_count=len(fixtures),runs=runs,original_ui=False,socket_execution=False,
                limits='Success fragment begins after UI dialog cleanup; executes obfuscated preparation to packet constructor. Packet writer uses virtual I/O adapter. Function 22 response stops before telemetry/destructor. No original server semantics or inventory authority inferred.')
    (ROOT/'docs/evidence/clan_creation_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=True,runs=len(runs),instructions=sum(r['original_instructions'] for r in runs),adapters=sum(len(r['adapter_calls']) for r in runs))))


if __name__=='__main__':main()
