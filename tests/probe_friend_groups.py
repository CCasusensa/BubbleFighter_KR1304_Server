"""Bounded original Messenger SDK construction/serialization; no UI or sockets."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX,
                              UC_X86_REG_ECX, UC_X86_REG_EDI, UC_X86_REG_EDX,
                              UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP)

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x30000000
BP, STACK, OBJECT, TEXT, ROW, FRIEND, STREAM, BUFFER, OUTPUT, STOP = [
    DATA+n for n in (0x10000, 0x8000, 0x20000, 0x22000, 0x24000, 0x26000,
                     0x28000, 0x29000, 0x2a000, 0x3f000)]


def put(m, a, *values):
    m.mem_write(a, struct.pack('<'+'I'*len(values), *values))


def get(m, a):
    return struct.unpack('<I', m.mem_read(a, 4))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    evidence_path = ROOT/'docs/evidence/friend_groups_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    chunks = [c for f in evidence['functions'] for c in f['chunks']]
    blocks = chunks + evidence['resources'] + evidence['ranges']
    for row in blocks:
        b = bytes.fromhex(row['bytes'])
        assert hashlib.sha256(b).hexdigest() == row['sha256']
        assert pe.get_data(int(row['rva'], 16), len(b)) == b
    mapped = pe.get_memory_mapped_image()
    intervals = [(int(c['va'], 16), int(c['end_va'], 16)) for c in chunks]
    cases_path = ROOT/'tests/friend_group_native_cases.json'
    cases = json.loads(cases_path.read_text('utf-8'))['requests']
    runs = []

    def machine():
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped)+4095)&~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x40000)
        m.mem_map(0, 4096)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_ESP, STACK)
        put(m, STACK, STOP)
        put(m, STREAM, BUFFER)
        put(m, BUFFER, OUTPUT, 0x1000, 0, 0, 0, 1)
        return m

    def execute(m, label, start, end):
        trace, adapters = [], []

        endings = {end} if isinstance(end, int) else end

        def hook(uc, a, n, _):
            if a in endings:
                uc.emu_stop()
                return
            if a in (0xf692f0, 0x1296140):
                sp = uc.reg_read(UC_X86_REG_ESP)
                ret = get(uc, sp)
                if a == 0xf692f0:
                    length = get(uc, sp+4)
                    assert length <= 4096 and get(uc, BUFFER+16)+length <= 4096
                    value, pop = 1, 8
                    kind = 'preallocated SDK buffer reserve'
                else:
                    dest, source, length = [get(uc, sp+i) for i in (4, 8, 12)]
                    assert length <= 4096
                    uc.mem_write(dest, bytes(uc.mem_read(source, length)))
                    value, pop = dest, 4
                    kind = 'memcpy'
                adapters.append(dict(va=hex(a), return_va=hex(ret), kind=kind))
                uc.reg_write(UC_X86_REG_EAX, value)
                uc.reg_write(UC_X86_REG_ESP, sp+pop)
                uc.reg_write(UC_X86_REG_EIP, ret)
                return
            assert any(lo <= a and a+n <= hi for lo, hi in intervals), (label, hex(a))
            assert bytes(uc.mem_read(a, n)) == mapped[a-BASE:a-BASE+n]
            trace.append(hex(a))

        h = m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, 0, count=10000)
        m.hook_del(h)
        reached = m.reg_read(UC_X86_REG_EIP)
        assert reached in endings, label
        runs.append(dict(label=label, start_va=hex(start), end_va=hex(reached),
                         original_instructions=len(trace), trace=trace, adapter_calls=adapters))
        return reached

    for case in cases:
        m = machine()
        action, label = case['action'], case['label']
        obj = OBJECT
        if action in ('create', 'rename'):
            m.mem_write(TEXT, case['input_name'].encode('utf-16-le')+b'\0\0')
            if action == 'create':
                m.reg_write(UC_X86_REG_ESI, 74255)
                m.reg_write(UC_X86_REG_EBX, TEXT)
                put(m, STACK, 0, 0, 0)
                execute(m, label+'_construct', 0xd748cd, 0xd74955)
                obj, offset = BP-0x88, 52
            else:
                m.reg_write(UC_X86_REG_EDI, ROW)
                put(m, ROW+12, 7)
                m.reg_write(UC_X86_REG_ESI, TEXT)
                execute(m, label+'_construct', 0xd74538, 0xd745b7)
                obj, offset = BP-0xb4, 48
            expected = case['stored_name'].encode('utf-16-le')
            assert bytes(m.mem_read(obj+offset, len(expected)+2)) == expected+b'\0\0', label
        elif action in ('assign', 'move'):
            put(m, ROW+36, FRIEND)
            put(m, FRIEND+336, *case['words'][:6])
            m.reg_write(UC_X86_REG_EDI, ROW)
            m.reg_write(UC_X86_REG_ESI, 7 if action == 'assign' else 9)
            put(m, BP-0x28, 7)
            if action == 'assign':
                execute(m, label+'_construct', 0xd75447, 0xd75492)
                obj = BP-0x8c
            else:
                execute(m, label+'_construct', 0xd7557e, 0xd755cf)
                obj = BP-0x90
            assert bytes(m.mem_read(obj+44, 4*len(case['words']))) == struct.pack('<'+'I'*len(case['words']), *case['words'])
        elif action == 'delete_group':
            put(m, ROW+12, 7)
            m.reg_write(UC_X86_REG_EDI, ROW)
            execute(m, label+'_construct', 0xd74721, 0xd7474e)
            obj = BP-0x40
            assert get(m, obj+44) == 7 and get(m, obj+48) == 1
        else:
            put(m, STACK+4, *case['words'], 0)
            execute(m, label+'_construct', 0xf90650, 0xf906b0)
            obj = m.reg_read(UC_X86_REG_EBP)-0x58
            assert bytes(m.mem_read(obj+44, 28)) == struct.pack('<7I', *case['words'])
        assert get(m, obj+12) == case['sdk_id']
        assert get(m, obj+36) == 0
        # The wrapper's stack object must outlive its separate writer stack.
        m.mem_write(OBJECT, bytes(m.mem_read(obj, 124)))
        obj = OBJECT
        # Explicitly select the SDK request-body branch. Outer SDK envelope and
        # dispatch state changes are not part of these isolated writer runs.
        put(m, obj+16, 1)
        m.reg_write(UC_X86_REG_ECX, obj)
        m.reg_write(UC_X86_REG_ESP, STACK)
        put(m, STACK, STOP, STREAM, 0)
        execute(m, label+'_write', int(case['writer_va'], 16), STOP)
        count = get(m, BUFFER+16)
        assert count == get(m, BUFFER+8)
        actual = bytes(m.mem_read(OUTPUT, count)).hex()
        assert actual == case['expected_body'], (label, actual, case['expected_body'])

    for count, allowed in ((0, True), (1, False), (200, False), (0xFFFFFFFF, True)):
        m = machine()
        put(m, BP+0x5c, ROW)
        put(m, ROW+40, count)
        reached = execute(m, 'delete_gate_'+str(count), 0xd74689, {0xd7471c, 0xd74696})
        assert (reached == 0xd7471c) == allowed
    for source, destination, expected in ((0, 0, 'assign'), (0, 7, 'assign'),
                                          (7, 0, 'skip'), (7, 9, 'move'), (7, 7, 'move')):
        m = machine()
        m.reg_write(UC_X86_REG_ESI, OBJECT)
        m.reg_write(UC_X86_REG_EDI, ROW)
        put(m, OBJECT+0x698, TEXT)
        put(m, TEXT, destination)
        put(m, ROW+48, source)
        put(m, BP+12, 0)
        ends = {'assign': 0xd75442, 'move': 0xd75579, 'skip': 0xd75638}
        reached = execute(m, 'move_gate_'+str(source)+'_'+str(destination), 0xd75428, set(ends.values()))
        assert reached == ends[expected]

    # Copy an empty CNMCateFriendInfo without adapting its constructor.
    # The base fields alone use 92 bytes; the nested friend vector makes 104.
    m = machine()
    m.mem_write(ROW, b'\0'*104)
    m.mem_write(ROW+4, bytes.fromhex('6963020100'))
    put(m, ROW+12, 7, 74255, 1)
    m.mem_write(ROW+24, '친구'.encode('utf-16-le')+b'\0\0')
    m.mem_write(ROW+88, b'\1\0')
    m.mem_write(OBJECT, b'\xa5'*112)
    m.reg_write(UC_X86_REG_ECX, OBJECT)
    put(m, STACK, STOP, ROW)
    execute(m, 'empty_group_copy', 0xf96740, STOP)
    assert get(m, OBJECT) == 0x1631944
    assert bytes(m.mem_read(OBJECT+4, 5)) == bytes(m.mem_read(ROW+4, 5))
    assert bytes(m.mem_read(OBJECT+12, 78)) == bytes(m.mem_read(ROW+12, 78))
    assert bytes(m.mem_read(OBJECT+92, 12)) == b'\0'*12
    assert bytes(m.mem_read(OBJECT+104, 8)) == b'\xa5'*8

    report = dict(passed=True, sample_sha256=digest,
                  evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
                  cases_sha256=hashlib.sha256(cases_path.read_bytes()).hexdigest(),
                  verified_blocks=len(blocks), functions=len(evidence['functions']),
                  cases=len(cases), cached_delete_gates=4, cached_move_gates=5,
                  empty_group_copy=1, runs=runs, original_ui=False,
                  limits='UI request construction bodies and SDK request-body writers execute separately. Constructors execute original instructions. The writer request branch is selected explicitly; original SDK dispatcher, outer envelope, platform transport, UI dialogs/rendering and persistent server business do not execute. Buffer reserve and memcpy are disclosed adapters. SDK IDs are not main TCP packet IDs. Empty/embedded-zero/overlong names are counterexamples, not accepted companion names.')
    (ROOT/'docs/evidence/friend_groups_probe.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(passed=True, blocks=len(blocks), cases=len(cases),
                         runs=len(runs), instructions=sum(x['original_instructions'] for x in runs),
                         adapters=sum(len(x['adapter_calls']) for x in runs))))


if __name__ == '__main__':
    main()
