"""Atomic native C1027/S1028 -> main C806/S813/S802 flow on real TCP."""
import struct
import zlib


def verify_inventory(body, check, wanted=1):
    check(struct.unpack_from('<I', body)[0] == len(body) - 4, 'S802 compressed length')
    data = zlib.decompress(body[4:])
    position = 0

    def take(fmt):
        nonlocal position
        value = struct.unpack_from(fmt, data, position)
        position += struct.calcsize(fmt)
        return value

    def text():
        nonlocal position
        size, = take('<I')
        position += size * 2
        assert position <= len(data)

    found = []
    count, = take('<I')
    for _ in range(count):
        item, quantity, bits = take('<IIQ')
        text()
        attributes, = take('<I')
        for _ in range(attributes):
            _, _, values = take('<III')
            for _ in range(values): take('<I')
            take('<I'); text()
        flags, = take('<B')
        if item == 100819: found.append((quantity, bits, flags))
    check(position == len(data), 'S802 exact inventory boundary')
    check(found == [(wanted, 0, 2 if wanted == 0 else 0)], 'S802 current owned clan card quantity')


def confirm(game, sql, check):
    before = sql(f'SELECT Quantity,Revision FROM InventorySlots WHERE CharacterId={game.character} AND ItemId=100819;')
    game.send(806, struct.pack('<III', 100819, 1, 0))
    check(game.receive(813) == struct.pack('<III', 0, 100819, 0), 'Native generic S813 confirmation')
    verify_inventory(game.receive(802), check)
    check(sql(f'SELECT Quantity,Revision FROM InventorySlots WHERE CharacterId={game.character} AND ItemId=100819;') == before == '1\t2', 'Confirmation never debits or advances revision')


def exercise(host, guest, active, guest_clan, sql, check, z, profile):
    def create(peer, name, item=100819, parameter=30):
        peer.send(1027, z(name) + struct.pack('<I', 0) + z('Hello world') + struct.pack('<II', item, parameter))
        return peer.receive(1028)

    check(create(active, 'RealClan', parameter=31) == bytes(12), 'Catalog parameter is server validated')
    check(create(active, 'RealClan') == struct.pack('<III', 1, 100819, 0), 'Native successful S1028 item ID')
    clan_id = int(sql(f'SELECT ClanId FROM ClanMembers WHERE CharacterId={host.character};'))
    check(clan_id >= 1000 and clan_id != 99, 'Allocated persistent clan identifier')
    host.expect_clan(clan_id)
    profile(active, clan_id, 101, 1)
    active.send(1017)
    details = active.receive(1018)
    check(details.startswith(z('RealClan')) and details.endswith(struct.pack('<II', 0, 30)), 'Created profile and catalog capacity')
    check(sql(f'SELECT ItemId,InventoryRevision FROM ClanCreationReceipts WHERE CharacterId={host.character};') == '100819\t2', 'Committed receipt identifies card revision')
    confirm(host, sql, check)
    confirm(host, sql, check)
    check(create(active, 'OtherClan') == struct.pack('<III', 0xffffff97, 0, 0), 'Already-member C1027 retry is not a second charge')
    check(create(guest_clan, 'RealClan') == struct.pack('<III', 2, 0, 0), 'Concurrent-capable unique clan name refusal')
    check(sql(f'SELECT Quantity,Revision FROM InventorySlots WHERE CharacterId={guest.character} AND ItemId=100819;') == '2\t1', 'Duplicate name preserves applicant inventory')
    sql(f'DELETE FROM Clans WHERE ClanId={clan_id};')
    host.expect_clan(0, timeout=12)
    profile(active)
    confirm(host, sql, check)
    check(sql('SELECT COUNT(*) FROM Clans;') == '0', 'Creation scenario cleaned before profile fixture')
