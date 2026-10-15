"""Compare current IDA waiting-room departure evidence against BFCLIENT02 PE."""
import datetime
import hashlib
import json
from pathlib import Path

import pefile


def main():
    root = Path(__file__).resolve().parents[1]
    source = root / 'docs/evidence/party_room_exit_ida.json'
    evidence = json.loads(source.read_text(encoding='utf-8'))
    sample = Path('F:/BubbleFighter1.3.16/BubbleFighterU.exe').read_bytes()
    expected = '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    assert hashlib.sha256(sample).hexdigest() == evidence['sha256'] == expected
    pe = pefile.PE(data=sample, fast_load=True)
    assert pe.FILE_HEADER.Machine == 0x14c
    assert pe.OPTIONAL_HEADER.ImageBase == int(evidence['image_base'], 16) == 0x400000
    ranges = []
    for row in evidence['functions']:
        va, rva = int(row['va'], 16), int(row['rva'], 16)
        raw = bytes.fromhex(row['bytes'])
        assert va - 0x400000 == rva and len(raw) == int(row['end'], 16) - va
        assert pe.get_data(rva, len(raw)) == raw
        ranges.append({'va': row['va'], 'rva': row['rva'], 'size': len(raw),
                       'sha256': hashlib.sha256(raw).hexdigest()})
    assert {r['va'] for r in ranges} == {'0xaa5b50', '0x99d510', '0x62a880',
                                       '0x9db1e0', '0x9dbb20', '0x9da3c0'}
    report = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'passed': True, 'sample_sha256': expected,
              'evidence_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'ranges': ranges, 'original_instructions_executed': 0,
              'new_codecs': 0, 'original_ui': False,
              'limits': 'Byte comparison and static caller/assembly evidence only. No full taskbar, conditional automatic departure, original UI, or special-mode execution.'}
    (root / 'docs/evidence/party_room_exit_probe.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: {len(ranges)} native PE function ranges; no original execution claimed.')


if __name__ == '__main__':
    main()
