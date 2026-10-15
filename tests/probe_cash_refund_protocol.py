"""Bounded BFCLIENT02 refund writers/readers; no UI, network or billing."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x30000000
BP, STACK, HEADER, VTABLE, TEXT, WRITE, READ, FREE, STREAM, OBJECT, FRAME, MANAGER = [DATA+n for n in (0x10000, 0x8000, 0x12000, 0x14000, 0x16000, 0x18000, 0x18100, 0x18200, 0x20000, 0x22000, 0x24000, 0x26000)]


def put(m, a, v): m.mem_write(a, struct.pack('<I', v & 0xffffffff))
def get(m, a): return struct.unpack('<I', m.mem_read(a, 4))[0]


def string(m, a, raw, heap=TEXT):
    count = len(raw)//2
    m.mem_write(a if count <= 7 else heap, raw+b'\0\0')
    if count > 7: put(m, a, heap)
    put(m, a+16, count)
    put(m, a+20, max(count, 7))


def value(m, a):
    return bytes(m.mem_read(get(m, a) if get(m, a+20)>7 else a, get(m, a+16)*2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    evidence_path = ROOT/'docs/evidence/cash_refund_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    blocks = evidence['functions']+evidence['ranges']+evidence['resources']
    for b in blocks:
        blob = bytes.fromhex(b['bytes'])
        assert len(blob) == b['size'] and hashlib.sha256(blob).hexdigest() == b['sha256']
        assert pe.get_data(int(b['rva'], 16), len(blob)) == blob
    mapped = pe.get_memory_mapped_image()
    fixture_path = ROOT/'tests/cash_refund_protocol_packets.txt'
    fixed = {label:(int(mid), bytes.fromhex(body)) for label,mid,body in (l.split() for l in fixture_path.read_text('ascii').splitlines())}
    cases_path = ROOT/'tests/cash_refund_protocol_cases.json'
    cases = json.loads(cases_path.read_text('utf-8'))
    runs = []

    def machine():
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped)+4095)&~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x80000)
        m.mem_map(0, 0x1000)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_ESP, STACK)
        m.reg_write(UC_X86_REG_EBX, FRAME)
        put(m, FRAME+12, STREAM)
        put(m, STREAM, VTABLE)
        put(m, VTABLE+44, READ)
        put(m, VTABLE+48, WRITE)
        put(m, 0x163b680+48, WRITE)
        put(m, 0x18e6d44, MANAGER)
        put(m, MANAGER, MANAGER+16)
        put(m, MANAGER+16, VTABLE)
        put(m, VTABLE+8, FREE)
        return m

    def execute(m, label, start, ends, payload=b''):
        if isinstance(ends, int): ends = {ends}
        cursor = 0
        next_heap = DATA+0x30000
        instructions, calls, output = [], [], bytearray()

        def hook(uc, a, n, _):
            nonlocal cursor, next_heap
            if a in ends:
                uc.emu_stop()
                return
            if a in (WRITE, READ, FREE, 0xfa2d60, 0x10bb360, 0x40daa0, 0x40cb40, 0x40dbf0, 0x108ceb0):
                sp = uc.reg_read(UC_X86_REG_ESP)
                ret, pop, result = get(uc, sp), 4, 0
                target = uc.reg_read(UC_X86_REG_ECX)
                kind = ''
                if a in (WRITE, READ):
                    pointer, count = get(uc, sp+4), get(uc, sp+8)
                    assert count <= 32768
                    if a == WRITE:
                        output.extend(uc.mem_read(pointer, count))
                    else:
                        assert cursor+count <= len(payload), (label,cursor,count)
                        if count: uc.mem_write(pointer, payload[cursor:cursor+count])
                        cursor += count
                    kind, pop, result = 'virtual write' if a == WRITE else 'virtual read', 12, count
                elif a in (0xfa2d60, 0x10bb360):
                    put(uc, target, VTABLE)
                    put(uc, target+24, HEADER)
                    kind = 'packet construction'
                elif a == 0x108ceb0:
                    count = get(uc, sp+8)
                    assert count <= 16385
                    put(uc, target+4, 0)
                    for offset in (16,20,24): put(uc, target+offset, next_heap if offset==16 else next_heap+count*2)
                    next_heap += count*2+32
                    kind, pop = 'temporary buffer allocation', 12
                elif a in (0x40dbf0, FREE):
                    kind, pop = 'temporary destruction', 12 if a == FREE else 4
                else:
                    source = get(uc, sp+4)
                    if a == 0x40daa0:
                        count = 0
                        while uc.mem_read(source+2*count,2) != b'\0\0':
                            count += 1
                            assert count <= 16384
                        blob = bytes(uc.mem_read(source, count*2))
                    else:
                        blob = value(uc, source)
                    string(uc, target, blob, next_heap)
                    next_heap += len(blob)+32
                    kind, pop, result = 'wstring construction' if a==0x40daa0 else 'wstring assignment', 8, target
                calls.append(dict(va=hex(a), return_va=hex(ret), kind=kind))
                uc.reg_write(UC_X86_REG_EAX, result)
                uc.reg_write(UC_X86_REG_ESP, sp+pop)
                uc.reg_write(UC_X86_REG_EIP, ret)
                return
            assert BASE <= a < BASE+len(mapped), (label,hex(a))
            assert bytes(uc.mem_read(a,n)) == mapped[a-BASE:a-BASE+n]
            instructions.append(hex(a))

        handle = m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, 0, count=10000)
        m.hook_del(handle)
        end = m.reg_read(UC_X86_REG_EIP)
        assert end in ends, (label,hex(end))
        runs.append(dict(label=label, start_va=hex(start), end_va=hex(end), original_instructions=len(instructions), trace=instructions, adapter_calls=calls))
        return bytes(output), cursor, end

    # The complete observed UI collection always constructs an empty string.
    for slot in (0, 1, 0x10203040, 0x80000000, 0xffffffff):
        m = machine()
        m.reg_write(UC_X86_REG_ECX, OBJECT)
        put(m, OBJECT+4, slot)
        out, _, _ = execute(m, f'ui_request_{slot}', 0xd3e0cb, 0xd3e18f)
        assert out == struct.pack('<II',0,slot)
        assert bytes(m.mem_read(HEADER+4,2)) == struct.pack('<H',817)
    for row in cases['requests']:
        label = row['label']
        mid, body = fixed[label]
        m = machine()
        put(m, BP-0x5c, VTABLE)
        put(m, BP-0x44, HEADER)
        put(m, BP-0x18, row['slot_id'])
        string(m, BP-0x30, row['text'].encode('utf-16le','surrogatepass'))
        out, _, _ = execute(m, label, 0xd3e139, 0xd3e18f)
        assert out == body and bytes(m.mem_read(HEADER+4,2)) == struct.pack('<H',mid)
    for row in cases['responses']:
        m = machine()
        mid, body = fixed[row['label']]
        assert mid == 824
        _, cursor, _ = execute(m, row['label'], 0x9b8732, 0x9b87b8, body)
        assert cursor == len(body)
        assert get(m,BP-0xe4) == row['slot_id'] and get(m,BP-0xe0) == row['result']
        expected = row['text'].split('\0')[0].encode('utf-16le','surrogatepass')
        assert value(m,m.reg_read(UC_X86_REG_EAX)) == expected
    # Original string length gates: no allocation or text read for these runs.
    for length, end in [(1023,0x112647f),(1024,0x11264d9),(16384,0x11264d9),(16385,0x11266af),(0xffffffff,0x11266af)]:
        m = machine()
        put(m,BP-0x14,length)
        _,_,found = execute(m,f'length_{length}',0x1126467,{0x112647f,0x11264d9,0x11266af})
        assert found == end
    for flag in range(256):
        m = machine()
        m.reg_write(UC_X86_REG_ESI,OBJECT)
        m.mem_write(OBJECT+144,bytes([flag]))
        execute(m,f'button_{flag}',0xd3cd1b,0xd3cd30)
        assert bytes(m.mem_read(OBJECT+176,1)) == bytes([int(flag==1)])
    targets = {1:0x9b88a7,2:0x9b8a92,3:0x9b8c6e,4:0x9b8e78,5:0x9b905c,6:0x9b9297,7:0x9b9488}
    for result in (0,1,2,3,4,5,6,7,0xffffffff):
        m = machine()
        put(m,BP-0xe0,result)
        _,_,end = execute(m,f'result_{result}',0x9b8890,set(targets.values())|{0x9b9680})
        assert end == targets.get(result,0x9b9680)
    m = machine()
    put(m,BP-0xe4,0xfedcba98)
    execute(m,'success_delete_argument',0x9b8a82,0x8055f0)
    assert get(m,m.reg_read(UC_X86_REG_ESP)+4) == 0xfedcba98
    report = dict(passed=True,sample_sha256=digest,evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),fixture_sha256=hashlib.sha256(fixture_path.read_bytes()).hexdigest(),cases_sha256=hashlib.sha256(cases_path.read_bytes()).hexdigest(),verified_blocks=len(blocks),fixtures=len(fixed),runs=runs,original_ui=False,
        limits='Bounded original writer/reader, exact-byte refundable flag, length gates, result branch selection and success deletion argument only. Packet construction, virtual I/O, temporary allocation/destruction and wstring construction/assignment are adapted. Native short/long UTF16 length and byte-read instructions execute; wstring construction models the separately inspected wcslen helper, which truncates at embedded NUL. Injected nonempty request fields are synthetic; the observed UI collection writes empty text. No complete dispatcher, dialogs, cash-slot deletion, server transactions, payment API or network execution.')
    (ROOT/'docs/evidence/cash_refund_protocol_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8', newline='\n')
    print(json.dumps(dict(passed=True,blocks=len(blocks),fixtures=len(fixed),runs=len(runs),instructions=sum(x['original_instructions'] for x in runs),adapters=sum(len(x['adapter_calls']) for x in runs))))


if __name__ == '__main__': main()
