"""Real TCP world isolation followed by the existing games in category 20."""
import struct
import time

from tcp_fixture import host_create_payload
from social_network_flow import request, message, barrier
from invitation_network_flow import failure


def verify_channel_worlds(host, guest, check, note, text):
    host.handoff('enter-beginner-world', category=20)
    guest.receive(511)
    guest.receive(501)
    time.sleep(3.1)
    host.send(502, struct.pack('<I', 0) + text('isolated plaza') + text(''))
    host.receive(509)
    barrier(guest, check)  # Same account session stays online, different plaza receives no chat.
    content = '跨频道 private'
    request(host, text, 8, guest.character_id, message=content)
    message(guest, check, host.character_id, 'RoomPeer', 'RoomPeer', content)
    message(host, check, guest.character_id, 'RoomProbe', 'RoomPeer', content)
    host.send(402, host_create_payload())
    host.receive(102)
    beginner_room = struct.unpack_from('<H', host.receive(117))[0]
    guest.send(401, struct.pack('<HH', 0, 10))
    check(guest.receive(101) == bytes(16), 'Free channel room list excludes the beginner room')
    guest.send(403, struct.pack('<I', beginner_room) + text(''))
    check(guest.receive(103) == struct.pack('<II', 5, 0),
          'A room ID in another channel cannot authorize joining it')
    guest.send(402, host_create_payload())
    guest.receive(102)
    free_room = struct.unpack_from('<H', guest.receive(117))[0]
    check(beginner_room == free_room, 'Independent room tables may reuse the same room ID')
    host.send(110, text('RoomProbe') + struct.pack('<I', 1))
    failure(host, 119, 'RoomProbe', 24, check)
    barrier(guest, check)
    host.handoff('waiting-room-to-free', category=60)
    host.send(401, struct.pack('<HH', 0, 10))
    listed = host.receive(101)
    check(struct.unpack_from('<I', listed, 12)[0] == 1 and
          struct.unpack_from('<H', listed, 16)[0] == free_room,
          'Switching adopts the destination room table and preserves its existing room')
    guest.handoff('waiting-room-to-beginner', category=20)
    host.send(401, struct.pack('<HH', 0, 10))
    check(host.receive(101) == bytes(16), 'Committed departure removes only its source room')
    guest.handoff('return-to-free', category=60)
    host.receive(503)
    host.receive(501)
    # Exercise subsequent room creation, public consent, party movement and all
    # five existing ready/load/start/movement/return rounds in the second world.
    host.handoff('games-in-beginner-world', category=20)
    guest.receive(511)
    guest.receive(501)
    guest.handoff('games-in-beginner-world', category=20)
    host.receive(503)
    host.receive(501)
    note('channel-world-isolation-passed', room_ids_overlap=True,
         room_lists_isolated=True, foreign_join_refused=True,
         scene_chat_isolated=True, private_chat_preserved=True,
         foreign_invitation_refused=True, subsequent_game_category=20,
         original_ui=False)
