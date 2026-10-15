"""Synthetic TCP test framing from preserved original instruction fixtures."""
from functools import reduce
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
FIRST = bytes.fromhex('5f3e3272c11846875d2e294e6879866cb58d56f138c148425f0ccd80e8e2fef0425ff7597e5fa58d3d76e711416267597f')
SECOND = bytes.fromhex('230024002500730065007200740033003400250077006500')


class Cipher:
    def __init__(self, outgoing):
        self.a, self.b = (0, 1) if outgoing else (5, 3)

    def transform(self, data, sign):
        result = bytearray()
        for byte in data:
            result.append((byte + sign * (FIRST[self.a] + SECOND[self.b])) & 255)
            self.a = (self.a + 1) % len(FIRST)
            self.b = (self.b + 1) % len(SECOND)
        return bytes(result)


def xor(data):
    return reduce(int.__xor__, data, 0)


def frame(message, payload=b''):
    header = struct.pack('<IHB', len(payload), message, xor(payload))
    return header + bytes([xor(header)]) + payload


def text(value):
    raw = value.encode('utf-16le')
    return struct.pack('<I', len(raw) // 2) + raw


def verify_wire():
    count = 0
    for line in (ROOT / 'tests/client_wire_packets.txt').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        _, _, plain, outgoing, incoming = line.split('|')
        raw = bytes.fromhex(plain)
        assert Cipher(True).transform(raw, 1).hex() == outgoing
        assert Cipher(False).transform(bytes.fromhex(incoming), -1) == raw
        count += 1
    assert count == 10


def host_create_payload():
    # E047's original-writer zero descriptor. Only the title and ordinary mode
    # are changed; all unknown configuration fields retain the fixture's zeros.
    lines = (ROOT / 'tests/session_descriptor_packets.txt').read_text().splitlines()
    zero = next(bytes.fromhex(line.split('|')[1]) for line in lines if line.startswith('Zero|'))
    descriptor = bytearray(zero)
    assert len(descriptor) == 225 and not any(descriptor)
    struct.pack_into('<H', descriptor, 32, 1)  # Configuration.ShortAt4
    descriptor[6:10] = text('PeerHost')
    return bytes(descriptor) + text('') + b'\xba'


def host_start_payload():
    # The exact original-writer E089 vector for the previously observed map.
    lines = (ROOT / 'tests/room_start_packets.txt').read_text().splitlines()
    return next(bytes.fromhex(line.split('|')[2]) for line in lines if line.startswith('rel_Village01_Byway|'))

