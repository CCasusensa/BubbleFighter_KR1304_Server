"""Current PE/IDA range comparison and bounded native C513 writer executions."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_EBP

ROOT=Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample',type=Path,required=True)
    parser.add_argument('--packed-sample',type=Path,required=True)
    args=parser.parse_args()
    path=ROOT/'docs/evidence/room_configuration_native.json'
    captured=json.loads(path.read_text('utf-8'))
    native={'Functions':{row['va']:{'VA':row['va'],'Bytes':row['bytes'],'Sha256':row['sha256']}
                         for row in captured['functions']},
            'SampleSha256':captured['sample_sha256'],
            'OriginalSha256':'311b06b65d0a4873f16d28d4bb93264884cf4aa9ec420caa41385f40497b04ee'}
    sample=args.sample
    assert sha(sample)==native['SampleSha256']=='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    assert sha(args.packed_sample)==native['OriginalSha256']
    pe=pefile.PE(str(sample)); base=pe.OPTIONAL_HEADER.ImageBase
    assert base==0x400000 and pe.FILE_HEADER.Machine==0x14c
    ranges=[]
    for row in native['Functions'].values():
        start=int(row['VA'],16); raw=bytes.fromhex(row['Bytes'])
        assert pe.get_data(start-base,len(raw))==raw
        assert hashlib.sha256(raw).hexdigest()==row['Sha256']
        ranges.append((start,start+len(raw)))
    mapped=pe.get_memory_mapped_image(); m=Uc(UC_ARCH_X86,UC_MODE_32)
    m.mem_map(base,(len(mapped)+4095)&~4095);m.mem_write(base,mapped)
    area=0x20000000;m.mem_map(area,0x80000)
    obj=area+0x1000;stream=area+0x2000;vtable=area+0x3000;pointer=area+0x4000
    stack=area+0x70000;stop=area+0x71000;writer=area+0x72000
    def put(at,*values):m.mem_write(at,struct.pack('<'+'I'*len(values),*values))
    def get(at):return struct.unpack('<I',m.mem_read(at,4))[0]
    put(stream,vtable);put(vtable+48,writer);put(pointer,stream)
    output=bytearray();instructions=0;runs=[];vectors=[]
    def hook(uc,va,size,_):
        nonlocal instructions
        if va==writer:
            sp=m.reg_read(UC_X86_REG_ESP);output.extend(m.mem_read(get(sp+4),get(sp+8)))
            m.reg_write(UC_X86_REG_EIP,get(sp));m.reg_write(UC_X86_REG_ESP,sp+12);return
        assert 0x5E14E1<=va<0x5E14F6 or 0x10D3F80<=va<0x10D3F80+len(bytes.fromhex(native['Functions']['0x10d3f80']['Bytes']))
        assert bytes(m.mem_read(va,size))==mapped[va-base:va-base+size]
        instructions+=1
    m.hook_add(UC_HOOK_CODE,hook)
    # Execute the exact constructor fragment that assigns the packet ID and
    # prepares the writer arguments. Do not enter unrelated allocation/SEH code.
    frame=area+0x60000;header=area+0x61000;put(stream+24,header);put(frame+8,obj)
    m.reg_write(UC_X86_REG_ESI,stream);m.reg_write(UC_X86_REG_EBP,frame)
    m.emu_start(0x5E14E1,0x5E14F6,count=100)
    assert m.reg_read(UC_X86_REG_EIP)==0x5E14F6 and get(header+4)&0xffff==513
    assert m.reg_read(UC_X86_REG_ECX)==obj and get(m.reg_read(UC_X86_REG_EDX))==stream
    runs.append(dict(Name='OriginalC513ConstructorPrefix',Instructions=instructions,MessageId=513))
    cases=[('Empty',['','',''],[0]*13),('Random',['Random','','Random'],[0,0,0,0,0,0,0,0,1,1,0,0,1]),
           ('Distinct',['地图名称12345678','DirectoryLong','Key'],[255,254]+list(range(101,110))+[253,252])]
    for label,strings,values in cases:
        m.mem_write(obj,bytes(128));expected=bytearray()
        for i,text in enumerate(strings):
            raw=text.encode('utf-16-le');length=len(raw)//2;at=obj+i*24
            if length<=7:m.mem_write(at,raw+b'\0\0');cap=7
            else:
                heap=area+0x5000+i*0x1000;m.mem_write(heap,raw+b'\0\0');put(at,heap);cap=length
            put(at+16,length,cap);expected.extend(struct.pack('<I',length)+raw)
        m.mem_write(obj+72,bytes(values[:2]));put(obj+76,*values[2:11]);m.mem_write(obj+112,bytes(values[11:]))
        expected.extend(struct.pack('<13I',*values));output.clear();before=instructions
        put(stack,stop);m.reg_write(UC_X86_REG_ESP,stack);m.reg_write(UC_X86_REG_ECX,obj);m.reg_write(UC_X86_REG_EDX,pointer)
        m.emu_start(0x10D3F80,stop,count=10000)
        assert m.reg_read(UC_X86_REG_EIP)==stop and output==expected
        vectors.append(label+' '+output.hex());runs.append(dict(Name=label,Instructions=instructions-before,Bytes=len(output)))
    fixture=ROOT/'tests/room_configuration_packets.txt'
    assert fixture.read_text('utf-8').splitlines()==vectors, 'Original writer disagrees with frozen historical fixtures'
    report=dict(SampleSha256=sha(sample),OriginalSha256=native['OriginalSha256'],Architecture='x86',ImageBase=hex(base),
                VerifiedRanges=len(ranges),Executions=len(runs),Instructions=instructions,Runs=runs,
                FixtureSha256=sha(fixture),Adapters=['Synthetic UTF16 objects and virtual stream writer'],OriginalClientUI=False)
    (ROOT/'docs/evidence/room_configuration_probe.json').write_text(json.dumps(report,indent=2)+'\n','utf-8')
    print(json.dumps(report))
if __name__=='__main__':main()
