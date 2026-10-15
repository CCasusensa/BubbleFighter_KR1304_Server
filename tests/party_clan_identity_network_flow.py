"""Independent S201 clan field/whole-party checks on real main and clan sockets."""
import struct
import time

from invitation_network_flow import ask
from social_network_flow import Reader
from tcp_fixture import text

NAMES = {'host': 'RoomPeer', 'guest': 'RoomProbe', 'observer': 'RoomObserver'}
ITEMS = [201005, 202006, 205033, 207001, 300027]


def observe(peer, payload, check):
    members = getattr(peer, 'party_members', [peer])
    r = Reader(payload)
    check((r.u32(), r.u32()) == (2, len(members)), 'Clan S201 exact state/member count')
    clans = []
    for member in members:
        check((r.u32(), r.text()) == (member.character, NAMES[member.role]), 'Clan S201 authenticated member identity/order')
        check(tuple(r.u32() for _ in range(3)) == (0xffffffff, 0xffffffff, 0), 'Clan S201 preserves all three opaque defaults')
        clans.append(r.u32())
    expected = {m.character for m in members if getattr(m, 'party_equipped', False)}
    check(r.u32() == len(expected), 'Clan S201 exact initialized equipment count')
    seen = set()
    for _ in expected:
        key, model, count = r.u32(), r.u32(), r.u32()
        check(key in expected and key not in seen and model == 300027, 'Clan refresh preserves owned equipment/model')
        seen.add(key)
        check(count == len(ITEMS) and [r.u32() for _ in range(count)] == ITEMS, 'Clan refresh preserves exact equipped items')
        check(tuple(r.u32() for _ in range(4)) == (0, 0, 0, 0), 'Clan refresh preserves empty extra equipment fields')
    r.end()
    peer.party_clans = clans
    peer.party_updates = getattr(peer, 'party_updates', 0) + 1
    return clans


def expect(peer, clans, check, timeout=3):
    expected = clans if isinstance(clans, list) else [clans]
    deadline = time.monotonic() + timeout
    previous = peer.socket.gettimeout()
    try:
        while getattr(peer, 'party_clans', None) != expected:
            peer.socket.settimeout(max(0.1, deadline-time.monotonic()))
            peer.receive(201)  # Every intervening complete S201 is validated.
            check(time.monotonic() <= deadline, 'Committed clan reaches main party before deadline')
        check(peer.party_clans == expected, 'S201 clan IDs equal controlled persistent membership')
        peer.party_assertions = getattr(peer, 'party_assertions', 0) + 1
    finally:
        peer.socket.settimeout(previous)


def exercise(host, guest, observer, active, guest_clan, observer_clan, sql, check, z):
    games = [host, guest, observer]
    for role, peer in zip(NAMES, games):
        assert peer.role == role
    # First two are already initialized by master-transfer/presence exercises.
    observer.send(521)
    for mid in (105, 117, 806, 837, 802, 810, 170, 201): observer.receive(mid)
    for peer, clan_id in zip(games, [9000, 9000, 0]):
        peer.issue()  # Drain older main publications before changing party shape.
        expect(peer, clan_id, check, timeout=12)
    _, move = ask(host, guest, NAMES['host'], NAMES['guest'], check, lambda *a, **k: None, text)
    host.party_members = guest.party_members = [host, guest]
    guest.send(115, move); guest.receive(117)
    for peer in [host, guest]:
        peer.receive(201)
        check(peer.receive(206) == text(NAMES['guest']), 'Consent joins before clan refresh checks')
        expect(peer, [9000, 9000], check)
    _, move = ask(host, observer, NAMES['host'], NAMES['observer'], check, lambda *a, **k: None, text)
    for peer in games: peer.party_members = games
    observer.send(115, move); observer.receive(117)
    for peer in games:
        peer.receive(201)
        check(peer.receive(206) == text(NAMES['observer']), 'Pending clan applicant may join ordinary party')
        expect(peer, [9000, 9000, 0], check)
    # Accept the existing pending applicant through the trusted clan handler.
    sql('UPDATE Clans SET IsRecruiting=1 WHERE ClanId=9000;')
    active.send(1039, struct.pack('<I', observer.character) + z(NAMES['observer']) + struct.pack('<II', 1, 100))
    check(active.receive(1040) == struct.pack('<I',1)+z(NAMES['observer'])+struct.pack('<II',100,1), 'Trusted applicant acceptance committed')
    event = z('NetClan') + struct.pack('<I', observer.character) + z(NAMES['observer']) + struct.pack('<II',100,1)
    for peer in [active, guest_clan]: check(peer.receive(1041) == event, 'Accepted member event exact')
    check(observer_clan.receive(1016) == struct.pack('<BII',0,9000,1)+z('NetClan')+struct.pack('<d',73051.0), 'Native acceptance seeds the prior pending profile')
    check(observer_clan.receive(1041) == event, 'Accepted target event follows pending profile')
    check(struct.unpack_from('<BII',observer_clan.receive(1016)) == (1,9000,100), 'Accepted canonical profile')
    for peer in games: expect(peer, [9000,9000,9000], check)
    observer.send(2025, struct.pack('<II',123456,0)+struct.pack('<I',101))
    observer.send(948, struct.pack('<III',123456,255,123456))
    for peer in games:
        peer.issue()
        expect(peer,[9000,9000,9000],check)
    # Auxiliary loss cannot hide this change from the owning main or party peers.
    observer_clan.socket.close()
    active.send(1039, struct.pack('<I',observer.character)+z(NAMES['observer'])+struct.pack('<II',100,0xffffffe2))
    check(active.receive(1040) == struct.pack('<I',1)+z(NAMES['observer'])+struct.pack('<II',0xffffffe2,100), 'Officer kicks accepted member with auxiliary disconnected')
    event = z('NetClan')+struct.pack('<I',observer.character)+z(NAMES['observer'])+struct.pack('<II',0xffffffe2,100)
    for peer in [active,guest_clan]: check(peer.receive(1041) == event, 'Kick delivered to remaining clan members')
    for peer in games: expect(peer,[9000,9000,0],check)
    # Restore only the isolated DB fixture;10-second refresh keeps applicant0.
    sql(f'INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES({observer.character},9000,1); UPDATE Clans SET IsRecruiting=0 WHERE ClanId=9000;')
    observer.party_members=[observer]
    host.party_members=guest.party_members=[host,guest]
    observer.send(202)
    observer.receive(201); check(observer.receive(204)==b'', 'Applicant leaves ordinary party')
    for peer in [host,guest]:
        peer.receive(201); check(peer.receive(205)==text(NAMES['observer']), 'Other members receive complete departure')
    guest.party_members=[guest]; host.party_members=[host]
    guest.send(202)
    guest.receive(201); check(guest.receive(204)==b'', 'Accepted clan member returns to solo party')
    host.receive(201); check(host.receive(205)==text(NAMES['guest']), 'Leader retains trusted solo clan')
    for peer,clan_id in zip(games,[9000,9000,0]): expect(peer,clan_id,check)
