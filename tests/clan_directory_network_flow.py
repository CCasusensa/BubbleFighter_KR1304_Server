"""Authenticated directory exchange against independently packed native layouts."""
import struct

def exercise(host, guest, active, guest_clan, observer_clan, auth, sql, check, z):
    queries = 0
    def exchange(peer, mode, page=1, name='', recruiting=0):
        nonlocal queries
        peer.send(1019, struct.pack('<II', mode, page) + z(name), split=True)
        row = (struct.pack('<I', 9000) + z('NetClan') + struct.pack('<I', 0) +
               z('RoomPeer') + struct.pack('<IIdQIII', 1, 0, 45292.0, 0, recruiting, 2, 0) +
               z('Hi') + struct.pack('<dQI', 73051.0, 0, 0))
        visible = page == 1 and (mode != 4 or name == 'NetClan') and (mode not in (7, 8) or recruiting != 0)
        expected = struct.pack('<III', int(visible), 0, page) + z('') + (row if visible else b'')
        check(peer.receive(1020) == expected, 'Trusted directory exact native field order and public role access')
        queries += 1
    for peer in (active, guest_clan, observer_clan):
        for mode in (4, 5, 6, 7, 8):
            exchange(peer, mode, name='NetClan' if mode == 4 else '')
    exchange(active, 4, name='netclan')
    exchange(active, 4, name='%')
    exchange(active, 6, page=2)
    exchange(active, 6, page=10000)
    sql('UPDATE Clans SET IsRecruiting=1 WHERE ClanId=9000;')
    for mode in (4, 5, 6, 7, 8):
        exchange(active, mode, name='NetClan' if mode == 4 else '', recruiting=1)
    sql('UPDATE Clans SET IsRecruiting=0 WHERE ClanId=9000;')
    for mode, page, name in ((4,0,'NetClan'),(6,10001,''),(4,1,''),(4,1,'1234567890123'),
                             (9,1,''),(6,1,'unexpected')):
        peer = auth(guest.issue())
        peer.send(1019, struct.pack('<II', mode, page) + z(name))
        peer.closed()
    peer = auth(guest.issue())
    peer.send(1019, struct.pack('<II',6,1)+b'A')
    peer.closed()
    disabled = auth(guest.issue())
    sql("UPDATE Accounts SET IsDisabled=1 WHERE Username='net_guest';")
    disabled.send(1019, struct.pack('<II',6,1)+z(''))
    disabled.closed()
    sql("UPDATE Accounts SET IsDisabled=0 WHERE Username='net_guest';")
    replacement = auth(guest.issue())
    exchange(replacement,6)
    check(sql('SELECT COUNT(*) FROM Clans;') == '1' and
          sql('SELECT COUNT(*) FROM ClanMembers;') == '3', 'Directory is read-only')
    return replacement, queries
