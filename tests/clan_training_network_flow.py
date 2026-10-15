"""Independent training wire sequence on the authenticated main/clan sockets.

Fixture SQL explicitly changes trusted level/time; no client report grants them.
"""
import struct


def exercise(guest, peer, auth, sql, check, z):
    actions = 0
    identity = guest.character
    absent = struct.pack('<BII', 0, 0, 0) + z('') + struct.pack('<d', 73051.0)
    enrolled = struct.pack('<BII', 1, 99, 100) + z('훈련소') + struct.pack('<d', 73051.0)

    def request(mid, body=b''):
        nonlocal actions
        actions += 1
        peer.send(mid, body, split=True)

    def join(success):
        request(1106)
        check(peer.receive(1107) == (struct.pack('<II', 99, 100) + z('훈련소') if success else bytes(4)), 'Conditional native training join layout')
        check(peer.receive(1016) == (enrolled if success else current), 'Join result uses persisted profile')

    def removal(status):
        check(peer.receive(1037) == z('훈련소') + struct.pack('<I', identity) + z('RoomProbe') + struct.pack('<II', 100, status), 'Native training old/new exit status')
        check(peer.receive(1016) == absent, 'Training transition returns canonical no-clan profile')

    current = absent
    sql(f'UPDATE Characters SET Level=46 WHERE CharacterId={identity};')
    join(False)
    sql(f'UPDATE Characters SET Level=45 WHERE CharacterId={identity};')
    join(True); current = enrolled
    guest.expect_clan(99)
    join(False)
    for status in (0xffffffc4, 123):
        request(1108, struct.pack('<I', status))
        check(peer.receive(1016) == enrolled, 'Reported graduation or unknown status cannot remove low-level member')
    guest.send(948, struct.pack('<III', 999999, 255, 999999))
    guest.send(2025, struct.pack('<I', 999999) + struct.pack('<I', 5) + 'Spoof'.encode('utf-16le') + struct.pack('<I',101))
    guest.issue()
    request(1015); check(peer.receive(1016) == enrolled, 'Main self-reports cannot grant clan or officer status')
    check(sql(f'SELECT ClanId,MembershipStatus FROM ClanMembers WHERE CharacterId={identity};') == '99\t100', 'Trusted training membership persists')
    request(1108, struct.pack('<I', 0xffffffec)); removal(0xffffffec); current = absent
    guest.expect_clan(0)
    before = sql(f'SELECT LastChangedAtUnixSeconds FROM TrainingClanHistory WHERE CharacterId={identity};')
    request(1036); check(peer.receive(1016) == absent, 'Following native ordinary leave is idempotent')
    check(sql(f'SELECT LastChangedAtUnixSeconds FROM TrainingClanHistory WHERE CharacterId={identity};') == before, 'Duplicate leave does not restart cooldown')
    join(False)
    peer = auth(guest.issue())
    request(1015)
    check(peer.receive(1016) == struct.pack('<BII',0,99,0xffffffec)+z('훈련소')+struct.pack('<d',73051.0), 'Reconnection recovers durable manual exit')
    check(peer.receive(1016) == absent, 'Manual recovery returns canonical state')
    request(1015); check(peer.receive(1016) == absent, 'Manual exit notice once per connection')
    # Explicit isolated fixture age; the server itself never accepts a client clock.
    sql(f'UPDATE TrainingClanHistory SET LastChangedAtUnixSeconds=LastChangedAtUnixSeconds-61 WHERE CharacterId={identity};')
    join(True); current = enrolled
    guest.expect_clan(99)
    sql(f'UPDATE Characters SET Level=46 WHERE CharacterId={identity};')
    guest.socket.settimeout(18)
    try:
        check(guest.receive(1515) == b'', 'Main poll emits native empty graduation notification')
    finally:
        guest.socket.settimeout(5)
    removal(0xffffffc4); current = absent
    guest.expect_clan(0)
    request(1108, struct.pack('<I',0xffffffc4))
    check(peer.receive(1016) == absent, 'Native graduation reply is an idempotent observation')
    peer = auth(guest.issue())
    request(1017)
    check(peer.receive(1016) == struct.pack('<BII',0,99,0xffffffc4)+z('훈련소')+struct.pack('<d',73051.0), 'Reconnection recovers durable graduation')
    check(peer.receive(1016) == absent, 'Graduation recovery returns canonical state')
    sql(f'UPDATE Characters SET Level=1 WHERE CharacterId={identity};')
    join(False)
    check(sql(f'SELECT Graduated,ExitStatus FROM TrainingClanHistory WHERE CharacterId={identity};') == '1\t4294967236', 'Graduation remains durable after changed level')
    # Restore this independent fixture before the existing ordinary-clan scenarios.
    sql(f'DELETE FROM TrainingClanHistory WHERE CharacterId={identity}; DELETE FROM Clans WHERE ClanId=99;')
    request(1015); check(peer.receive(1016) == absent, 'Fixture restored without stale removal')
    return peer, actions
