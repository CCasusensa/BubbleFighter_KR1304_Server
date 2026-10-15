"""Native clan lifecycle and main C2025 exercised across three authenticated TCP peers."""
import struct


def exercise(host, guest, observer, active, guest_clan, observer_clan,
             sql, check, z):
    actions = 0
    peers = {'host': active, 'guest': guest_clan, 'observer': observer_clan}
    games = {'host': host, 'guest': guest, 'observer': observer}
    names = {'host': 'RoomPeer', 'guest': 'RoomProbe', 'observer': 'RoomObserver'}

    def send(who, mid, payload=b''):
        nonlocal actions
        peers[who].send(mid, payload, split=True)
        actions += 1

    def profile(who, role):
        expected = (struct.pack('<BII', int(role >= 100), 9000 if role else 0, role) +
                    z('NetClan' if role else '') + struct.pack('<d', 73051.0))
        check(peers[who].receive(1016) == expected, 'Trusted lifecycle profile')

    def details(who, mode, count, deputy=''):
        expected = (z('NetClan') + struct.pack('<I', 0) + z('RoomPeer') +
                    struct.pack('<I', 0) + z(deputy) +
                    struct.pack('<IIdQIII', 1, 0, 45292.0, 0, mode, count, 0) +
                    z('') + z('Hi') + struct.pack('<II', 0, 30))
        check(peers[who].receive(1018) == expected, 'Committed recruitment/details notification')

    def roster(who, members):
        body = struct.pack('<I', len(members))
        for member, role in members:
            body += struct.pack('<I', games[member].character) + z(names[member]) + struct.pack('<III', 0, 0, role)
        check(peers[who].receive(1012) == body, 'Committed accepted roster notification')

    def event(who, target, old, new, mid=1041):
        tail = (old, new) if mid == 1037 else (new, old)
        body = z('NetClan') + struct.pack('<I', games[target].character) + z(names[target]) + struct.pack('<II', *tail)
        check(peers[who].receive(mid) == body, 'Native membership notification field order')

    def recruit(mode, count=2):
        send('host', 1102, struct.pack('<II', 9000, mode))
        check(active.receive(1103) == struct.pack('<III', 0, 1, mode), 'Recruitment write result')
        details('guest', mode, count)

    def change(actor, target, old, new, result=1, current=None):
        send(actor, 1039, struct.pack('<I', games[target].character) + z(names[target]) + struct.pack('<II', old, new))
        response_new, response_old = (new, old) if result == 1 else (current, current)
        expected = struct.pack('<I', result) + z(names[target]) + struct.pack('<II', response_new, response_old)
        check(peers[actor].receive(1040) == expected, 'Member mutation response matches trusted transition/refusal')

    def join(who, mode='manual', result=1, role=1, supplied_name=None):
        body = struct.pack('<I', 9000) + z('NetClan')
        if mode == 'auto': body += z(supplied_name if supplied_name is not None else names[who])
        send(who, 1033 if mode == 'auto' else 1032, body)
        expected = struct.pack('<III', result, role, 9000 if role else 0) + z('NetClan' if role else '')
        check(peers[who].receive(1034) == expected, 'Join result preserves authoritative role')

    recruit(1)
    change('host', 'observer', 1, 100)
    event('host', 'observer', 1, 100)
    event('guest', 'observer', 1, 100)
    profile('observer', 1); event('observer', 'observer', 1, 100); profile('observer', 100)
    observer.expect_clan(9000)
    for cid, cname, role in ((9000, 'NetClan', 100), (12345, 'Forged', 101), (0, '', 0)):
        encoded = cname.encode('utf-16le')
        observer.send(2025, struct.pack('<II', cid, len(encoded)//2) + encoded + struct.pack('<I', role))
        observer.issue()  # Proves the main connection remains usable; creates no clan authority.
        send('observer', 1015); profile('observer', 100)
    change('host', 'observer', 100, 102)
    event('host', 'observer', 100, 102); event('guest', 'observer', 100, 102)
    event('observer', 'observer', 100, 102); profile('observer', 102)
    change('observer', 'guest', 100, 102, result=0, current=100)
    change('host', 'guest', 100, 102, result=0xffffff9a, current=100)
    change('host', 'guest', 100, 101, result=0, current=100)
    change('host', 'observer', 102, 100)
    event('host', 'observer', 102, 100); event('guest', 'observer', 102, 100)
    event('observer', 'observer', 102, 100); profile('observer', 100)
    send('observer', 1036)
    event('observer', 'observer', 100, 0xffffffec, 1037); profile('observer', 0)
    event('host', 'observer', 100, 0xffffffec, 1037); event('guest', 'observer', 100, 0xffffffec, 1037)
    observer.expect_clan(0)
    join('observer'); check(active.receive(1035) == b'', 'Officers learn about manual application')
    send('observer', 1063, struct.pack('<I', 9001))
    check(observer_clan.receive(1064) == bytes(8), 'Cannot cancel a different clan application')
    send('observer', 1063, struct.pack('<I', 9000))
    check(observer_clan.receive(1064) == struct.pack('<II', 1, 0), 'Cancellation committed')
    profile('observer', 0)
    check(active.receive(1065) == struct.pack('<II', 1, 0)+z('RoomObserver'), 'Officer cancellation notification')
    join('observer'); check(active.receive(1035) == b'', 'Application can be submitted again')
    recruit(0)
    send('host', 1069); details('host', 0, 2)
    pending = struct.pack('<II', 1, observer.character)+z('RoomObserver')+struct.pack('<IIQIIII', 0, 1, 0, 0, 0, 0, 0)
    check(active.receive(1070) == pending, 'Stop exposes current applicants for native individual rejection')
    change('host', 'observer', 1, 100, result=0, current=1)
    change('host', 'observer', 1, 0xfffffff6)
    event('observer', 'observer', 1, 0xfffffff6); profile('observer', 0)
    send('host', 1069); details('host', 0, 2)
    check(active.receive(1070) == bytes(4), 'Rejected application disappears from the next native refresh')
    join('observer', result=0xfffffffb, role=0)
    recruit(2)
    join('observer', 'auto', result=0, role=0, supplied_name='Forged')
    join('observer', 'auto', role=100); profile('observer', 100)
    observer.expect_clan(9000)
    for who in ('host', 'guest'):
        details(who, 2, 3); roster(who, [('host', 101), ('guest', 100), ('observer', 100)])
    send('guest', 1036)
    event('guest', 'guest', 100, 0xffffffec, 1037); profile('guest', 0)
    event('host', 'guest', 100, 0xffffffec, 1037); event('observer', 'guest', 100, 0xffffffec, 1037)
    guest.expect_clan(0)
    sql('UPDATE Clans SET MemberCapacity=2 WHERE ClanId=9000;')
    join('guest', 'auto', result=0, role=0)
    sql('UPDATE Clans SET MemberCapacity=30 WHERE ClanId=9000;')
    join('guest', 'auto', role=100); profile('guest', 100)
    guest.expect_clan(9000)
    for who in ('host', 'observer'):
        details(who, 2, 3); roster(who, [('host', 101), ('guest', 100), ('observer', 100)])
    change('host', 'observer', 100, 0xffffffe2)
    event('host', 'observer', 100, 0xffffffe2); event('guest', 'observer', 100, 0xffffffe2)
    event('observer', 'observer', 100, 0xffffffe2); profile('observer', 0)
    observer.expect_clan(0)
    recruit(1)
    join('observer'); check(active.receive(1035) == b'', 'Final pending application uses the public path')
    recruit(0)
    check(sql('SELECT GROUP_CONCAT(MembershipStatus ORDER BY CharacterId) FROM ClanMembers WHERE ClanId=9000;') == '101,100,1',
          'Lifecycle leaves expected persistent identities for subsequent handoff tests')
    return actions
