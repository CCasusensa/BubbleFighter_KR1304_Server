"""Public native party consent over three authenticated synthetic TCP peers."""
import struct
import time

from invitation_network_flow import ask, failure, verify_invited_game
from social_network_flow import Reader, barrier
from tcp_fixture import host_create_payload
from party_equipment_network_flow import party, party_payload, verify_equipment_changes
from channel_network_flow import verify_channel_refusals


NAMES = {'host': 'RoomPeer', 'guest': 'RoomProbe', 'observer': 'RoomObserver'}


def joined(peers, newcomer, text, check):
    for peer in peers:
        party(peer, peers, text, check)
        check(peer.receive(206) == text(NAMES[newcomer.role]), 'Joined notice follows complete party roster')


def verify_party_consent(peers, check, note, text):
    host, guest, observer = peers
    time.sleep(3.1)
    # Prior room game leaves guest in the plaza, host/observer in the room.
    observer.send(404)
    observer.receive(117)
    host.receive(511)
    guest.receive(503)
    guest.receive(501)
    observer.send(105)
    for mid in (105, 117, 806, 837, 802, 810):
        observer.receive(mid)
    guest.receive(511)
    guest.receive(501)
    host.send(404)
    host.receive(117)
    guest.receive(503)
    guest.receive(501)
    host.send(105)
    for mid in (105, 117, 806, 837, 802, 810):
        host.receive(mid)
    guest.receive(511)
    guest.receive(501)
    guest.send(105)
    for mid in (105, 117, 806, 837, 802, 810):
        guest.receive(mid)
    _, move = ask(host, guest, 'RoomPeer', 'RoomProbe', check, note, text)
    guest.send(115, move)
    guest.receive(117)
    joined([host, guest], guest, text, check)
    guest.send(115, move)
    failure(guest, 122, 'RoomPeer', 21, check)

    observer.send(112, text('RoomProbe'))
    r = Reader(guest.receive(124))
    key = r.u32()
    check(key > 0 and r.text() == 'RoomObserver' and r.u32() == 0,
          'Visit asks a nonleader party member for explicit consent')
    r.end()
    observer.send(115, text('RoomProbe') + bytes(7))
    failure(observer, 122, 'RoomProbe', 21, check)
    guest.send(114, struct.pack('<I', key))
    check(observer.receive(125) == text('RoomProbe') + bytes(6), 'Approved visit uses native address baseline')
    observer.send(115, text('RoomProbe') + bytes(7))
    observer.receive(117)
    joined(peers, observer, text, check)

    verify_channel_refusals(peers, check, note, 'party-menu')
    verify_equipment_changes(peers, check, note, text, 'menu')
    guest.send(106)
    guest.receive(509)
    for peer in peers:
        barrier(peer, check)

    time.sleep(3.1)
    host.send(303, text('RoomProbe'))
    for peer in peers:
        party(peer, [guest, host, observer], text, check)
        check(peer.receive(303) == text('RoomProbe') + text('RoomPeer') + bytes(4), 'Leader change reaches all consented party members')
    guest.send(302, text('RoomObserver'))
    party(guest, [guest, host], text, check)
    check(guest.receive(302) == bytes(4) + text('RoomObserver'), 'Leader kick succeeds')
    party(host, [guest, host], text, check)
    check(host.receive(307) == text('RoomProbe') + text('RoomObserver'), 'Remaining peer observes kick')
    party(observer, [observer], text, check)
    check(observer.receive(304) == text('RoomProbe'), 'Kicked connection receives solo roster and authenticated kicker')
    guest.send(202)
    party(guest, [guest], text, check)
    check(guest.receive(204) == b'', 'Explicit party leave succeeds')
    party(host, [host], text, check)
    check(host.receive(203) == text('RoomProbe') + text('RoomPeer'), 'Old leader leaves and successor is announced')

    # Replace an existing plaza lease when accepting an invitation in that plaza.
    host.send(106)
    host.receive(117)
    guest.send(106)
    guest.receive(117)
    host.receive(503)
    host.receive(501)
    time.sleep(3.1)
    _, move = ask(host, guest, 'RoomPeer', 'RoomProbe', check, note, text)
    guest.send(115, move)
    guest.receive(117)
    # Scene and party notifications are separate queues. Poll can drain an
    # empty plaza queue just before the other connection publishes admission,
    # then drain the new party queue first. Require every exact message once
    # and preserve both queue orders without inventing a global send order.
    scene_messages, party_messages = [511, 503, 501], [201, 206]
    while scene_messages or party_messages:
        choices = tuple(chain[0] for chain in (scene_messages, party_messages) if chain)
        mid, payload = host.receive(choices)
        if scene_messages and mid == scene_messages[0]:
            scene_messages.pop(0)
        else:
            party_messages.pop(0)
            if mid == 201:
                party_payload(payload, [host, guest], text, check)
            else:
                check(payload == text('RoomProbe'), 'Joined notice follows complete party roster')
    party(guest, [host, guest], text, check)
    check(guest.receive(206) == text('RoomProbe'), 'Joined notice follows complete party roster')
    verify_equipment_changes([host, guest], check, note, text, 'plaza')
    _, old_move = ask(host, observer, 'RoomPeer', 'RoomObserver', check, note, text)
    host.send(105)
    for peer in (host, guest):
        for mid in (105, 117, 806, 837, 802, 810):
            peer.receive(mid)
    observer.send(115, old_move)
    failure(observer, 122, 'RoomPeer', 21, check)
    _, move = ask(host, observer, 'RoomPeer', 'RoomObserver', check, note, text)
    observer.send(115, move)
    observer.receive(117)
    joined(peers, observer, text, check)
    for peer in peers:
        barrier(peer, check)
    note('public-party-consent-passed', invitation=True, approved_visit=True,
         menu=True, plaza=True, scene_change_revokes_consent=True,
         leader_transfer=True, kick=True, leave=True, original_ui=False,
         collective_scene_transfer=False)

    # Only the leader requests the move and creation. Every follower is prepared
    # by its own server session and receives its complete destination snapshot.
    time.sleep(3.1)
    host.send(106)
    for peer in peers:
        peer.receive(117)
    host.send(402, host_create_payload())
    host.receive(102)
    snapshots = [peer.receive(117) for peer in peers]
    room_ids = [struct.unpack_from('<H', payload)[0] for payload in snapshots]
    check(len(set(room_ids)) == 1 and room_ids[0] > 0, 'Whole party creates and enters exactly one room')
    for peer in peers:
        barrier(peer, check)
    verify_equipment_changes(peers, check, note, text, 'room')
    verify_invited_game(peers, check, note, text, leave_guest=False)
    note('consented-party-game-passed', public_party_admission=True,
         individual_room_consent=False, loads=True, shared_start=True,
         movement=True, explicit_return=True, collective_scene_transfer=True,
         original_ui=False)
    # The former party leader becomes a solo room owner. The remaining pair
    # joins that existing room with only its new leader's C403 request.
    guest.send(404)
    guest.receive(509)
    for peer in peers:
        barrier(peer, check)
    host.send(404)
    for peer in peers:
        peer.receive(117)
        barrier(peer, check)
    host.send(202)
    party(host, [host], text, check)
    host.receive(204)
    for peer in (guest, observer):
        party(peer, [guest, observer], text, check)
        peer.receive(203)
    host.send(402, host_create_payload())
    host.receive(102)
    room = struct.unpack_from('<H', host.receive(117))[0]
    for peer in (guest, observer):
        peer.receive(511)
        peer.receive(501)
    guest.send(403, struct.pack('<I', room) + text(''))
    guest.receive(103)
    for peer in (guest, observer):
        check(struct.unpack_from('<H', peer.receive(117))[0] == room, 'Whole party joins the leader-selected existing room')
    host.receive(503)
    host.receive(503)
    verify_invited_game(peers, check, note, text, leave_guest=False)
    note('party-scene-transfer-passed', menu=True, plaza=True, create_room=True,
         join_room=True, authenticated_peers=3, shared_start=True, movement=True,
         explicit_return=True, original_ui=False)
    # A visiting party departs together while the solo room owner remains.
    guest.send(404)
    for peer in (guest, observer):
        peer.receive(117)
    for _ in range(2):
        host.receive(511)
    host.send(404)
    host.receive(117)
    for peer in (guest, observer):
        peer.receive(503)
        peer.receive(501)
    # The same pair creates again, admits the outsider, and starts another game.
    guest.send(402, host_create_payload())
    guest.receive(102)
    room = struct.unpack_from('<H', guest.receive(117))[0]
    check(struct.unpack_from('<H', observer.receive(117))[0] == room,
          'Party recreates one room after collective departure')
    for _ in range(2):
        host.receive(511)
        host.receive(501)
    host.send(403, struct.pack('<I', room) + text(''))
    host.receive(103)
    host.receive(117)
    guest.receive(503)
    observer.receive(503)
    verify_invited_game([guest, observer, host], check, note, text, leave_guest=False)
    guest.send(404)
    guest.receive(117)
    observer.receive(117)
    host.receive(511)
    host.receive(511)
    check(host.receive(502) == struct.pack('<I', 2),
          'Departing host party transfers directly to the one remaining outsider')
    host.send(404)
    host.receive(117)
    for peer in (guest, observer):
        peer.receive(503)
        peer.receive(501)
    for peer in peers:
        barrier(peer, check)
    note('party-room-exit-passed', authenticated_peers=3, nonleader_refused=True,
         collective_exit=True, outsider_preserved=True, final_host_transfer=True,
         recreate=True, repeated_shared_start=True, explicit_return=True,
         original_ui=False)
