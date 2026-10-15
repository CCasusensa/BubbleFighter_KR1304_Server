"""Independently pack S1012 scenes from real main and auxiliary socket actions."""
import struct
from tcp_fixture import host_create_payload


def exercise(host, guest, active, guest_clan, auth, sql, check, z):
    queries = 0
    before = sql(f'SELECT CharacterId,Level FROM Characters WHERE CharacterId IN ({host.character},{guest.character}) ORDER BY CharacterId;')

    def roster(first, second):
        nonlocal queries
        active.send(1011)
        queries += 1
        expected = struct.pack('<I', 2)
        for game, name, scene, role in ((host, 'RoomPeer', first, 101), (guest, 'RoomProbe', second, 100)):
            expected += struct.pack('<I', game.character) + z(name) + struct.pack('<III', scene, 0, role)
        check(active.receive(1012) == expected, 'Roster scene derives from committed main session; other opaque fields unchanged')

    def menu(game):
        game.send(105)
        for mid in (105, 117, 806, 837, 802, 810): game.receive(mid)

    roster(1, 1)
    for level, scene in ((0xffffffff, 6), (0, 0), (45, 2)):
        active.send(1045, struct.pack('<II', level, scene), split=True)
        roster(1, 1)  # A guessed S1046 or untrusted scene would fail here.
    host.send(106); host.receive(117)
    roster(3, 1)
    guest.send(106); guest.receive(117); host.receive(503); host.receive(501)
    roster(3, 3)
    host.send(402, host_create_payload()); host.receive(102); host.receive(117)
    guest.receive(511); guest.receive(501)
    roster(2, 3)  # Native model2 includes waiting rooms.
    host.send(404); host.receive(117); guest.receive(503); guest.receive(501)
    roster(3, 3)
    menu(guest); host.receive(511); host.receive(501)
    roster(3, 1)
    menu(host)
    roster(1, 1)
    for size in (0, 7, 9):
        guest_clan.send(1045, bytes(size)); guest_clan.closed()
        roster(1, 1)  # Auxiliary closure leaves the trusted main scene visible.
        guest_clan = auth(guest.issue())
        guest_clan.send(1045, struct.pack('<II', 0xffffffff, 6))
        guest_clan.send(1015); check(struct.unpack_from('<BII', guest_clan.receive(1016)) == (1, 9000, 100),
                                   'Presence report cannot change membership or role')
    check(sql(f'SELECT CharacterId,Level FROM Characters WHERE CharacterId IN ({host.character},{guest.character}) ORDER BY CharacterId;') == before,
          'Forged reports and all scene changes leave durable level unchanged')
    return guest_clan, queries
