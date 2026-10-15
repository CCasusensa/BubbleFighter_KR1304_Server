"""Cross-check this increment's IDA evidence against the preserved x86 PE sample."""
import argparse
import hashlib
import json
from pathlib import Path

import pefile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    raw = args.sample.read_bytes()
    evidence_path = ROOT / 'docs/evidence/clan_authentication_ida.json'
    evidence = json.loads(evidence_path.read_text('utf-8'))
    assert hashlib.sha256(raw).hexdigest() == evidence['sample_sha256']
    pe = pefile.PE(data=raw)
    assert pe.FILE_HEADER.Machine == 0x14c and pe.OPTIONAL_HEADER.ImageBase == 0x400000
    functions = list(evidence['additional_functions'])
    for reference in evidence['functions']:
        source = json.loads((ROOT / reference['evidence']).read_text('utf-8'))
        row = next(row for row in source['functions'] if row['va'] == reference['va'])
        assert row['sha256'] == reference['sha256'] and row['rva'] == reference['rva']
        functions.append(row)
    assert len({row['va'] for row in functions}) == len(functions)
    for row in functions + evidence['resources']:
        blob = bytes.fromhex(row['bytes'])
        assert int(row['va'],16) - 0x400000 == int(row['rva'],16)
        assert hashlib.sha256(blob).hexdigest() == row['sha256']
        assert pe.get_data(int(row['rva'],16),len(blob)) == blob, row['va']
    report = dict(passed=True, functions=len(functions), resources=len(evidence['resources']),
                  evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest(),
                  sample_sha256=evidence['sample_sha256'], original_instructions_executed=0,
                  scope='Read-only IDA/PE equality; no UI, client process or full native handlers executed.')
    (ROOT / 'docs/evidence/clan_authentication_native.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__': main()
