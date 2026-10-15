"""Execute the original main C2025 writer body against fixed independent bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x30000000
BP, STACK, HEADER, VTABLE, TEXT, WRITE = [DATA+n for n in (0x10000,0x8000,0x12000,0x14000,0x16000,0x18000)]


def put(m, address, value): m.mem_write(address, struct.pack('<I', value & 0xffffffff))
def get(m, address): return struct.unpack('<I', m.mem_read(address, 4))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    evidence_path = ROOT/'docs/evidence/clan_membership_business_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    raw = args.sample.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == evidence['sample_sha256']
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    blocks = list(evidence['ranges']) + list(evidence['resources'])
    for f in evidence['functions']: blocks.extend(f['chunks'])
    for b in blocks:
        blob = bytes.fromhex(b['bytes'])
        assert hashlib.sha256(blob).hexdigest() == b['sha256']
        assert pe.get_data(int(b['rva'],16), len(blob)) == blob
    mapped = pe.get_memory_mapped_image()
    fixture_path = ROOT/'tests/clan_state_report_packets.txt'
    fixtures = [line.split() for line in fixture_path.read_text('ascii').splitlines()]
    runs = []
    for label, mid, body_hex in fixtures:
        body = bytes.fromhex(body_hex)
        clan, size = struct.unpack_from('<II', body)
        name, status = body[8:8+2*size], struct.unpack_from('<I', body, 8+2*size)[0]
        for heap in (False, True):
            m = Uc(UC_ARCH_X86, UC_MODE_32)
            m.mem_map(BASE, (len(mapped)+4095)&~4095); m.mem_write(BASE, mapped)
            m.mem_map(DATA, 0x20000)
            m.reg_write(UC_X86_REG_EBP, BP); m.reg_write(UC_X86_REG_ESP, STACK)
            put(m,BP-0x64,VTABLE); put(m,VTABLE+48,WRITE); put(m,BP-0x4c,HEADER)
            put(m,BP-0x38,clan); put(m,BP-0x24,size); put(m,BP-0x20,8 if heap else 7)
            put(m,BP-0x1c,status)
            if heap: put(m,BP-0x34,TEXT)
            m.mem_write(TEXT if heap else BP-0x34, name+b'\0\0')
            output = bytearray(); calls = []; steps = [0]
            def hook(uc, address, count, unused):
                if address == WRITE:
                    sp = uc.reg_read(UC_X86_REG_ESP); ptr = get(uc,sp+4); length = get(uc,sp+8)
                    assert length <= 64
                    output.extend(uc.mem_read(ptr,length))
                    calls.append({'return_va':hex(get(uc,sp)),'bytes':length})
                    uc.reg_write(UC_X86_REG_EAX,length); uc.reg_write(UC_X86_REG_EIP,get(uc,sp)); uc.reg_write(UC_X86_REG_ESP,sp+12)
                    return
                assert BASE <= address < BASE+len(mapped)
                assert bytes(uc.mem_read(address,count)) == mapped[address-BASE:address-BASE+count]
                steps[0] += 1
            m.hook_add(UC_HOOK_CODE,hook)
            m.emu_start(0x51922c,0x519297,count=10000)
            assert m.reg_read(UC_X86_REG_EIP) == 0x519297
            assert bytes(output) == body
            assert bytes(m.mem_read(HEADER+4,2)) == struct.pack('<H',int(mid))
            runs.append({'label':label,'heap_string':heap,'original_instructions':steps[0],'adapter_calls':calls})
    result = {'passed':True,'sample_sha256':evidence['sample_sha256'],
              'evidence_sha256':hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
              'fixture_sha256':hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
              'functions':len(evidence['functions']),'chunks':sum(len(f['chunks']) for f in evidence['functions']),
              'ranges':len(evidence['ranges']),'resources':len(evidence['resources']),'fixture_count':len(fixtures),
              'runs':runs,'start_va':'0x51922c','end_va':'0x519297',
              'limits':'Original packet-id/field/length/inline-or-heap selection instructions execute. Virtual packet writes adapted. Constructors, global manager collection, telemetry, socket, destructor and UI skipped. Other exported functions are PE-verified static evidence only.'}
    (ROOT/'docs/evidence/clan_state_report_probe.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'passed':True,'runs':len(runs),'instructions':sum(r['original_instructions'] for r in runs),'adapters':sum(len(r['adapter_calls']) for r in runs)}))


if __name__ == '__main__': main()
