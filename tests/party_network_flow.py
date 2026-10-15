"""Native administration on authenticated solo parties; no public group admission."""
import struct
import time

from social_network_flow import Reader, barrier


def verify_solo_roster(payload, identity, name, text, check):
    expected = (struct.pack('<III', 2, 1, identity) + text(name) +
                struct.pack('<IIIII', 0xffffffff, 0xffffffff, 0, 0, 0))
    check(payload == expected, 'Login S201 has one trusted identity and native default fields')


def verify_solo_administration(host, guest, check, note, text, scene):
    time.sleep(3.1)
    host.send(202)
    barrier(host, check)
    for name, reason in [('OfflinePeer', 1), ('RoomPeer', 2), ('RoomProbe', 3)]:
        host.send(302, text(name))
        response = Reader(host.receive(302))
        check((response.u32(), response.text()) == (reason, name),
              'Native kick failure matches connection-owned solo membership')
        response.end()
    for name, reason in [('RoomPeer', 2), ('RoomProbe', 1)]:
        host.send(303, text(name))
        response = Reader(host.receive(303))
        check((response.text(), response.text(), response.u32()) ==
              (name, 'RoomPeer', reason), 'Native leader response retains target/leader order')
        response.end()
    barrier(guest, check)
    note('party-administration-passed', scene=scene, solo=True,
         public_party_admission=False, collective_scene_transfer=False)
    # Later invitation/chat phases use the same quota.
    time.sleep(3.1)
