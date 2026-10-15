"""E066 C832/S836 synthetic fixtures through original BFCLIENT02 x86 slices.

C832 uses the original message-ID and field writes, after client eligibility
checks and packet construction. S836 executes the original uint8 helper, result
read, pending reset and conditional active-index assignment. Virtual packet IO
and player-view lookup are adapters. No UI, gameplay, server or database runs.
"""
import argparse
import pefile
import hashlib
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX,
                              UC_X86_REG_EDI, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP)

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / 'docs/evidence/equipment_presets_native.json'
FIXTURE = ROOT / 'tests/weapon_preset_packets.txt'
REQUEST, REQUEST_END = 0x8091F9, 0x80922C
RESPONSE, RESPONSE_END, REJECTED = 0x9D5043, 0x9D509A, 0x9D3C73
STACK, FRAME, ARGS = 0x308000, 0x309000, 0x30A000
PACKET, VTABLE, HEADER = 0x201000, 0x202000, 0x203000
INVENTORY, USER, VIEW = 0x204000, 0x205000, 0x206000
IO, LOOKUP = 0x100000, 0x81CC20


def execute(rows, request, index, value):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    pages = set()

    def ensure(address, size):
        for page in range(address & ~0xFFF, (address + size + 0xFFF) & ~0xFFF, 0x1000):
            if page not in pages:
                uc.mem_map(page, 0x1000)
                pages.add(page)

    for base in (0x100000, 0x200000, 0x300000, 0x18E0000):
        ensure(base, 0x10000)
    for row in rows:
        address, raw = int(row['Va'], 16), bytes.fromhex(row['Data'])
        assert len(raw) == row['Length'] and hashlib.sha256(raw).hexdigest() == row['Sha256']
        ensure(address, len(raw))
        uc.mem_write(address, raw)
    ensure(LOOKUP, 3)
    ensure(REJECTED, 1)
    uc.mem_write(LOOKUP, b'\xc2\x04\x00')
    uc.mem_write(IO, b'\xc2\x08\x00')

    def put(address, fmt, *values):
        uc.mem_write(address, struct.pack('<' + fmt, *values))

    def read(address, fmt):
        return struct.unpack('<' + fmt, uc.mem_read(address, struct.calcsize('<' + fmt)))

    packet = FRAME - 0x80 if request else PACKET
    put(packet, 'I', VTABLE)
    put(VTABLE + (48 if request else 44), 'I', IO)
    put(FRAME - 0x68, 'I', HEADER)
    put(ARGS + 8, 'I', index)
    put(0x18E55D4, 'I', INVENTORY)
    put(0x18E56E8, 'I', USER)
    put(0x18E55D8, 'I', VIEW)
    put(INVENTORY + 208, 'B', 1)
    put(USER + 48, 'I', 77)
    put(VIEW + 28, 'B', 0xA5)
    payload = b'' if request else struct.pack('<BI', index, value)
    output, trace = bytearray(), []
    offset = 0

    def hook(machine, address, size, context):
        nonlocal offset
        if address == REJECTED:
            machine.emu_stop()
        elif address == LOOKUP:
            ret, player = read(machine.reg_read(UC_X86_REG_ESP), 'II')
            assert player == 77
            machine.reg_write(UC_X86_REG_EAX, VIEW)
        elif address == IO:
            ret, buffer, count = read(machine.reg_read(UC_X86_REG_ESP), 'III')
            assert count in (1, 4)
            if request:
                output.extend(machine.mem_read(buffer, count))
            else:
                assert count <= len(payload) - offset
                machine.mem_write(buffer, payload[offset:offset + count])
            trace.append(dict(ReturnVa=hex(ret), Offset=offset, Length=count))
            offset += count
            machine.reg_write(UC_X86_REG_EAX, count)

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(UC_X86_REG_ESP, STACK)
    uc.reg_write(UC_X86_REG_EBP, FRAME)
    uc.reg_write(UC_X86_REG_EBX, ARGS)
    uc.reg_write(UC_X86_REG_EDI, PACKET)
    uc.reg_write(UC_X86_REG_ESI, value)
    uc.emu_start(REQUEST if request else RESPONSE, REQUEST_END if request else RESPONSE_END, count=1000)
    assert uc.reg_read(UC_X86_REG_ESP) == STACK and offset == 5
    assert [x['Length'] for x in trace] == ([4, 1] if request else [1, 4])
    if request:
        assert uc.reg_read(UC_X86_REG_EIP) == REQUEST_END
        assert read(HEADER + 4, 'H')[0] == 832
        assert output == struct.pack('<IB', value, index)
        return bytes(output), trace, None
    active = read(VIEW + 28, 'B')[0]
    assert uc.reg_read(UC_X86_REG_EIP) == (RESPONSE_END if value == 0 else REJECTED)
    assert read(INVENTORY + 208, 'B')[0] == 0
    assert active == (index if value == 0 else 0xA5)
    return payload, trace, dict(Pending=0, Active=active)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample',type=Path,required=True)
    args=parser.parse_args()
    native=json.loads(EVIDENCE.read_text('utf-8'))
    helper=json.loads((ROOT/'docs/evidence/equipment_presets_reader.json').read_text('utf-8'))
    assert hashlib.sha256(args.sample.read_bytes()).hexdigest()==native['sample_sha256']=='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe=pefile.PE(str(args.sample)); base=pe.OPTIONAL_HEADER.ImageBase
    assert base==0x400000 and pe.FILE_HEADER.Machine==0x14c
    ranges=native['functions']+native['ranges']+[helper]
    for row in ranges:
        raw=bytes.fromhex(row['bytes']); va=int(row['va'],16)
        assert hashlib.sha256(raw).hexdigest()==row['sha256']
        assert pe.get_data(va-base,len(raw))==raw
    rows=[]
    for va,length in [(0x8091f9,51),(0x9d5043,87),(0x429dd0,40)]:
        containing=next(r for r in ranges if int(r['va'],16)<=va and int(r['va'],16)+len(r['bytes'])//2>=va+length)
        offset=va-int(containing['va'],16); raw=bytes.fromhex(containing['bytes'])[offset:offset+length]
        rows.append(dict(Va=hex(va),Length=length,Data=raw.hex(),Sha256=hashlib.sha256(raw).hexdigest()))
    cases, lines = [], ['# E066 synthetic original-x86 C832/S836 vectors; not a client capture.']
    inputs = [('RequestMenu' + str(i), True, i, 0xFFFFFFFF) for i in range(5)]
    inputs += [('RequestObject', True, 3, 0x12345678), ('RequestZeroObject', True, 0, 0)]
    inputs += [('ResponseSuccess' + str(i), False, i, 0) for i in range(5)]
    inputs += [('ResponseUnknown', False, 4, 0x12345678), ('ResponseMaximum', False, 255, 0xFFFFFFFF)]
    for name, request, index, value in inputs:
        payload, trace, state = execute(rows, request, index, value)
        lines.append('{}|{}|{}|{}|{}'.format(name, int(request), index, value, payload.hex()))
        cases.append(dict(Name=name, Request=request, Index=index, Value=value, PayloadHex=payload.hex(), Trace=trace, State=state))
    assert FIXTURE.read_text('utf-8').splitlines()==lines, 'Original instructions disagree with frozen historical fixtures'
    report = dict(Evidence='Current MCP and PE revalidation of E066', VerifiedRanges=len(ranges), OriginalClientUI=False, Sample='BFCLIENT02',
                  SampleSha256='7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4',
                  ImageBase='0x400000', Scope=__doc__, Source=str(EVIDENCE.relative_to(ROOT)),
                  Instructions=[{k:v for k,v in r.items() if k not in ('Data','Assembly')} for r in rows],
                  FixtureSha256=hashlib.sha256(FIXTURE.read_bytes()).hexdigest(), Cases=cases)
    with (ROOT/'docs/evidence/equipment_presets_probe.json').open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(Cases=len(cases), VerifiedRanges=len(ranges), IoOperations=sum(len(c['Trace']) for c in cases), FixtureSha256=report['FixtureSha256'])))


if __name__ == '__main__':
    main()
