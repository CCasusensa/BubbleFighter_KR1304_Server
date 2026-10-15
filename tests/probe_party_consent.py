"""Verify saved party-consent IDA ranges and S122 branches against BFCLIENT02."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct

import pefile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sample', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    evidence_path = root / 'docs/evidence/party_consent_discovery.json'
    evidence = json.loads(evidence_path.read_text(encoding='utf-8'))
    digest = hashlib.sha256(args.sample.read_bytes()).hexdigest()
    assert digest == evidence['sha256'] == '7ee7872aaeb78104f6b9d81fcb40a0ff4f0fbc9826712f90097c6820bb0f42e4'
    pe = pefile.PE(str(args.sample), fast_load=True)
    assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
    rows = evidence['functions'] + evidence['failure_resources'] + [evidence['failure_switch']]
    verified = []
    for row in rows:
        raw = bytes.fromhex(row['bytes'])
        rva = int(row['rva'], 16)
        assert int(row['va'], 16) - 0x400000 == rva
        assert pe.get_data(rva, len(raw)) == raw, row['va']
        verified.append(dict(va=row['va'], rva=row['rva'], size=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
    switch = evidence['failure_switch']
    assert switch['lowcase'] == 0 and switch['count'] == 24
    table = bytes.fromhex(switch['bytes'])
    cases = []
    for reason, branch, resource, suffix in [(2, 0x9a9e9f, 0x15b3df8, 'SameParty'),
                                             (14, 0x9aa260, 0x15b4150, 'FullParty')]:
        assert struct.unpack_from('<I', table, reason * 4)[0] == branch
        assert pe.get_data(branch - 0x400000, 5) == b'\x68' + struct.pack('<I', resource)
        text = next(x for x in evidence['failure_resources'] if int(x['va'], 16) == resource)
        assert bytes.fromhex(text['bytes']).decode('utf-16le') == 'party/MoveToInviterFailed.' + suffix + '\0'
        cases.append(dict(reason=reason, branch_va=hex(branch), resource_va=hex(resource), resource=text['text']))
    result = dict(utc=datetime.now(timezone.utc).isoformat(), sample_sha256=digest,
                  image_base='0x400000', passed=True, functions=6, ranges=verified,
                  error_cases=cases, original_instructions_executed=0,
                  original_client_ui=False, new_codecs=0,
                  unclassified_function='0x906ac0 (query 0x906d30 resolves here; not used for protocol conclusions)',
                  evidence_sha256=hashlib.sha256(evidence_path.read_bytes()).hexdigest())
    (root / 'docs/evidence/party_consent_analysis_validation.json').write_text(
        json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('PASS: 6 IDA functions, 2 resources, 1 switch table; native S122 reasons 2/14 verified. No instruction execution or UI.')


if __name__ == '__main__':
    main()
