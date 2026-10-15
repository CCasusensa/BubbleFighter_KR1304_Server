"""BFCLIENT02 bounded original block writers/readers; synthetic identities.
Virtual packet I/O and string/tree/vector allocation helpers are adapters.
No executable or client resource is changed.
"""
import argparse,hashlib,json,struct
from pathlib import Path
import pefile
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import *
ROOT=Path(__file__).resolve().parents[1]
BASE=0x400000;DATA=0x20000000;BP=DATA+0x10000;STACK=BP-0x1000
VT=DATA+0x100;IO=DATA+0x200;HEADER=DATA+0x300;OBJ=DATA+0x400;ARG=DATA+0x1000;OUT=DATA+0x2000
def put(m,a,*v):m.mem_write(a,struct.pack('<'+'I'*len(v),*v))
def get(m,a):return struct.unpack('<I',m.mem_read(a,4))[0]
def wire(s):
 b=s.encode('utf-16-le');return struct.pack('<I',len(b)//2)+b
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--sample',type=Path,required=True);args=parser.parse_args()
 raw=args.sample.read_bytes();digest=hashlib.sha256(raw).hexdigest()
 assert digest=='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
 pe=pefile.PE(data=raw);assert pe.FILE_HEADER.Machine==0x14c and pe.OPTIONAL_HEADER.ImageBase==BASE; mapped=pe.get_memory_mapped_image()
 ranges=json.loads((ROOT/'docs/evidence/social_native.json').read_text('utf-8-sig'))
 for r in ranges:
  b=bytes.fromhex(r['Bytes']);assert pe.get_data(int(r['Rva'],16),len(b))==b
  assert hashlib.sha256(b).hexdigest()==r['Sha256']
 runs=[];vectors=[]
 def machine():
  m=Uc(UC_ARCH_X86,UC_MODE_32);m.mem_map(BASE,(len(mapped)+4095)&~4095);m.mem_write(BASE,mapped)
  m.mem_map(DATA,0x20000);m.mem_map(0,4096);m.reg_write(UC_X86_REG_EBP,BP);m.reg_write(UC_X86_REG_ESP,STACK)
  put(m,VT+44,IO);put(m,VT+48,IO);put(m,OBJ,VT);return m
 def string(m,a,s):
  b=s.encode('utf-16-le');n=len(b)//2;m.mem_write(a,b'\0'*24)
  if n<=7:m.mem_write(a,b+b'\0\0')
  else:put(m,a,DATA+0x8000);m.mem_write(DATA+0x8000,b+b'\0\0')
  put(m,a+16,n,max(7,n))
 def run(m,start,end,incoming=None):
  out=bytearray();offset=0;steps=0;native_steps=0;adapter_calls=0
  def ret(uc,pop=0):
   sp=uc.reg_read(UC_X86_REG_ESP);uc.reg_write(UC_X86_REG_EIP,get(uc,sp));uc.reg_write(UC_X86_REG_ESP,sp+4+pop)
  def hook(uc,a,size,_):
   nonlocal offset,steps,native_steps,adapter_calls
   steps+=1;sp=uc.reg_read(UC_X86_REG_ESP)
   if a==IO:
    adapter_calls+=1
    dest=get(uc,sp+4);n=get(uc,sp+8)
    if incoming is None:out.extend(uc.mem_read(dest,n))
    else:
     assert offset+n<=len(incoming);uc.mem_write(dest,incoming[offset:offset+n]);offset+=n
    ret(uc,8);return
   if a==0x1119c10:
    adapter_calls+=1
    n=struct.unpack_from('<I',incoming,offset)[0];offset+=4
    s=incoming[offset:offset+2*n].decode('utf-16-le');offset+=2*n
    dest=uc.reg_read(UC_X86_REG_ECX);string(uc,dest,s);uc.reg_write(UC_X86_REG_EAX,dest);ret(uc);return
   native_steps+=1
   assert start<=a<end,hex(a)
   assert bytes(uc.mem_read(a,size))==mapped[a-BASE:a-BASE+size]
  m.hook_add(UC_HOOK_CODE,hook);m.emu_start(start,end,count=2000);assert m.reg_read(UC_X86_REG_EIP)==end
  if incoming is not None:assert offset==len(incoming)
  runs.append(dict(StartVA=hex(start),EndVA=hex(end),HookSteps=steps,OriginalInstructions=native_steps,AdapterCalls=adapter_calls,Consumed=offset));return bytes(out)
 for ident,name in [(None,'Bob'),(42,'角色'),(0xfedcba98,'LongPlayerName'),(13,'😀')]:
  expected=(b'\x3a'+struct.pack('<I',ident) if ident is not None else b'\xba')+wire(name)
  for mid,start,end,obj,scalar,flag,s in [(147,0xee78b0,0xee7947,-0x88,-0x44,-0x40,-0x3c),(148,0xee752b,0xee75aa,-0x74,-0x48,-0x44,-0x40)]:
   m=machine();put(m,BP+obj,VT);put(m,BP+obj+24,HEADER);put(m,BP+scalar,ident or 0);put(m,BP+flag,int(ident is not None));string(m,BP+s,name)
   out=run(m,start,end);assert out==expected;assert get(m,HEADER+4)&0xffff==mid;vectors.append((mid,out))
  # Native S171 optional-id primitive consumes the original marker/id prefix.
  m=machine();put(m,ARG,OBJ);put(m,STACK,DATA+0x200,OUT);m.reg_write(UC_X86_REG_ECX,ARG)
  prefix=expected[:5] if ident is not None else expected[:1]
  run(m,0x10c0640,0x10c0699,prefix);assert m.mem_read(OUT+4,1)[0]==int(ident is not None)
  if ident is not None:assert get(m,OUT)==ident
  vectors.append((171,expected))
 for ident,name in [(42,'角色'),(0xfedcba98,'LongPlayerName')]:
  m=machine();m.reg_write(UC_X86_REG_EBX,ARG);put(m,ARG+12,OBJ)
  expected=struct.pack('<I',ident)+wire(name);run(m,0x9d0ce9,0x9d0d0e,expected)
  assert get(m,BP-0x5c)==ident;vectors.append((172,expected))
 # Original S170 prefix reads a uint32 count; entry shape is separately recorded from the loop.
 m=machine();m.reg_write(UC_X86_REG_EDX,ARG);m.reg_write(UC_X86_REG_ECX,OUT);put(m,ARG,OBJ)
 run(m,0x10c4c71,0x10c4c9d,b'\0'*4);assert get(m,BP-0x20)==0;vectors.append((170,b'\0'*4))
 fixture=ROOT/'tests/social_block_packets.txt'
 expected=[line for line in fixture.read_text('utf-8-sig').splitlines() if line and not line.startswith('#')]
 assert expected==[str(i)+'|'+b.hex() for i,b in vectors]
 report=dict(Scope=__doc__,SampleSha256=digest,VerifiedRanges=len(ranges),Runs=runs,Fixtures=len(vectors),FixtureSha256=hashlib.sha256(fixture.read_bytes()).hexdigest())
 (ROOT/'docs/evidence/social_block_probe.json').write_text(json.dumps(report,indent=2)+'\n','utf-8')
 print(json.dumps(dict(Runs=len(runs),Fixtures=len(vectors),VerifiedRanges=len(ranges))))
if __name__=='__main__':main()
