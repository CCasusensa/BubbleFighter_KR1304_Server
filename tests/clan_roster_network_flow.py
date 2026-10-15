"""Read-only roster exchange using independently packed original field layouts."""
import struct


def exercise(host, guest, observer, active, guest_clan, observer_clan,
             auth, sql, check, z):
    queries = 0

    def query(peer, mid):
        nonlocal queries
        peer.send(mid, struct.pack('<I', 1) if mid == 1013 else b'', split=True)
        queries += 1

    def member_rows(extended=False, guest_role=100):
        rows = b''
        for game, name, role in ((host, 'RoomPeer', 101), (guest, 'RoomProbe', guest_role)):
            rows += struct.pack('<I', game.character) + z(name)
            if extended:
                rows += struct.pack('<IIIIQQQIIII', 0, role, 0, 0, 0, 0, 0, 0, 0, 0, 0)
            else:
                rows += struct.pack('<III', 0, 0, role)
        return (struct.pack('<II', 0, 2) if extended else struct.pack('<I', 2)) + rows

    def applicants(peer, mode, deputy=''):
        query(peer, 1069)
        details = (z('NetClan') + struct.pack('<I', 0) + z('RoomPeer') +
                   struct.pack('<I', 0) + z(deputy) +
                   struct.pack('<IIdQIII', 1, 0, 45292.0, 0, mode, 2, 0) +
                   z('') + z('Hi') + struct.pack('<II', 0, 30))
        check(peer.receive(1018) == details, 'Fresh trusted mode precedes applicant list')
        expected = (struct.pack('<II', 1, observer.character) + z('RoomObserver') +
                    struct.pack('<IIQIIII', 0, 1, 0, 0, 0, 0, 0))
        check(peer.receive(1070) == expected, 'Officer receives applicants with exact opaque zero fields')

    for peer in (active, guest_clan):
        query(peer, 1011)
        check(peer.receive(1012) == member_rows(), 'Accepted roster excludes pending applicant')
        query(peer, 1013)
        check(peer.receive(1014) == member_rows(True), 'Extended roster keeps trusted identity and role')
    for mid in (1011, 1013, 1069):
        query(observer_clan, mid)
        check(struct.unpack_from('<BII', observer_clan.receive(1016)) == (0, 9000, 1),
              'Applicant gets only trusted profile')
    query(guest_clan, 1069)
    check(struct.unpack_from('<BII', guest_clan.receive(1016)) == (1, 9000, 100),
          'Ordinary member cannot enumerate applicants')
    for mode in (0, 1):
        sql(f'UPDATE Clans SET IsRecruiting={mode} WHERE ClanId=9000;')
        applicants(active, mode)
    sql(f'UPDATE ClanMembers SET MembershipStatus=102 WHERE CharacterId={guest.character};')
    applicants(guest_clan, 1, 'RoomProbe')
    sql(f'UPDATE ClanMembers SET MembershipStatus=100 WHERE CharacterId={guest.character};')
    query(guest_clan, 1069)
    check(struct.unpack_from('<BII', guest_clan.receive(1016)) == (1, 9000, 100),
          'Authority downgrade applies without reauthentication')
    sql('UPDATE Clans SET IsRecruiting=0 WHERE ClanId=9000;')
    for mid, body in ((1011, b'\0'), (1069, b'\0'), (1013, b''),
                      (1013, bytes(4)), (1013, struct.pack('<I', 2))):
        peer = auth(guest.issue())
        peer.send(mid, body)
        peer.closed()
    disabled = auth(guest.issue())
    sql("UPDATE Accounts SET IsDisabled=1 WHERE Username='net_guest';")
    disabled.send(1011)
    disabled.closed()
    sql("UPDATE Accounts SET IsDisabled=0 WHERE Username='net_guest';")
    replacement = auth(guest.issue())
    query(replacement, 1011)
    check(replacement.receive(1012) == member_rows(), 'Roster survives previous rejected connection')
    check(sql('SELECT COUNT(*) FROM Clans;') == '1' and
          sql('SELECT COUNT(*) FROM ClanMembers;') == '3', 'Roster operations are read-only')
    return replacement, queries
