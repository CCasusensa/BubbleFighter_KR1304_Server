"""Check C1019/S1020 against bounded BFCLIENT02 instructions; never rewrites fixtures."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT=Path(__file__).resolve().parents[1]
BASE,DATA=0x400000,0x20000000
OBJECT,HEAP,TEXT,STACK,STOP=[DATA+n for n in (0x10000,0x30000,0x60000,0xe0000,0xff000)]

def put(m,a,v):m.mem_write(a,struct.pack('<I',v))
def get(m,a):return struct.unpack('<I',m.mem_read(a,4))[0]
def wide(m,a):
 n=get(m,a+16);p=get(m,a) if get(m,a+20)>7 else a
 return bytes(m.mem_read(p,n*2))+b'\0\0'

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--sample',type=Path,required=True)
 args=parser.parse_args()
 raw=args.sample.read_bytes();digest=hashlib.sha256(raw).hexdigest()
 assert digest=='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
 pe=pefile.PE(data=raw)
 assert pe.FILE_HEADER.Machine==0x14c and pe.OPTIONAL_HEADER.ImageBase==BASE
 evidence_path=ROOT/'docs/evidence/clan_directory_ida.json'
 evidence=json.loads(evidence_path.read_text('utf-8'))
 blocks=list(evidence['resources'])
 for f in evidence['functions']:blocks.extend(f['chunks'])
 for b in blocks:
  blob=bytes.fromhex(b['bytes'])
  assert hashlib.sha256(blob).hexdigest()==b['sha256']
  assert pe.get_data(int(b['rva'],16),len(blob))==blob
 mapped=pe.get_memory_mapped_image();runs=[]
 fixture_path=ROOT/'tests/clan_directory_packets.txt'
 fixtures=[line.split() for line in fixture_path.read_text('utf-8').splitlines()]
 for label,mid,body,frame in fixtures:
  mid=int(mid);body=bytes.fromhex(body)
  m=Uc(UC_ARCH_X86,UC_MODE_32)
  m.mem_map(BASE,(len(mapped)+4095)&~4095);m.mem_write(BASE,mapped)
  m.mem_map(DATA,0x100000);m.mem_map(0,0x1000)
  m.reg_write(UC_X86_REG_EBP,STACK);m.reg_write(UC_X86_REG_ESP,STACK)
  put(m,STACK,STOP);put(m,STACK+4,OBJECT)
  put(m,OBJECT+4,HEAP);put(m,OBJECT+8,HEAP+2)
  put(m,OBJECT+65548,HEAP+4);put(m,OBJECT+65552,HEAP+4)
  put(m,OBJECT+65560,HEAP+4+len(body))
  m.mem_write(HEAP,struct.pack('<HH',len(body),mid)+body)
  m.reg_write(UC_X86_REG_ECX,OBJECT)
  if mid==1019:
   mode,page=struct.unpack_from('<II',body)
   put(m,STACK+4,mode);put(m,STACK+8,page);put(m,STACK+12,TEXT)
   n=len(body[8:])//2-1
   if n>7:m.mem_write(TEXT+0x100,body[8:]);put(m,TEXT,TEXT+0x100)
   else:m.mem_write(TEXT,body[8:])
   put(m,TEXT+16,n);put(m,TEXT+20,max(n,7))
  start,end=(0x526020,0x1114270) if mid==1019 else (0x5290a0,0x529ca6)
  steps,next_heap=0,TEXT+0x1000
  calls,rows=[],[]
  skipped=None
  def hook(uc,a,size,_):
   nonlocal steps,next_heap,skipped
   if a==end:uc.emu_stop();return
   bp=uc.reg_read(UC_X86_REG_EBP);sp=uc.reg_read(UC_X86_REG_ESP)
   if mid==1020 and a==0x529452:
    # Native advances over this DWORD without loading its value.
    skipped=bytes(uc.mem_read(get(uc,OBJECT+65552),4))
   if mid==1020 and a==0x529549:
    row=bytes(uc.mem_read(bp-0xdc,4))+wide(uc,bp-0x98)
    row+=bytes(uc.mem_read(bp-0xe4,4))+wide(uc,bp-0x80)
    for off in (0xe0,0x18):row+=bytes(uc.mem_read(bp-off,4))
    for off in (0x100,0x108):row+=bytes(uc.mem_read(bp-off,8))
    for off in (0x20,0x1c):row+=bytes(uc.mem_read(bp-off,4))
    assert skipped is not None
    row+=skipped+wide(uc,bp-0xb0)
    for off in (0xf8,0x110):row+=bytes(uc.mem_read(bp-off,8))
    row+=bytes(uc.mem_read(bp-0x28,4));rows.append(row);skipped=None
    calls.append(dict(va=hex(a),kind='omit date conversion and post-read row containers/destructors',resume_va='0x529c88'))
    uc.reg_write(UC_X86_REG_EIP,0x529c88);return
   if a in (0x12966c0,0x1296140,0x40c2e0):
    if a==0x40c2e0:
     target=uc.reg_read(UC_X86_REG_ECX);source,n=get(uc,sp+4),get(uc,sp+8)
     blob=bytes(uc.mem_read(source,n*2))+b'\0\0'
     if n>7:
      assert next_heap+len(blob)<STACK-0x20000
      uc.mem_write(next_heap,blob);put(uc,target,next_heap);next_heap+=len(blob)+16
     else:uc.mem_write(target,blob)
     put(uc,target+16,n);put(uc,target+20,max(n,7));pop=12;kind='wstring assignment after native Z scan'
    else:
     target,source,n=(get(uc,sp+i) for i in (4,8,12))
     assert DATA<=target and target+n<DATA+0x100000
     blob=bytes([source&255])*n if a==0x12966c0 else bytes(uc.mem_read(source,n))
     uc.mem_write(target,blob);pop=4;kind='memset' if a==0x12966c0 else 'memcpy'
    uc.reg_write(UC_X86_REG_EAX,target)
    calls.append(dict(va=hex(a),kind=kind))
    uc.reg_write(UC_X86_REG_EIP,get(uc,sp));uc.reg_write(UC_X86_REG_ESP,sp+pop);return
   assert BASE<=a<BASE+len(mapped),(label,hex(a))
   assert bytes(uc.mem_read(a,size))==mapped[a-BASE:a-BASE+size]
   steps+=1
  h=m.hook_add(UC_HOOK_CODE,hook);m.emu_start(start,STOP,count=1000000);m.hook_del(h)
  assert m.reg_read(UC_X86_REG_EIP)==end,(label,hex(m.reg_read(UC_X86_REG_EIP)))
  assert get(m,0)!=100,label
  if mid==1019:
   packet=get(m,m.reg_read(UC_X86_REG_ESP)+4)
   assert bytes(m.mem_read(get(m,packet+4),len(body)+4))==struct.pack('<HH',len(body),mid)+body,label
  else:
   bp=m.reg_read(UC_X86_REG_EBP)
   header=struct.pack('<III',get(m,bp-0x4c),get(m,bp-0xec),get(m,bp-0xe8))+wide(m,bp-0xd8)
   assert len(rows)==get(m,bp-0x4c)
   assert header+b''.join(rows)==body,label
   assert get(m,OBJECT+65552)==HEAP+4+len(body),label
  runs.append(dict(label=label,message_id=mid,start_va=hex(start),end_va=hex(end),original_instructions=steps,adapter_calls=calls,rows=len(rows)))
 report=dict(passed=True,sample_sha256=digest,evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(fixture_path.read_bytes()).hexdigest(),fixture_count=len(fixtures),functions=len(evidence['functions']),chunks=sum(len(f['chunks']) for f in evidence['functions']),resources=len(evidence['resources']),runs=runs,
 limits='Original request writes and response header/field reads, UTF-16Z scans, cursor/count loops execute. Skipped DWORD is observed before original cursor advance. Date conversion, record/vector construction/destruction and UI are bypassed after all reads. CRT/string assignment adapted; no native sockets, full containers, malformed-native packets or UI.')
 (ROOT/'docs/evidence/clan_directory_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
 print(json.dumps(dict(passed=True,runs=len(runs),instructions=sum(r['original_instructions'] for r in runs),adapters=sum(len(r['adapter_calls']) for r in runs),rows=sum(r['rows'] for r in runs))))

if __name__=='__main__':main()
