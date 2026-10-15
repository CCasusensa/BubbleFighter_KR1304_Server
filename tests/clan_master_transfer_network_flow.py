"""Native three-phase master consent across trusted game and clan sockets."""
import struct


def exercise(host, guest, observer, active, guest_clan, observer_clan,
             auth, sql, check, z):
    actions = 0
    games = [host, guest, observer]
    peers = [active, guest_clan, observer_clan]
    names = ['RoomPeer', 'RoomProbe', 'RoomObserver']
    roles = [101, 100, 1]

    def send(who, mid, body=b''):
        nonlocal actions
        actions += 1
        peers[who].send(mid, body, split=True)

    def profile(who):
        expected = struct.pack('<BII', int(roles[who] >= 100), 9000, roles[who]) + z('NetClan') + struct.pack('<d', 73051.0)
        check(peers[who].receive(1016) == expected, 'Transfer preserves trusted current profile')

    def refusal(who=0, target=1, supplied_name=None):
        send(who, 1054, struct.pack('<I', games[target].character) + z(names[target] if supplied_name is None else supplied_name))
        check(peers[who].receive(1058) == bytes(5), 'Unconsented/stale commit fails')
        profile(who)

    def offer(who=0, target=1, success=True):
        send(who, 1057, struct.pack('<I', games[target].character))
        check(peers[who].receive(1058) == struct.pack('<BI', int(success), 0), 'Offer availability uses native byte and opaque DWORD')
        if not success:
            return None
        body = peers[target].receive(1059)
        check(body[:-4] == z(names[who]), 'Offer includes trusted requester name')
        key, = struct.unpack('<I', body[-4:])
        check(key != 0, 'Project one-use correlation is nonzero')
        return key

    def answer(key, accepted=1, who=1, requester=0, success=True):
        send(who, 1060, struct.pack('<BI', accepted, key))
        check(peers[who].receive(1061) == bytes([int(success)]), 'Recipient answer acknowledgement')
        if success:
            expected = struct.pack('<BI', accepted, games[who].character) + z(names[who])
            check(peers[requester].receive(1062) == expected, 'Requester sees recipient decision and trusted target')

    def committed(who, target):
        send(who, 1054, struct.pack('<I', games[target].character) + z(names[target]))
        notice = (struct.pack('<II', 0, games[who].character) + z(names[who]) +
                  struct.pack('<I', games[target].character) + z(names[target]))
        roles[who], roles[target] = 100, 101
        details = (z('NetClan') + struct.pack('<I', 0) + z(names[target]) + struct.pack('<I', 0) + z('') +
                   struct.pack('<IIdQIII', 1, 0, 45292.0, 0, 0, 2, 0) + z('') + z('Hi') + struct.pack('<II', 0, 30))
        for participant in (who, target):
            check(peers[participant].receive(1056) == notice, 'Both participants receive the native committed master transition')
            profile(participant)
            check(peers[participant].receive(1018) == details, 'Master name and count reflect the same committed snapshot')
        check(sql('SELECT GROUP_CONCAT(MembershipStatus ORDER BY CharacterId) FROM ClanMembers WHERE ClanId=9000;') == ','.join(map(str, roles)),
              'Atomic roles persisted before success')
        send(2, 1015); profile(2)  # Pending applicants must not receive S1056.

    def menu(who):
        games[who].send(105)
        for mid in (105, 117, 806, 837, 802, 810):
            games[who].receive(mid)

    def enter(who, existing=None):
        games[who].send(106); games[who].receive(117)
        if existing is not None:
            games[existing].receive(503); games[existing].receive(501)

    refusal()
    offer(success=False)  # Neither trusted game session has entered the plaza.
    for game in (host, guest):
        game.send(521)
        for mid in (105, 117, 806, 837, 802, 810, 170, 201): game.receive(mid)
    host.initialized = True
    enter(0)
    offer(success=False)  # Online menu presence alone is insufficient.
    enter(1, 0)
    offer(who=1, target=0, success=False)
    offer(target=2, success=False)  # Applicant/menu target cannot receive an offer.
    key = offer()
    offer(success=False)  # Duplicate cannot replace consent.
    answer(key, who=2, success=False)
    answer(key, accepted=255, success=False)
    answer(key, accepted=0)
    refusal()
    answer(key, success=False)
    next_key = offer(); check(next_key > key, 'Rejected correlation is not reused')
    answer(next_key)
    answer(next_key, success=False)
    refusal(supplied_name='Forged')
    refusal()  # Even a malformed commit consumes its grant.
    key = offer(); answer(key)
    menu(1); host.receive(511); host.receive(501)
    refusal()
    enter(1, 0)
    refusal()  # Returning to the plaza cannot revive an old acceptance.
    key = offer(); answer(key)
    replacement = auth(guest.issue()); guest_clan.closed()
    peers[1] = replacement; guest_clan = replacement
    refusal()
    answer(key, success=False)
    key = offer(); answer(key)
    committed(0, 1)
    refusal()  # Old master's replay does not change the new master.
    offer(success=False)
    key = offer(who=1, target=0)
    answer(key, who=0, requester=1)
    committed(1, 0)
    # Preserve the caller's original fixture and main scene before handoff tests.
    menu(1); host.receive(511); host.receive(501)
    menu(0)
    return guest_clan, actions
