"""Private groups over authenticated TCP, followed through handoff and removal."""
import struct
import time

from social_network_flow import Reader, barrier, roster, throttle


def decode_groups(payload, check, status=0, action=0):
    r = Reader(payload)
    check((r.u32(), r.u32(), r.u8()) == (1, status, action), 'Exact group version/status/action')
    count = r.u32()
    check(count <= 32, 'Bounded group count')
    groups = {}
    for _ in range(count):
        identity, name = r.u32(), r.text()
        check(identity != 0 and identity not in groups and 1 <= len(name.encode('utf-16-le'))//2 <= 31 and name not in groups.values(),
              'Unique personal group identity and UTF16 name')
        groups[identity] = name
    count = r.u32()
    check(count <= 200, 'Bounded assignment count')
    assignments = {}
    for _ in range(count):
        peer, group = r.u32(), r.u32()
        check(peer != 0 and peer not in assignments and group in groups, 'Unique known group assignment')
        assignments[peer] = group
    r.end()
    return groups, assignments


def perform(peer, peers, check, text, action=0, group=0, friend=0, source=0, name='', status=0):
    throttle(peer)
    for other in peers:
        if other is not peer:
            barrier(other, check)  # Drain preceding automatic snapshots before this operation.
    peer.group_companion = True
    peer.send(65012, struct.pack('<IBIII', 1, action, group, friend, source)+text(name))
    roster(peer, check)
    snapshot = decode_groups(peer.receive(65013), check, status, action)
    peer.group_state = snapshot
    peer.group_actions += 1
    for other in peers:
        if other is not peer:
            before = other.group_updates
            barrier(other, check)
            check(other.group_updates == before, 'Personal group mutation sends no other owner notification')
    return snapshot


def verify_friend_groups(host, guest, check, note, text):
    time.sleep(3.1)
    peers = (host, guest)
    call = lambda peer, **kw: perform(peer, peers, check, text, **kw)
    for peer in peers:
        check(call(peer) == ({}, {}), 'Fresh generated personal groups')
    groups, _ = call(host, action=1, name='战友')
    a = next(iter(groups))
    groups, _ = call(host, action=1, name='Raid')
    b = next(identity for identity in groups if identity != a)
    groups, _ = call(guest, action=1, name='战友')
    private = next(iter(groups))
    check(private not in (a, b), 'Owner groups use distinct durable IDs')
    call(host, action=2, group=private, name='stolen', status=1)
    call(host, action=1, name='战友', status=2)
    groups, assigned = call(host, action=4, group=a, friend=guest.character_id)
    check(assigned == {guest.character_id:a}, 'Assign accepted friend to personal group')
    call(host, action=4, group=b, friend=guest.character_id, status=6)
    groups, assigned = call(host, action=4, group=b, friend=guest.character_id, source=a)
    check(assigned == {guest.character_id:b}, 'Conditional group move commits')
    call(host, action=3, group=b, status=5)
    groups, assigned = call(host, action=4, friend=guest.character_id, source=b)
    check(not assigned, 'Ungroup preserves accepted friendship')
    call(host, action=3, group=b)
    call(host, action=2, group=a, name='MyTeam😀')
    call(host, action=4, group=a, friend=guest.character_id)
    call(guest, action=4, group=private, friend=host.character_id)
    host.expected_groups = ({a:'MyTeam😀'}, {guest.character_id:a})
    guest.expected_groups = ({private:'战友'}, {host.character_id:private})
    check(host.group_state == host.expected_groups and guest.group_state == guest.expected_groups,
          'Grouping is independent for the two friendship endpoints')
    note('friend-groups-crud-passed', owner_scoped=True, conditional_move=True,
         populated_delete_refused=True, unicode=True, original_ui=False)


def verify_group_handoff(host, guest, check, note, text):
    for peer in (host, guest):
        check(perform(peer, (host, guest), check, text) == peer.expected_groups,
              'Channel handoff preserves private groups and explicit subscription')
    note('friend-groups-handoff-passed', persistent=True, owner_scoped=True)


def verify_group_removal(host, guest, observer, check, note, text):
    time.sleep(3.1)
    for peer in (host, guest):
        groups, assigned = perform(peer, (host, guest), check, text)
        check(groups == peer.expected_groups[0] and not assigned, 'Friend removal clears assignments but retains groups')
    barrier(observer, check)
    check(observer.group_updates == 0 and not observer.group_companion, 'Legacy companion never receives group extension')
    note('friend-groups-removal-passed', both_assignments_revoked=True, groups_retained=True, legacy_unchanged=True)
