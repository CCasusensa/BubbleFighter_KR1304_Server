"""Native invitation messages over authenticated synthetic game TCP peers."""
import struct
import time
from social_network_flow import Reader, roster, barrier
from tcp_fixture import host_start_payload
from channel_network_flow import verify_channel_refusals


def failure(peer, mid, name, reason, check):
    r = Reader(peer.receive(mid))
    check((r.text(), r.u32()) == (name, reason), 'Invitation native failure identity/reason')
    r.end()


def ask(sender, recipient, sender_name, recipient_name, check, note, text):
    sender.send(110, text(recipient_name) + struct.pack('<I', 1))
    check(sender.receive(120) == text(recipient_name), 'Invitation delivery names the requested recipient')
    payload = recipient.receive(121)
    r = Reader(payload)
    check(r.text() == sender_name, 'Invitation uses authenticated sender name')
    key, address = r.u32(), r.u32()
    port = struct.unpack('<H', r.take(2))[0]
    check(key != 0 and (address, port, r.u32(), r.u32()) == (0, 0, 0, 0), 'Bounded invitation key and opaque zero baseline')
    r.end()
    note('native-invitation', sender=sender.role, recipient=recipient.role, message_id=121, payload_hex=payload.hex())
    return key, text(sender_name) + struct.pack('<IHB', address, port, 1)


def verify_invitation_entry(host, guest, check, note, text):
    time.sleep(3.1)
    guest.send(115, text('RoomPeer') + bytes(6) + b'\x01')
    failure(guest, 122, 'RoomPeer', 21, check)
    key, accept = ask(host, guest, 'RoomPeer', 'RoomProbe', check, note, text)
    host.send(110, text('RoomProbe') + struct.pack('<I', 1))
    failure(host, 119, 'RoomProbe', 20, check)
    guest.send(111, struct.pack('<I', key))
    failure(host, 119, 'RoomProbe', 26, check)
    guest.send(111, struct.pack('<I', key))
    barrier(host, check)
    # Existing persistent block cache must also gate the original invitation path.
    guest.send(147, b'\xba' + text('RoomPeer'))
    guest.receive(171)
    roster(host, check)
    roster(guest, check)
    host.send(110, text('RoomProbe') + struct.pack('<I', 1))
    failure(host, 119, 'RoomProbe', 26, check)
    barrier(guest, check)
    guest.send(148, b'\xba' + text('RoomPeer'))
    guest.receive(172)
    roster(host, check)
    roster(guest, check)
    time.sleep(3.1)
    _, accept = ask(host, guest, 'RoomPeer', 'RoomProbe', check, note, text)
    guest.send(115, text('RoomPeer') + struct.pack('<IHB', 1, 0, 1))
    failure(guest, 122, 'RoomPeer', 21, check)
    guest.send(115, accept)
    failure(guest, 122, 'RoomPeer', 21, check)
    _, accept = ask(host, guest, 'RoomPeer', 'RoomProbe', check, note, text)
    guest.send(115, accept)
    # C115 succeeds directly with S117; C403's S103 response must not appear.
    guest.receive(117)
    host.receive(503)
    note('invitation-entry-passed', declined=True, block_checked=True, invalid_echo_consumed=True, original_ui=False)


def verify_invitation_guest(guest, observer, host, check, note, text):
    _, accept = ask(guest, observer, 'RoomProbe', 'RoomObserver', check, note, text)
    observer.send(115, accept)
    observer.receive(117)
    host.receive(503)
    guest.receive(503)
    note('nonhost-invitation-passed', friendship_required=False)


def verify_invited_game(peers, check, note, text, leave_guest=True):
    host, guest, observer = peers
    time.sleep(3.1)
    verify_channel_refusals(peers, check, note, 'waiting-room')
    for peer in peers:
        peer.handoff('waiting-room')
    for member in (guest, observer):
        member.send(505, struct.pack('<III', 1, 0, 0))
        for peer in peers:
            peer.receive(503)
    host.send(503, host_start_payload())
    for peer in peers:
        check(peer.receive(585) == bytes(12), 'Explicit shared game transport options precede loading')
    loads = [peer.receive(510) for peer in peers]
    check(loads[0] == loads[1] == loads[2], 'Invited members share the same map load')
    verify_channel_refusals(peers, check, note, 'loading')
    for peer in peers:
        peer.handoff('loading')
    for peer in peers:
        peer.send(578, bytes(16))
        check(peer.receive(606) == bytes(4), 'Each invited member completes the load barrier')
    countdowns = [peer.receive(607) for peer in peers]
    check(countdowns[0] == countdowns[1] == countdowns[2], 'Shared countdown after all three loads')
    starts = [peer.receive(608) for peer in peers]
    check(starts[0] == starts[1] == starts[2], 'Shared start seed and duration')
    duration, seed = struct.unpack('<II', starts[0])
    check(duration == 240000 and 0 < seed < 0x7fffffff, 'Bounded configured match start')
    verify_channel_refusals(peers, check, note, 'playing')
    queued_move = struct.pack('<IfffBBBH', 1100000, 12.0, 23.0, 34.0, 1, 2, 3, 4)

    def queue_during_switch():
        guest.send(0x8075, queued_move)
        check(observer.receive(0x8075) == queued_move, 'Other actor sees movement while host is switching')
        barrier(guest, check)

    host.handoff('playing', queue_during_switch)
    check(host.receive(0x8075) == queued_move, 'Paused source queue is delivered by the committed destination')
    note('handoff-queued-movement-passed', original_ui=False)
    for peer in (guest, observer):
        peer.handoff('playing')
    for slot, sender in enumerate(peers):
        payload = struct.pack('<IfffBBBH', 1000000 + slot*100000, 10.0+slot, 20.0, 30.0, 1, 2, 3, 4)
        sender.send(0x8075, payload)
        for peer in peers:
            if peer is not sender:
                check(peer.receive(0x8075) == payload, 'Movement reaches both other invited actors')
        barrier(sender, check)
    host.send(110, text('Offline') + struct.pack('<I', 1))
    failure(host, 119, 'Offline', 8, check)
    host.send(624, struct.pack('<II', 1, 0))
    for peer in peers:
        check(peer.receive(620) == b'', 'Explicit game exit returns all peers')
        peer.receive(117)
    verify_channel_refusals(peers, check, note, 'returned-room')
    if leave_guest:
        guest.send(404)
        guest.receive(117)
        host.receive(511)
        observer.receive(511)
        guest.send(115, text('RoomPeer') + bytes(6) + b'\x01')
        failure(guest, 122, 'RoomPeer', 21, check)
    note('invited-game-passed', peers=3, loads=True, countdown=True, shared_start=True, movement=True, explicit_return=True, unsolicited_reentry_refused=leave_guest, natural_result=False)
