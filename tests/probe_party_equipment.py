"""PE verification and bounded native 817500 equipment-consumer execution."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP


BASE, DATA = 0x400000, 0x20000000
SELF, STATE, RECORD, STACK, STOP = [DATA + n for n in (0x1000, 0x2000, 0x4000, 0xf000, 0xffe0)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.OPTIONAL_HEADER.ImageBase == BASE and pe.FILE_HEADER.Machine == 0x14c
    evidence_path = root / 'docs/evidence/party_equipment_ida.json'
    evidence = json.loads(evidence_path.read_text(encoding='utf-8'))
    verified = []
    for f in evidence['functions']:
        b = bytes.fromhex(f['bytes'])
        assert pe.get_data(int(f['rva'], 16), len(b)) == b, f['va']
        verified.append(dict(va=f['va'], rva=f['rva'], size=len(b), sha256=hashlib.sha256(b).hexdigest()))
    assert len(verified) == 10
    mapped = pe.get_memory_mapped_image()
    runs = []
    cases = [(300027, [[], [], []], 0),
             (300027, [[201005, 202006, 205033, 207001, 300027], [], []], 1),
             (300010, [[202006], [111, 222], [333]], 0),
             (300010, [[201005, 207001], [444], [555, 666]], 1)]
    for model, groups, refresh in cases:
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x10000)

        def put(address, *values):
            m.mem_write(address, struct.pack('<' + 'I' * len(values), *values))

        def get(address):
            return struct.unpack('<I', m.mem_read(address, 4))[0]

        put(SELF + 8, STATE)
        for offset, node in [(44, DATA + 0x3000), (56, DATA + 0x3020)]:
            put(STATE + offset, node, 99)
            put(node, node, node, node)
        put(RECORD, model)
        for i, group in enumerate(groups):
            ptr = DATA + 0x5000 + i * 0x100
            put(RECORD + 4 + i * 12, ptr, ptr + len(group) * 4, ptr + len(group) * 4)
            if group:
                put(ptr, *group)
        put(STACK, STOP, RECORD, refresh)
        m.reg_write(UC_X86_REG_ESP, STACK)
        m.reg_write(UC_X86_REG_ECX, SELF)
        calls, applied = [], []
        instruction_count = 0

        def hook(uc, address, size, _):
            nonlocal instruction_count
            if 0x817500 <= address < 0x8175dc:
                instruction_count += 1
                return
            sp = uc.reg_read(UC_X86_REG_ESP)
            target = get(sp)
            args = [get(sp + 4 * i) for i in range(1, 4)]
            if address == 0x40e370:
                pop = 8  # Tree destruction adapter; nodes are synthetic sentinels.
            elif address == 0x817680:
                assert uc.reg_read(UC_X86_REG_ECX) == SELF
                applied.append(args[0])
                pop = 8
            elif address == 0x817df0:
                assert uc.reg_read(UC_X86_REG_ECX) == SELF
                assert args == [model, RECORD + 4, refresh]
                pop = 12  # Model/first-group application is not executed here.
            elif address == 0x8175e0:
                assert args[0] == RECORD and uc.reg_read(UC_X86_REG_ECX) == STATE + 76
                pop = 4
            elif address == 0x818aa0:
                assert refresh == 1 and args[0] == STATE + 128
                pop = 4
            else:
                raise AssertionError(f'Unexpected native target {address:x}')
            calls.append(hex(address))
            uc.reg_write(UC_X86_REG_EAX, 0)
            uc.reg_write(UC_X86_REG_ESP, sp + 4 + pop)
            uc.reg_write(UC_X86_REG_EIP, target)

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(0x817500, STOP, count=3000)
        assert m.reg_read(UC_X86_REG_EIP) == STOP and m.reg_read(UC_X86_REG_ESP) == STACK + 12
        assert get(STATE + 48) == get(STATE + 60) == 0
        assert applied == groups[1] + groups[2]
        assert calls.count('0x817df0') == 1 and calls.count('0x818aa0') == refresh
        runs.append(dict(model=model, groups=groups, refresh=refresh,
                         native_instructions=instruction_count, adapted_calls=calls,
                         first_group_delegation_verified=True,
                         model_item_application_executed=False, original_ui=False))
    report = dict(utc=datetime.now(timezone.utc).isoformat(), passed=True,
                  sample_sha256=digest, image_base=hex(BASE), functions=verified,
                  evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
                  runs=runs, native_instructions=sum(x['native_instructions'] for x in runs),
                  adapted_call_count=sum(len(x['adapted_calls']) for x in runs),
                  new_wire_codecs=0, original_ui=False,
                  limits='Only original 817500 executes. Tree destruction, per-item rendering, model/group application, details and final refresh use explicit adapters; no UI or full S201 handler execution.')
    (root / 'docs/evidence/party_equipment_probe.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"PASS: 10 IDA/PE functions; 4 native consumer cases, {report['native_instructions']} instructions, {report['adapted_call_count']} adapted calls.")


if __name__ == '__main__':
    main()
