"""Execute the original party clan-mismatch counter against fixed S201 rows."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]
BASE, DATA, STACK, STOP = 0x400000, 0x20000000, 0x200e0000, 0x200ff000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    source = ROOT / 'docs/evidence/party_clan_identity_ida.json'
    evidence = json.loads(source.read_text('utf-8'))
    blocks = list(evidence['resources']) + list(evidence['ranges'])
    for function in evidence['functions']:
        blocks.extend(function['chunks'])
    for block in blocks:
        content = bytes.fromhex(block['bytes'])
        assert hashlib.sha256(content).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'], 16), len(content)) == content
    mapped = pe.get_memory_mapped_image()
    fixture = ROOT / 'tests/party_clan_identity_packets.txt'
    runs = []
    for line in fixture.read_text().splitlines():
        label, expected, wire = line.split()
        payload = bytes.fromhex(wire)
        state, size = struct.unpack_from('<II', payload)
        assert state == 2
        offset, clans = 8, []
        for i in range(size):
            identity, length = struct.unpack_from('<II', payload, offset)
            assert identity == i+1
            offset += 8 + length*2
            a, b, c, clan = struct.unpack_from('<IIII', payload, offset)
            assert (a, b, c) == (0xffffffff, 0xffffffff, 0)
            clans.append(clan)
            offset += 16
        assert payload[offset:] == bytes(4)
        m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped)+4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x100000)
        rows = DATA + 0x1000
        m.mem_write(DATA+8, struct.pack('<II', rows, rows + size*48))
        # The native empty-vector branch still reads first+44. Keep padding
        # mapped; this does not establish that the real empty UI is safe.
        for i, clan in enumerate(clans):
            m.mem_write(rows+i*48+44, struct.pack('<I', clan))
        m.mem_write(STACK, struct.pack('<I', STOP))
        m.reg_write(UC_X86_REG_ESP, STACK)
        m.reg_write(UC_X86_REG_ECX, DATA)
        count = 0

        def hook(uc, address, instruction_size, _):
            nonlocal count
            assert 0x90ab30 <= address <= 0x90ab58, hex(address)
            assert bytes(uc.mem_read(address, instruction_size)) == mapped[address-BASE:address-BASE+instruction_size]
            count += 1

        m.hook_add(UC_HOOK_CODE, hook)
        m.emu_start(0x90ab30, STOP, count=1000)
        assert m.reg_read(UC_X86_REG_EIP) == STOP
        assert m.reg_read(UC_X86_REG_EAX) == int(expected)
        runs.append(dict(label=label, clans=clans, outsiders=int(expected), original_instructions=count, adapter_calls=[]))
    report = dict(passed=True, sample_sha256=digest, evidence_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest(), functions=len(evidence['functions']),
                  resources=len(evidence['resources']), verified_blocks=len(blocks), runs=runs, original_ui=False,
                  limits='Counter90AB30 only. S201 decoding is independently constructed memory; native reader, UI, sockets, channel admission and clan plaza not executed. Empty vector uses mapped padding.')
    (ROOT/'docs/evidence/party_clan_identity_probe.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(dict(passed=True, runs=len(runs), instructions=sum(r['original_instructions'] for r in runs), adapters=0, verified_blocks=len(blocks))))


if __name__ == '__main__':
    main()
