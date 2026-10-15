"""Bounded native refund feature selection; no game process or UI execution."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA = 0x400000, 0x30000000
STACK, BP, MANAGER, SENTINEL, NODE, STOP = [DATA + n for n in (0x8000, 0x10000, 0x12000, 0x14000, 0x16000, 0x18000)]
EXPECTED_KEYS = [1, 41, 42, 43, 46, 48, 49, 52, 53, 54, 101, 102, 201, 202,
                 301, 302, 304, 305, 306, 307, 501, 502, 503, 702, 705, 706,
                 707, 780, 1001, 1002, 1011, 1012, 1014, 1017, 1018, 1102,
                 509, 2001, 9999]


def put(m, address, value):
    m.mem_write(address, struct.pack('<I', value))


def get(m, address):
    return struct.unpack('<I', m.mem_read(address, 4))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    evidence_path = ROOT / 'docs/evidence/cash_refund_features_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    for block in evidence['functions']:
        blob = bytes.fromhex(block['bytes'])
        assert hashlib.sha256(blob).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'], 16), len(blob)) == blob
    mapped = pe.get_memory_mapped_image()
    profile = json.loads((ROOT / 'tools/client_runtime/profiles.json').read_text('utf-8'))
    patch = profile['ShopFeatures']['Shop']
    assert patch['Rva'] == '0x00cf2fcc'
    assert patch['ExpectedBytes'] == '680f2700008bcee81898ffff'
    assert patch['ReplacementBytes'] == '909090909090909090909090'
    runs = []

    def machine():
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x20000)
        m.reg_write(UC_X86_REG_EBP, BP)
        m.reg_write(UC_X86_REG_ESP, STACK)
        m.reg_write(UC_X86_REG_ECX, MANAGER)
        put(m, STACK, STOP)
        return m

    def execute(m, label, start, ends, adapter, patched=False):
        original, modified, calls = [], [], []

        def hook(uc, address, size, _):
            if address in ends:
                uc.emu_stop()
                return
            if adapter(uc, address, calls):
                return
            target = modified if patched and 0x10f2fcc <= address < 0x10f2fd8 else original
            target.append(address)
            assert len(original) + len(modified) < 500

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(start, STOP, count=600)
        end = m.reg_read(UC_X86_REG_EIP)
        assert end in ends, (label, hex(end))
        result = {'case': label, 'original_instructions': len(original),
                  'patched_instructions': len(modified), 'adapter_calls': calls,
                  'stop_va': hex(end)}
        runs.append(result)
        return result

    for patched in (False, True):
        m = machine()
        if patched:
            assert bytes(m.mem_read(0x10f2fcc, 12)).hex() == patch['ExpectedBytes']
            m.mem_write(0x10f2fcc, bytes.fromhex(patch['ReplacementBytes']))

        def insertion(uc, address, calls):
            if address != 0x10ec7f0:
                return False
            sp = uc.reg_read(UC_X86_REG_ESP)
            assert uc.reg_read(UC_X86_REG_ECX) == MANAGER
            calls.append({'adapter': 'feature insertion boundary', 'key': get(uc, sp + 4)})
            uc.reg_write(UC_X86_REG_EAX, NODE)
            uc.reg_write(UC_X86_REG_EIP, get(uc, sp))
            uc.reg_write(UC_X86_REG_ESP, sp + 8)
            return True

        result = execute(m, 'existing shop patch' if patched else 'native startup',
                         0x10f2e20, {STOP}, insertion, patched)
        keys = [call['key'] for call in result['adapter_calls']]
        assert keys == (EXPECTED_KEYS[:-1] if patched else EXPECTED_KEYS)
        assert keys.count(1002) == 1

    for label, manager, node, nil, key, expected in (
            ('manager absent', 0, NODE, 0, 1002, 0xd3d905),
            ('key absent', MANAGER, SENTINEL, 1, 0, 0xd3d905),
            ('lower bound is later key', MANAGER, NODE, 0, 1003, 0xd3d905),
            ('refund key present', MANAGER, NODE, 0, 1002, 0xd3d415)):
        m = machine()
        put(m, 0x18e6d38, manager)
        put(m, MANAGER + 4, SENTINEL)
        m.mem_write(node + 13, bytes([nil]))
        put(m, node + 16, key)

        def lookup(uc, address, calls):
            if address != 0x4154e0:
                return False
            sp = uc.reg_read(UC_X86_REG_ESP)
            assert uc.reg_read(UC_X86_REG_ECX) == MANAGER + 4
            output, key_pointer = get(uc, sp + 4), get(uc, sp + 8)
            assert get(uc, key_pointer) == 1002
            put(uc, output + 8, node)
            calls.append({'adapter': 'set lower-bound lookup', 'key': 1002})
            uc.reg_write(UC_X86_REG_EAX, output)
            uc.reg_write(UC_X86_REG_EIP, get(uc, sp))
            uc.reg_write(UC_X86_REG_ESP, sp + 12)
            return True

        result = execute(m, label, 0xd3d3cc, {0xd3d415, 0xd3d905}, lookup)
        assert result['stop_va'] == hex(expected)
    report = {'passed': True, 'sample_sha256': digest, 'verified_blocks': 3,
              'evidence_sha256': hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
              'original_ui': False, 'runs': runs,
              'limits': 'Insertion and lookup containers are adapted; original startup calls and branch instructions execute. Stops precede dialog creation. The existing shop patch is applied only to emulator memory; original files and runtime adapters are unchanged.'}
    (ROOT / 'docs/evidence/cash_refund_features_probe.json').write_text(
        json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(f"PASS: refund feature gate, {len(runs)} bounded runs, "
          f"{sum(x['original_instructions'] for x in runs)} original instructions; "
          "feature 1002 retained by existing shop patch; no UI.")


if __name__ == '__main__':
    main()
