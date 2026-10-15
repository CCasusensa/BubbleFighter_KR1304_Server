"""Bounded BFCLIENT02 gift serializers/readers, without a game process or UI."""
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
STACK, BP, OBJECT, INPUT, STREAM, PTR, VTABLE, HEADER, READ, WRITE, END, STOP = [DATA + n for n in
    (0x8000, 0x10000, 0x12000, 0x14000, 0x16000, 0x18000, 0x20000, 0x22000, 0x24000, 0x24100, 0x24200, 0x7f000)]


def put(m, address, value):
    m.mem_write(address, struct.pack('<I', value & 0xffffffff))


def get(m, address):
    return struct.unpack('<I', m.mem_read(address, 4))[0]


def raw_text(value):
    return value.encode('utf-16le', errors='surrogatepass')


def native_text(value):
    # The shared native reader constructs with wcslen after consuming every unit.
    return value.split('\0', 1)[0]


class Machine:
    def __init__(self, mapped, payload=b''):
        self.m = m = Uc(UC_ARCH_X86, UC_MODE_32)
        m.mem_map(BASE, (len(mapped) + 4095) & ~4095)
        m.mem_write(BASE, mapped)
        m.mem_map(DATA, 0x800000)
        m.mem_map(0, 0x1000)
        m.reg_write(UC_X86_REG_ESP, STACK)
        m.reg_write(UC_X86_REG_EBP, BP)
        put(m, STACK, STOP)
        put(m, STREAM, VTABLE)
        put(m, PTR, STREAM)
        put(m, VTABLE + 24, END)
        put(m, VTABLE + 44, READ)
        put(m, VTABLE + 48, WRITE)
        put(m, 0x163b680 + 48, WRITE)
        self.payload, self.cursor, self.output = payload, 0, bytearray()
        self.heap = DATA + 0x100000
        self.instructions, self.calls = [], []

    def allocate(self, size):
        value = self.heap
        self.heap += (size + 31) & ~15
        assert self.heap < DATA + 0x700000
        return value

    def take(self, size):
        assert self.cursor + size <= len(self.payload), (self.cursor, size, len(self.payload))
        value = self.payload[self.cursor:self.cursor + size]
        self.cursor += size
        return value

    def number(self):
        return struct.unpack('<I', self.take(4))[0]

    def take_text(self):
        count = self.number()
        assert count <= 16384
        return native_text(self.take(count * 2).decode('utf-16le', errors='surrogatepass'))

    def string(self, address, value):
        raw = raw_text(value)
        count = len(raw) // 2
        target = address if count <= 7 else self.allocate(len(raw) + 2)
        self.m.mem_write(target, raw + b'\0\0')
        if count > 7:
            put(self.m, address, target)
        put(self.m, address + 16, count)
        put(self.m, address + 20, max(7, count))

    def string_value(self, address):
        m = self.m
        count = get(m, address + 16)
        target = address if get(m, address + 20) <= 7 else get(m, address)
        return bytes(m.mem_read(target, count * 2)).decode('utf-16le', errors='surrogatepass')

    def vector(self, address, values, width=4):
        target = self.allocate(max(1, len(values) * width))
        for index, value in enumerate(values):
            self.m.mem_write(target + index * width, value.to_bytes(width, 'little'))
        for offset, value in [(0, target), (4, target + len(values) * width), (8, target + len(values) * width)]:
            put(self.m, address + offset, value)

    def vector_value(self, address, width=4):
        m = self.m
        begin, end = get(m, address), get(m, address + 4)
        assert (end - begin) % width == 0 and 0 <= end - begin <= 4096
        return [int.from_bytes(m.mem_read(pos, width), 'little') for pos in range(begin, end, width)]

    def execute(self, label, start, end=STOP):
        def hook(m, address, size, _):
            if address == end:
                m.emu_stop()
                return
            sp, target = m.reg_read(UC_X86_REG_ESP), m.reg_read(UC_X86_REG_ECX)
            pop, result, kind = 4, 0, None
            if address in (READ, WRITE):
                pointer, count = get(m, sp + 4), get(m, sp + 8)
                assert count <= 32768
                if address == READ:
                    value = self.take(count)
                    if value:
                        m.mem_write(pointer, value)
                else:
                    self.output.extend(m.mem_read(pointer, count))
                pop, result, kind = 12, count, 'virtual byte read' if address == READ else 'virtual byte write'
            elif address == END:
                result, kind = int(self.cursor == len(self.payload)), 'virtual stream end'
            elif address in (0xfa2d60, 0x10bb360):
                put(m, target, VTABLE)
                put(m, target + 24, HEADER)
                result, kind = target, 'packet construction'
            elif address == 0x1119c10:
                assert get(m, m.reg_read(UC_X86_REG_EDX)) == STREAM
                self.string(target, self.take_text())
                result, kind = target, 'shared native string reader boundary (wcslen behavior)'
            elif address == 0x40cb40:
                self.string(target, self.string_value(get(m, sp + 4)))
                pop, result, kind = 8, target, 'wstring assignment'
            elif address in (0x40db80, 0x40e140, 0x459a60, 0x4b24d0):
                kind = 'temporary string/vector destruction'
            elif address in (0x4ca9b0, 0x624460):
                count = get(m, get(m, sp + 4))
                assert count <= 1024
                width = 4 if address == 0x4ca9b0 else 1
                storage = self.allocate(max(1, count * width))
                for offset, value in [(0, storage), (4, storage), (8, storage + count * width)]:
                    put(m, target + offset, value)
                pop, result, kind = 8, storage, 'vector reserve'
            elif address == 0x10d9050:
                # The compound-vector container is adapted here. Each record reader
                # is separately executed below, including its nested native u32 vector.
                assert get(m, target) == STREAM
                destination, count = get(m, sp + 4), self.number()
                assert count <= 1024
                storage = self.allocate(max(1, count * 48))
                for index in range(count):
                    row = storage + index * 48
                    put(m, row, self.number()); put(m, row + 4, self.number())
                    length = self.number()
                    assert length <= 1024
                    self.vector(row + 8, [self.number() for _ in range(length)])
                    put(m, row + 20, self.number())
                    self.string(row + 24, self.take_text())
                for offset, value in [(0, storage), (4, storage + count * 48), (8, storage + count * 48)]:
                    put(m, destination + offset, value)
                pop, result, kind = 8, target, 'compound-vector container boundary'
            if kind is not None:
                self.calls.append({'va': hex(address), 'kind': kind})
                m.reg_write(UC_X86_REG_EAX, result)
                m.reg_write(UC_X86_REG_EIP, get(m, sp))
                m.reg_write(UC_X86_REG_ESP, sp + pop)
                return
            self.instructions.append(address)
            assert len(self.instructions) < 20000, label

        self.m.hook_add(UC_HOOK_CODE, hook)
        self.m.emu_start(start, end, count=30000)
        assert self.m.reg_read(UC_X86_REG_EIP) == end, label
        assert self.cursor == len(self.payload), (label, self.cursor, len(self.payload))
        return {'case': label, 'start': hex(start), 'stop': hex(end),
                'original_instructions': len(self.instructions), 'adapter_calls': self.calls,
                'input_bytes': self.cursor, 'output_bytes': len(self.output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', required=True, type=Path)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert digest == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == BASE
    evidence_path = ROOT / 'docs/evidence/gift_delivery_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    blocks = evidence['functions'] + evidence['ranges'] + evidence['resources']
    for block in blocks:
        data = bytes.fromhex(block['bytes'])
        assert len(data) == block['size'] and hashlib.sha256(data).hexdigest() == block['sha256']
        assert pe.get_data(int(block['rva'], 16), len(data)) == data
    mapped = pe.get_memory_mapped_image()
    cases_path = ROOT / 'tests/gift_protocol_cases.json'
    cases = json.loads(cases_path.read_text('utf-8'))['cases']
    fixture_path = ROOT / 'tests/gift_protocol_packets.txt'
    fixed = {label: (int(mid), bytes.fromhex(body)) for label, mid, body in
             (line.split() for line in fixture_path.read_text('ascii').splitlines())}
    for case in cases:
        assert fixed[case['label']] == (case['mid'], bytes.fromhex(case['payload']))
    runs = []
    for case in cases:
        typ, values, body = case['type'], case['fields'], bytes.fromhex(case['payload'])
        if typ == 'GiftSendRequest':
            vm = Machine(mapped)
            vm.string(INPUT, values['recipient_name']); vm.string(INPUT + 24, values['text_at24'])
            put(vm.m, INPUT + 48, values['stock_id']); vm.string(INPUT + 52, values['message'])
            vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
            put(vm.m, STACK + 4, INPUT)
            runs.append(vm.execute(case['label'], 0x650770))
            assert bytes(vm.output) == body
            assert int.from_bytes(vm.m.mem_read(HEADER + 4, 2), 'little') == 818
        elif typ == 'GiftDeleteReceivedRequest':
            vm = Machine(mapped); put(vm.m, INPUT, values['gift_id'])
            vm.m.reg_write(UC_X86_REG_ECX, OBJECT); put(vm.m, STACK + 4, INPUT)
            runs.append(vm.execute(case['label'], 0xe4a180))
            assert bytes(vm.output) == body
            assert int.from_bytes(vm.m.mem_read(HEADER + 4, 2), 'little') == 825
        elif typ == 'GiftSendThanksRequest':
            vm = Machine(mapped)
            put(vm.m, BP - 0x64, values['gift_id'])
            vm.string(BP - 0x60, values['message'])
            put(vm.m, BP - 0x48, values['value_at28'])
            put(vm.m, BP - 0x44, VTABLE); put(vm.m, BP - 0x2c, HEADER)
            runs.append(vm.execute(case['label'], 0xe4bc47, 0xe4bcae))
            assert bytes(vm.output) == body
            assert int.from_bytes(vm.m.mem_read(HEADER + 4, 2), 'little') == 821
        elif typ == 'GiftSendResponse':
            vm = Machine(mapped, body); vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
            put(vm.m, STACK + 4, STREAM)
            runs.append(vm.execute(case['label'], 0x99b580))
            for offset, key in [(0, 'text_at0'), (32, 'text_at32'), (56, 'text_at56')]:
                assert vm.string_value(OBJECT + offset) == native_text(values[key])
            assert get(vm.m, OBJECT + 24) == values['value_at24']
            assert get(vm.m, OBJECT + 28) == values['result']
        elif typ in ('GiftReceivedListResponse', 'GiftSentListResponse'):
            # List header/count is statically verified; execute each original record reader.
            offset = 16 if typ == 'GiftReceivedListResponse' else 12
            for index, row in enumerate(values['records']):
                def wire_text(text):
                    value = raw_text(text); return struct.pack('<I', len(value) // 2) + value
                if typ == 'GiftReceivedListResponse':
                    row_body = struct.pack('<IQI', row['gift_id'], row['wide_at8'], row['value_at16']) + wire_text(row['text_at20']) + struct.pack('<IH', row['stock_id'], row['short_at48']) + wire_text(row['text_at52']) + struct.pack('<Q', row['wide_at80'])
                    start, words, strings, wides, shorts = 0x114dba0, [(4, 'gift_id'), (16, 'value_at16'), (44, 'stock_id')], [(20, 'text_at20'), (52, 'text_at52')], [(8, 'wide_at8'), (80, 'wide_at80')], [(48, 'short_at48')]
                else:
                    row_body = struct.pack('<IQQI', row['gift_id'], row['wide_at8'], row['wide_at16'], row['value_at24']) + wire_text(row['text_at28']) + struct.pack('<IHH', row['stock_id'], row['short_at56'], row['short_at58']) + wire_text(row['text_at60']) + wire_text(row['text_at84'])
                    start, words, strings, wides, shorts = 0x114def0, [(4, 'gift_id'), (24, 'value_at24'), (52, 'stock_id')], [(28, 'text_at28'), (60, 'text_at60'), (84, 'text_at84')], [(8, 'wide_at8'), (16, 'wide_at16')], [(56, 'short_at56'), (58, 'short_at58')]
                assert body[offset:offset + len(row_body)] == row_body
                offset += len(row_body)
                vm = Machine(mapped, row_body); vm.m.reg_write(UC_X86_REG_ECX, OBJECT)
                put(vm.m, STACK + 4, STREAM)
                runs.append(vm.execute(case['label'] + '_' + str(index), start))
                for at, key in words:
                    assert get(vm.m, OBJECT + at) == row[key]
                for at, key in strings:
                    assert vm.string_value(OBJECT + at) == native_text(row[key])
                for width, fields in [(8, wides), (2, shorts)]:
                    for at, key in fields:
                        assert int.from_bytes(vm.m.mem_read(OBJECT + at, width), 'little') == row[key]
            assert offset == len(body)
        elif typ == 'GiftViewThanksResponse':
            vm = Machine(mapped, body); vm.m.reg_write(UC_X86_REG_ECX, OBJECT); vm.m.reg_write(UC_X86_REG_EDX, PTR)
            runs.append(vm.execute(case['label'], 0x10cfea0))
            assert bytes(vm.m.mem_read(OBJECT, 1)) == bytes([bool(values['value_at0'])])
            for at, key in [(4, 'gift_id'), (56, 'value_at56'), (60, 'value_at60')]:
                assert get(vm.m, OBJECT + at) == values[key]
            for at, key in [(8, 'text_at8'), (32, 'text_at32')]:
                assert vm.string_value(OBJECT + at) == native_text(values[key])
            extra = values['extra']
            assert get(vm.m, OBJECT + 64) == extra['value_at0']
            for at, key, width in [(68, 'values_at4', 4), (80, 'values_at16', 4), (92, 'values_at28', 4), (104, 'bytes_at40', 1)]:
                assert vm.vector_value(OBJECT + at, width) == extra[key]
            begin, end = get(vm.m, OBJECT + 116), get(vm.m, OBJECT + 120)
            assert end - begin == len(extra['records_at52']) * 48
            for index, row in enumerate(extra['records_at52']):
                adapted = begin + index * 48
                for at, key in [(0, 'value_at0'), (4, 'value_at4'), (20, 'value_at20')]:
                    assert get(vm.m, adapted + at) == row[key]
                assert vm.vector_value(adapted + 8) == row['values_at8']
                assert vm.string_value(adapted + 24) == native_text(row['text_at24'])
                text = raw_text(row['text_at24'])
                row_body = struct.pack('<III', row['value_at0'], row['value_at4'], len(row['values_at8'])) + b''.join(struct.pack('<I', x) for x in row['values_at8']) + struct.pack('<II', row['value_at20'], len(text) // 2) + text
                child = Machine(mapped, row_body); child.m.reg_write(UC_X86_REG_EBX, OBJECT); child.m.reg_write(UC_X86_REG_EDI, PTR)
                runs.append(child.execute(case['label'] + '_detail_' + str(index), 0x10d94e2, 0x10d955d))
                for at, key in [(0, 'value_at0'), (4, 'value_at4'), (20, 'value_at20')]:
                    assert get(child.m, OBJECT + at) == row[key]
                assert child.vector_value(OBJECT + 8) == row['values_at8']
                assert child.string_value(OBJECT + 24) == native_text(row['text_at24'])
    # Independent instruction-boundary checks of UI-produced values. These are
    # synthetic inputs, separate from the fixed wire fixtures and full UI behavior.
    for mid, start, stop in [(815, 0xe49e3c, 0xe49e70), (816, 0xe49f3a, 0xe49f6e)]:
        for offset in (0, 4, 0xfffffffc):
            vm = Machine(mapped); put(vm.m, BP - 0x38, VTABLE); put(vm.m, BP - 0x20, HEADER)
            vm.m.reg_write(UC_X86_REG_ESI, offset)
            runs.append(vm.execute(f'page_{mid}_{offset}', start, stop))
            assert bytes(vm.output) == struct.pack('<II', offset, 4)
            assert int.from_bytes(vm.m.mem_read(HEADER + 4, 2), 'little') == mid
    for value in (0, 0x7fff, 0x8000, 0xffff):
        vm = Machine(mapped); put(vm.m, BP - 0x64, 0x87654321)
        vm.string(BP - 0x60, 'Thanks\0!')
        vm.m.reg_write(UC_X86_REG_ESI, INPUT)
        vm.m.mem_write(INPUT + 0x678, struct.pack('<H', value))
        runs.append(vm.execute(f'thanks_sign_extension_{value}', 0xe4bc00, 0xe4bcae))
        signed = value if value < 0x8000 else value - 0x10000
        expected = struct.pack('<II', 0x87654321, 8) + raw_text('Thanks\0!') + struct.pack('<i', signed)
        assert bytes(vm.output) == expected
        assert int.from_bytes(vm.m.mem_read(HEADER + 4, 2), 'little') == 821
    result = {'passed': True, 'sample_sha256': digest, 'verified_blocks': len(blocks),
              'evidence_sha256': hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
              'fixture_sha256': hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
              'cases_sha256': hashlib.sha256(cases_path.read_bytes()).hexdigest(),
              'fixture_count': len(cases), 'runs': runs, 'original_ui': False,
              'limits': 'Native send/delete-received writers, thanks-write and page-write fragments, send-response reader, received/sent record readers, thanks response and nested detail readers execute. Thanks UI signed-short conversion executes in a bounded fragment. Other scalar handlers and response list headers have static evidence only. Packet allocation, byte I/O, shared string decoding/assignment, vector reserve/destruction and the outer compound-vector container are adapted. Its container output is checked and its detail reader executes separately. No full UI, network or gifting transaction.'}
    (ROOT / 'docs/evidence/gift_protocol_probe.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print(json.dumps({'passed': True, 'blocks': len(blocks), 'fixtures': len(cases), 'runs': len(runs),
                      'instructions': sum(x['original_instructions'] for x in runs),
                      'adapters': sum(len(x['adapter_calls']) for x in runs)}))


if __name__ == '__main__':
    main()
