"""Execute recovered gift result/color/button consumers with synthetic state only."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_ESI, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x30000000
STACK, BP, OBJECT, STOP = [DATA + n for n in (0x8000,0x10000,0x20000,0x7f000)]
PALETTE = [0xffe82706,0xff1379db,0xffe82706,0xff7c5dff,0xff30db02,
           0xffbfe836,0xfff6a822,0xfff145a6,0xff858689,0xffe82706,
           0xff00ebde,0xff4b9a34,0xff383838,0xffff9dbd,0xffff8c43,
           0xffffdf62,0xffeee4d4,0xff4b4141,0xff7a761f,0xffce2900]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    mapped = pe.get_memory_mapped_image()
    evidence = json.loads((ROOT/'docs/evidence/gift_consumers_ida.json').read_text(encoding='utf-8'))
    verified = []
    addresses = set()
    for collection in ('functions','resources','ranges'):
        for row in evidence[collection]:
            block = bytes.fromhex(row['bytes'])
            assert pe.get_data(int(row['rva'],16),len(block)) == block, row['va']
            assert hashlib.sha256(block).hexdigest() == row['sha256']
            verified.append(dict(va=row['va'],rva=row['rva'],size=len(block),sha256=row['sha256']))
            addresses.update(int(i['va'],16) for i in row.get('instructions',[]))
    runs = []

    def put(m,address,value): m.mem_write(address,struct.pack('<I',value & 0xffffffff))
    def get(m,address): return struct.unpack('<I',m.mem_read(address,4))[0]
    def machine():
        m=Uc(UC_ARCH_X86,UC_MODE_32)
        m.mem_map(BASE,(len(mapped)+4095)&~4095);m.mem_write(BASE,mapped)
        m.mem_map(DATA,0x800000);m.mem_map(0,0x1000)
        m.reg_write(UC_X86_REG_ESP,STACK);m.reg_write(UC_X86_REG_EBP,BP)
        put(m,STACK,STOP)
        return m
    def execute(m,start,end,kind,expected=None,patched=False):
        executed=[];replacement=[]
        def hook(m,address,size,_):
            if address==end:
                m.emu_stop();return
            if patched and 0x9fcb54<=address<0x9fcb5b: replacement.append(address)
            else:
                assert address in addresses, hex(address)
                executed.append(address)
        handle=m.hook_add(UC_HOOK_CODE,hook)
        m.emu_start(start,end,count=1000)
        assert m.reg_read(UC_X86_REG_EIP)==end
        m.hook_del(handle)
        runs.append(dict(kind=kind,expected=expected,instructions=len(executed),
                         replacement_instructions=len(replacement),adapters=0))
    failures = {-77:0x15b5748,-76:0x15b56f8,-51:0x15b576c,-18:0x15b58ec,
                -17:0x15b58ac,-15:0x15b5724,-11:0x15b5664,-3:0x15b5680,
                -2:0x15b56a4,-1:0x15b56d4,2:0x15ab780,4:0x15ab7b8,
                8:0x15ab7f8,9:0x15ab828,27:0x15b5818,30:0x15abb4c,
                501:0x15b5860,502:0x15aba38,99:0x15b5798,-999:0x15b5798}
    for code,resource in failures.items():
        m=machine();put(m,BP-0x11c,code)
        execute(m,0x9ba05b,0x40daa0,'send_failure_resource',code)
        assert get(m,m.reg_read(UC_X86_REG_ESP)+4)==resource,code
    for color in range(21):
        for enabled in (0,1):
            m=machine();m.reg_write(UC_X86_REG_ESI,OBJECT)
            put(m,BP+12,color);put(m,BP+16,enabled)
            execute(m,0x4b9c15,0x4b9c33,'thanks_color',dict(color=color,enabled=enabled))
            selected=color if enabled else 2
            assert get(m,OBJECT+0x2fc)==selected
            assert get(m,OBJECT+0x300)==(PALETTE[selected] if selected<20 else PALETTE[0])
    profile=json.loads((ROOT/'tools/client_runtime/profiles.json').read_text(encoding='utf-8'))['ShopFeatures']['CashPurchase']
    assert profile['Rva']=='0x005fcb54' and profile['ExpectedBytes']=='8d0445ffffffff'
    assert profile['ReplacementBytes']=='90909090909090'
    for flag in (0,1,255):
        for patched in (False,True):
            m=machine();m.reg_write(UC_X86_REG_EBX,OBJECT);m.mem_write(OBJECT+202,bytes([flag]))
            if patched:m.mem_write(0x9fcb54,bytes.fromhex(profile['ReplacementBytes']))
            execute(m,0x9fcb49,0x9fcb5b,'cash_gift_mode',dict(flag=flag,patched=patched),patched)
            assert m.reg_read(UC_X86_REG_EAX)==(int(flag==0) if patched else 1 if flag==0 else 0xffffffff)
        m=machine();m.reg_write(UC_X86_REG_EBX,OBJECT);m.mem_write(OBJECT+202,bytes([flag]))
        execute(m,0x9fcbb6,0x9fcbc1,'unchanged_lucci_cashcn_mode',flag)
        assert m.reg_read(UC_X86_REG_EAX)==int(flag==0)
    for mode in (0xffffffff,0,1,2):
        for category in (1100,1300):
            m=machine()
            buy,gift,manager=OBJECT+0x4000,OBJECT+0x6000,OBJECT+0x8000
            put(m,0x18e6850,manager);put(m,OBJECT+427*4,mode)
            put(m,OBJECT+438*4,category);put(m,OBJECT+411*4,buy);put(m,OBJECT+410*4,gift)
            m.reg_write(UC_X86_REG_ECX,OBJECT)
            execute(m,0xe1d3f0,STOP,'native_dialog_buttons',dict(mode=mode,category=category))
            assert m.mem_read(buy+908,1)[0]==int(mode in (0,1))
            assert m.mem_read(gift+908,1)[0]==int(mode==0 and category!=1300)
    report=dict(passed=True,sample_sha256=hashlib.sha256(raw).hexdigest(),blocks=verified,runs=runs,
                evidence_sha256=hashlib.sha256((ROOT/'docs/evidence/gift_consumers_ida.json').read_bytes()).hexdigest(),
                profile_sha256=hashlib.sha256((ROOT/'tools/client_runtime/profiles.json').read_bytes()).hexdigest(),
                original_instructions=sum(r['instructions'] for r in runs),
                replacement_instructions=sum(r['replacement_instructions'] for r in runs),
                boundary_adapters=0,original_client_started=False,
                scope='Bounded failure message selection, palette application and shop gift gates; no full UI, SDK or network.')
    (ROOT/'docs/evidence/gift_consumers_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(f"PASS: {len(verified)} PE blocks, {len(runs)} runs, {report['original_instructions']} original instructions, {report['replacement_instructions']} replacement instructions, zero adapters.")


if __name__ == '__main__':
    main()

