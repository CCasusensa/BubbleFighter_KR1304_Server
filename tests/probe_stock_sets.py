"""Bounded original StockSet writers and decisions; no client process or UI."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import pefile
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_EIP, UC_X86_REG_ESP
from probe_gift_protocol import Machine, get, put, DATA, STACK, BP, OBJECT, INPUT, STREAM, HEADER, STOP

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    mapped = pe.get_memory_mapped_image()
    evidence_path = ROOT/'docs/evidence/stock_set_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    verified, addresses = [], set()
    for kind in ('functions', 'resources', 'ranges'):
        for row in evidence[kind]:
            block = bytes.fromhex(row['bytes'])
            assert pe.get_data(int(row['rva'],16),len(block)) == block, row['va']
            assert hashlib.sha256(block).hexdigest() == row['sha256']
            verified.append({k:row[k] for k in ('va','rva','size','sha256')})
            addresses.update(int(x['va'],16) for x in row.get('instructions',[]))
    cases = json.loads((ROOT/'tests/stock_set_packet_cases.json').read_text('utf-8'))
    fixtures = {}
    for line in (ROOT/'tests/stock_set_packets.txt').read_text('utf-8').splitlines():
        if line.startswith('#'): continue
        label, message, payload = line.split('|')
        fixtures[label] = (int(message),bytes.fromhex(payload))
    assert len(fixtures) == len(cases) == 8
    runs = []
    for case in cases:
        label, fields = case['name'], case['fields']
        m = Machine(mapped)
        if case['message_id'] == 809:
            put(m.m,INPUT,fields['stock_id'])
            # Deliberate high bytes prove only the low native byte is widened.
            put(m.m,INPUT+4,0xaabbcc00 | fields['value_at4'])
            put(m.m,STACK+4,INPUT)
            m.m.reg_write(UC_X86_REG_ECX,OBJECT)
            run = m.execute(label,0xe1d720)
            assert struct.unpack('<H',m.m.mem_read(HEADER+4,2))[0] == 809
            expected = bytes.fromhex(case['payload'])
        else:
            price_vtable = DATA+0x26000
            put(m.m,price_vtable,0x113eec0)
            def price(address, row):
                stock,item,flag,kind,amount,shown,sell = row
                put(m.m,address,price_vtable)
                put(m.m,address+4,stock);put(m.m,address+8,item)
                m.m.mem_write(address+12,bytes([flag]))
                m.m.mem_write(address+14,struct.pack('<HIII',kind,amount,shown,sell))
            put(m.m,OBJECT+4,fields['identifier']);put(m.m,OBJECT+8,1)
            m.string(OBJECT+12,fields['name'])
            for offset,value in [(36,17),(40,100),(44,107),(48,1)]:put(m.m,OBJECT+offset,value)
            m.m.mem_write(OBJECT+56,struct.pack('<QQ',0x1122334455667788,0xfedcba9876543210))
            price(OBJECT+72,fields['price'])
            begin = m.allocate(max(1,len(fields['prices'])*28))
            for i,row in enumerate(fields['prices']):price(begin+i*28,row)
            for offset,value in [(100,begin),(104,begin+len(fields['prices'])*28),(108,begin+len(fields['prices'])*28)]:put(m.m,OBJECT+offset,value)
            put(m.m,STACK+4,STREAM);m.m.reg_write(UC_X86_REG_ECX,OBJECT)
            run = m.execute(label,0x1140210)
            expected = bytes.fromhex(case['card_payload'])
            outer = struct.pack('<IIQI',1,1,9,1)+bytes(m.output)+struct.pack('<QIII',0,0,0,100)
            assert outer == fixtures[label][1]
        assert bytes(m.output) == expected
        assert fixtures[label] == (case['message_id'],bytes.fromhex(case['payload']))
        assert set(m.instructions) <= addresses
        runs.append(run)

    def bounded(machine,start,end,label,expected,lookups=None):
        instructions, adapters = [], []
        m=machine.m
        def hook(m,address,size,_):
            if address==end:
                m.emu_stop();return
            if lookups and address in (0xa00c60,0xa00ce0):
                sp=m.reg_read(UC_X86_REG_ESP)
                assert get(m,sp+4)==lookups['id']
                m.reg_write(UC_X86_REG_EAX,lookups['result'])
                m.reg_write(UC_X86_REG_EIP,get(m,sp))
                m.reg_write(UC_X86_REG_ESP,sp+8)
                adapters.append({'va':hex(address),'kind':'synthetic resource lookup'})
                return
            assert address in addresses,hex(address)
            instructions.append(address)
        handle=m.hook_add(UC_HOOK_CODE,hook)
        m.emu_start(start,end,count=2000)
        m.hook_del(handle)
        assert m.reg_read(UC_X86_REG_EIP)==end
        runs.append({'case':label,'start':hex(start),'stop':hex(end),'expected':expected,
                     'original_instructions':len(instructions),'adapter_calls':adapters})

    for identity,kind in [(0,0),(999999,11),(1000000,3),(1999999,3),(2000000,4),
                           (2000001,4),(2100001,4),(2999999,4),(3000000,0),
                           (900104901,0),(1000000000,8),(0xffffffff,0)]:
        m=Machine(mapped);m.m.reg_write(UC_X86_REG_ECX,identity)
        bounded(m,0x1135250,STOP,'identifier_class',{'id':identity,'kind':kind})
        assert m.m.reg_read(UC_X86_REG_EAX)==kind
    for mode in (0xffffffff,0,1,2):
        for category in (100,1300):
            m=Machine(mapped);buy,gift,manager=OBJECT+0x4000,OBJECT+0x6000,OBJECT+0x8000
            put(m.m,0x18e6850,manager);put(m.m,OBJECT+434*4,mode)
            put(m.m,OBJECT+446*4,category);put(m.m,OBJECT+431*4,buy);put(m.m,OBJECT+432*4,gift)
            m.m.reg_write(UC_X86_REG_ECX,OBJECT)
            bounded(m,0xe22920,STOP,'bundle_buttons',{'mode':mode,'category':category})
            assert m.m.mem_read(buy+908,1)[0]==int(mode in (0,1))
            assert m.m.mem_read(gift+908,1)[0]==int(mode==0 and category!=1300)
    for identity,found,price_at in [(1000139,True,24),(2000001,True,212),(2000001,False,212),(3000000,False,0)]:
        m=Machine(mapped);pointer=OBJECT+0x3000 if found else 0
        put(m.m,STACK+4,identity);put(m.m,pointer+price_at if found else OBJECT+0x3000,4500)
        bounded(m,0xa015f0,STOP,'price_lookup',{'id':identity,'found':found}, {'id':identity,'result':pointer})
        assert m.m.reg_read(UC_X86_REG_EAX)==(4500 if found else 0)
    # Execute the native post-CRT acceptance decision. Numeric conversion and
    # errno production are synthetic inputs, not a claim to execute wcstol.
    for value,errno in [(4500,0),(0x7fffffff,0),(0x7fffffff,34),(0x80000000,34)]:
        for allow_range in (0,1):
            for full_text in (False,True):
                m=Machine(mapped);put(m.m,BP-0x14,7);put(m.m,BP-0x18,3)
                put(m.m,BP-0x10,BP-0x28+(6 if full_text else 4));put(m.m,INPUT,errno)
                put(m.m,OBJECT,0xdeadbeef)
                m.m.reg_write(UC_X86_REG_EAX,INPUT);m.m.reg_write(UC_X86_REG_ESI,value)
                m.m.reg_write(UC_X86_REG_EDI,OBJECT);m.m.reg_write(UC_X86_REG_EBX,allow_range)
                accepted=full_text and (errno!=34 or value not in (0x7fffffff,0x80000000) or bool(allow_range))
                expected={'value':value,'errno':errno,'allow_range':allow_range,'full_text':full_text,'accepted':accepted}
                bounded(m,0x11171c1,0x11171fa,'price_parser_decision',expected)
                assert m.m.reg_read(UC_X86_REG_EBX)&0xff==int(accepted)
                assert get(m.m,OBJECT)==(value if accepted else 0xdeadbeef)
    report={'passed':True,'sample_sha256':digest,'evidence_sha256':hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
            'fixture_sha256':hashlib.sha256((ROOT/'tests/stock_set_packets.txt').read_bytes()).hexdigest(),
            'cases_sha256':hashlib.sha256((ROOT/'tests/stock_set_packet_cases.json').read_bytes()).hexdigest(),
            'shared_machine_sha256':hashlib.sha256((ROOT/'tests/probe_gift_protocol.py').read_bytes()).hexdigest(),
            'blocks':verified,'runs':runs,'original_instructions':sum(x['original_instructions'] for x in runs),
            'boundary_adapters':sum(len(x['adapter_calls']) for x in runs),'original_client_started':False,
            'scope':'Synthetic original C809/card writers, identifier/button/price-selection and post-CRT decisions. No loader, wcstol, full UI, SDK, purchase, gift or refund execution.'}
    (ROOT/'docs/evidence/stock_set_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(f"PASS: {len(verified)} PE blocks, {len(runs)} bounded runs, {report['original_instructions']} original instructions, {report['boundary_adapters']} boundary adapters.")


if __name__ == '__main__':
    main()
