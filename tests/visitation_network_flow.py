"""Original visitation packets through three authenticated production TCP peers."""
import struct
import time

from social_network_flow import Reader, roster, barrier
from invitation_network_flow import failure


def ask(visitor, owner, owner_name, check, note, text):
    visitor.send(112, text(owner_name))
    payload = owner.receive(124)
    r = Reader(payload)
    key = r.u32()
    check(key != 0 and r.text() == 'RoomProbe' and r.u32() == 0,
          'Visitation identifies the authenticated visitor and asks for consent')
    r.end()
    barrier(visitor, check)
    note('native-visitation', visitor=visitor.role, owner=owner.role,
         message_id=124, payload_hex=payload.hex())
    return key


def approve(visitor, owner, key, owner_name, check, note, text):
    owner.send(114, struct.pack('<I', key))
    payload = visitor.receive(125)
    r = Reader(payload)
    check(r.text() == owner_name and r.u32() == 0 and r.take(2) == bytes(2),
          'Visitation approval names the authenticated room member')
    r.end()
    note('native-visitation-approved', message_id=125, payload_hex=payload.hex())
    return text(owner_name) + bytes(7)


def verify_visitation_entry(peers, check, note, text):
    host, visitor, observer = peers
    time.sleep(3.1)
    key = ask(visitor, host, 'RoomPeer', check, note, text)
    visitor.send(115, text('RoomPeer') + bytes(7))
    failure(visitor, 122, 'RoomPeer', 21, check)
    # Neither the visitor nor a third connection may approve this application.
    visitor.send(114, struct.pack('<I', key))
    barrier(visitor, check)
    observer.send(114, struct.pack('<I', key))
    barrier(observer, check)
    barrier(visitor, check)
    host.send(113, struct.pack('<II', key, 1))
    failure(visitor, 123, 'RoomPeer', 28, check)
    host.send(114, struct.pack('<I', key))
    barrier(host, check)
    barrier(visitor, check)

    time.sleep(3.1)
    key = ask(visitor, host, 'RoomPeer', check, note, text)
    move = approve(visitor, host, key, 'RoomPeer', check, note, text)
    host.send(114, struct.pack('<I', key))
    host.send(113, struct.pack('<II', key, 0))
    barrier(host, check)
    barrier(visitor, check)
    # Byte 1 belongs to invitations; a wrong echo consumes this approved visit.
    visitor.send(115, move[:-1] + b'\x01')
    failure(visitor, 122, 'RoomPeer', 21, check)
    visitor.send(115, move)
    failure(visitor, 122, 'RoomPeer', 21, check)

    time.sleep(3.1)
    key = ask(visitor, host, 'RoomPeer', check, note, text)
    visitor.send(147, b'\xba' + text('RoomPeer'))
    visitor.receive(171)
    roster(host, check)
    roster(visitor, check)
    host.send(114, struct.pack('<I', key))
    failure(visitor, 123, 'RoomPeer', 28, check)
    visitor.send(112, text('RoomPeer'))
    failure(visitor, 123, 'RoomPeer', 28, check)
    barrier(host, check)
    visitor.send(148, b'\xba' + text('RoomPeer'))
    visitor.receive(172)
    roster(host, check)
    roster(visitor, check)

    time.sleep(3.1)
    # A nonhost room member can consent, without a friendship requirement.
    key = ask(visitor, observer, 'RoomObserver', check, note, text)
    visitor.send(111, struct.pack('<I', key))
    barrier(visitor, check)
    observer.send(113, struct.pack('<II', key, 255))
    barrier(observer, check)
    barrier(visitor, check)
    move = approve(visitor, observer, key, 'RoomObserver', check, note, text)
    visitor.send(115, move)
    visitor.receive(117)
    host.receive(503)
    observer.receive(503)
    visitor.send(112, text('RoomObserver'))
    failure(visitor, 123, 'RoomObserver', 2, check)
    note('visitation-entry-passed', unapproved_move_refused=True,
         forged_approval_refused=True, declined=True, wrong_kind_consumed=True,
         block_rechecked=True, nonhost_consent=True, friendship_required=False,
         unknown_decline_preserved=True, original_ui=False)
