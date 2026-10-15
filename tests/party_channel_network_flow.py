"""Collective channel handoff, then real three-peer room/game continuation."""
import struct
import time

from invitation_network_flow import verify_invited_game
from social_network_flow import barrier
from tcp_fixture import host_create_payload


def move_party(peers, category, check, note, phase):
    peers[0].send(53, struct.pack('<I', category))
    pending = []
    try:
        for peer in peers:
            destination, proof = peer.prepare_handoff(category)
            pending.append((peer, destination, proof))
        for _, destination, _ in pending:
            check(destination.receive(9) == b'', 'Whole-party native entry barrier released')
        for _, destination, _ in pending:
            destination.send(55)
        for peer, destination, proof in pending:
            peer.finish_handoff(phase, category, destination, proof)
        note('party-channel-move-passed', members=len(peers), category=category,
             phase=phase, one_leader_request=True, individual_proofs=True,
             whole_party=True, original_ui=False)
    finally:
        for _, destination, _ in pending:
            destination.socket.close()


def verify_party_channels(host, leader, member, check, note, text):
    peers = [leader, member, host]
    member.send(53, struct.pack('<I', 60))
    check(member.receive(8) == struct.pack('<I4sHI', 2, bytes(4), 0, 60),
          'A follower cannot initiate a whole-party channel move')
    leader.send(53, struct.pack('<I', 70))
    check(leader.receive(8) == struct.pack('<I4sHI', 2, bytes(4), 0, 70),
          'One ineligible member prevents every destination offer')
    for peer in peers:
        barrier(peer, check)
    move_party([leader, member], 60, check, note, 'party-plaza-to-free')
    for _ in range(2):
        host.receive(511)
        host.receive(501)
    host.handoff('join-migrated-party-world', category=60)
    for peer in (leader, member):
        peer.receive(503)
        peer.receive(501)
    for round_index in range(2):
        time.sleep(3.1)
        leader.send(402, host_create_payload())
        leader.receive(102)
        room = struct.unpack_from('<H', leader.receive(117))[0]
        check(struct.unpack_from('<H', member.receive(117))[0] == room,
              'Migrated party creates one shared room')
        for _ in range(2):
            host.receive(511)
            host.receive(501)
        host.send(403, struct.pack('<I', room) + text(''))
        host.receive(103)
        host.receive(117)
        leader.receive(503)
        member.receive(503)
        verify_invited_game(peers, check, note, text, leave_guest=False)
        if round_index == 0:
            # The pair owns the room. Migrating out must preserve the outsider
            # and make that one remaining player the room host exactly once.
            move_party([leader, member], 20, check, note, 'party-waiting-to-beginner')
            host.receive(511)
            host.receive(511)
            check(host.receive(502) == struct.pack('<I', 2),
                  'Collective channel departure transfers the host directly to the outsider')
            host.send(404)
            host.receive(117)
            host.handoff('rejoin-migrated-party-world', category=20)
            for peer in (leader, member):
                peer.receive(503)
                peer.receive(501)
    for peer in peers:
        barrier(peer, check)
    note('party-channel-games-passed', rounds=2, categories=[60, 20],
         loads=True, shared_start=True, movement=True, explicit_return=True,
         waiting_room_departure=True, outsider_preserved=True, original_ui=False)
