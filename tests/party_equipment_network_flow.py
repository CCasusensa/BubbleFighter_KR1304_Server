"""Independently decode native S201 equipment and exercise persisted changes."""
import struct

from social_network_flow import Reader, barrier

NAMES = {'host': 'RoomPeer', 'guest': 'RoomProbe', 'observer': 'RoomObserver'}
DEFAULT_ITEMS = [201005, 202006, 205033, 207001, 300027]


def view_bytes(peer):
    items = peer.equipment_items
    return (struct.pack('<III', peer.character_id, 300027, len(items)) +
            struct.pack('<' + 'I' * len(items), *items) + bytes(16))


def inventory_view(peer, payload, check):
    check(payload == view_bytes(peer), 'S806 matches the known persisted equipment and authenticated model')


def party(peer, members, text, check):
    party_payload(peer.receive(201), members, text, check)


def party_payload(payload, members, text, check):
    r = Reader(payload)
    check((r.u32(), r.u32()) == (2, len(members)), 'S201 state and complete member count')
    for member in members:
        check((r.u32(), r.text()) == (member.character_id, NAMES[member.role]), 'Party leader/member identity order')
        check(tuple(r.u32() for _ in range(4)) == (0xffffffff, 0xffffffff, 0, 0), 'Native unknown member baselines retained')
    check(r.u32() == len(members), 'Every initialized party member has one equipment record')
    expected = {member.character_id: member for member in members}
    seen = set()
    for _ in members:
        key, model = r.u32(), r.u32()
        check(key in expected and key not in seen, 'Equipment map contains unique owned identities')
        seen.add(key)
        check(model == 300027, 'Equipment model matches persisted network character')
        n = r.u32()
        check(n == len(expected[key].equipment_items), 'Exact ordinary equipment count')
        check([r.u32() for _ in range(n)] == expected[key].equipment_items, 'Party equipment equals known persisted items')
        check(tuple(r.u32() for _ in range(4)) == (0, 0, 0, 0), 'Unimplemented supplementary groups and details stay empty')
    r.end()


def change(owner, members, text, check, item, equip, scene):
    owner.send(804 if equip else 805, struct.pack('<Q', item))
    owner.receive(802)
    if equip:
        owner.equipment_items = sorted(set(owner.equipment_items) | {item})
    else:
        owner.equipment_items = [x for x in owner.equipment_items if x != item]
    inventory_view(owner, owner.receive(806), check)
    owner.receive(837)
    if scene == 'room':
        owner.receive(503)
    party(owner, members, text, check)
    for peer in members:
        if peer is owner:
            continue
        if scene in ('plaza', 'room'):
            peer.receive(503)
        party(peer, members, text, check)


def verify_equipment_changes(members, check, note, text, scene):
    owner = members[0]
    change(owner, members, text, check, 202006, False, scene)
    # Repeating unchanged equipment must not produce another S201. Existing
    # plaza refresh deliberately republishes S503 even for an identical view.
    owner.send(805, struct.pack('<Q', 202006))
    owner.receive(802)
    inventory_view(owner, owner.receive(806), check)
    owner.receive(837)
    if scene == 'plaza':
        for peer in members:
            if peer is not owner:
                peer.receive(503)
    for peer in members:
        barrier(peer, check)
    change(owner, members, text, check, 202006, True, scene)
    if scene == 'menu':
        for page, expected in [(4, [201005, 207001, 300027]), (0, DEFAULT_ITEMS)]:
            owner.send(832, struct.pack('<IB', 0xffffffff, page))
            owner.receive(802)
            owner.equipment_items = list(expected)
            inventory_view(owner, owner.receive(806), check)
            owner.receive(837)
            owner.receive(836)
            for peer in members:
                party(peer, members, text, check)
        # Explicit refresh uses its established 802/837/806/810 order.
        owner.send(803)
        owner.receive(802)
        owner.receive(837)
        inventory_view(owner, owner.receive(806), check)
        owner.receive(810)
        for peer in members:
            barrier(peer, check)
    note('party-equipment-passed', scene=scene, peers=len(members),
         equipment=True, idempotent=True, weapon_presets=scene == 'menu',
         persistence='isolated MySQL', original_ui=False)
