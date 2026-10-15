"""Three authenticated synthetic TCP peers; no original client UI or external server."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import queue
import socket
import struct
import subprocess
import threading
import time

from tcp_fixture import ROOT, Cipher, frame, xor, text, verify_wire, host_create_payload
from social_network_flow import verify_plaza_social, verify_room_social, roster, barrier, request
from invitation_network_flow import verify_invitation_entry, verify_invitation_guest, verify_invited_game
from visitation_network_flow import verify_visitation_entry
from party_network_flow import verify_solo_administration, verify_solo_roster
from party_consent_network_flow import verify_party_consent
from party_equipment_network_flow import DEFAULT_ITEMS, inventory_view, party as equipped_party
from channel_network_flow import verify_channel_refusals
from channel_worlds_network_flow import verify_channel_worlds
from friend_group_network_flow import decode_groups, verify_friend_groups, verify_group_handoff, verify_group_removal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--database-config', type=Path, required=True)
    parser.add_argument('--tickets', type=Path, nargs=3)
    parser.add_argument('--expect-missing-schema', action='store_true')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.expect_missing_schema:
        result = subprocess.run([str(args.executable), '--listen-port', '0', '--once',
                                 '--database-config', str(args.database_config)],
                                capture_output=True, timeout=10,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        passed = (result.returncode == 1 and b'1146' in result.stderr and
                  b'Listening on' not in result.stdout)
        args.report.write_text(json.dumps(dict(passed=passed, checks=1,
                                              server_exit=result.returncode,
                                              expected_mysql_code=1146,
                                              listened=b'Listening on' in result.stdout)) + '\n', encoding='utf-8')
        if not passed:
            raise RuntimeError('Server did not reject missing social schema before listening')
        print('PASS: missing social migration rejected before listening.')
        return
    if args.tickets is None:
        parser.error('Supply three private ticket paths for authenticated network checks')
    verify_wire()
    checks = 0
    peers = []
    events = []

    def check(ok, reason):
        nonlocal checks
        checks += 1
        if not ok:
            raise RuntimeError(reason)

    def note(event, **fields):
        events.append(dict(utc=datetime.now(timezone.utc).isoformat(), event=event, **fields))

    log_path = args.report.with_suffix('.server.log')
    passed = False
    with log_path.open('x', encoding='utf-8') as log:
        server = subprocess.Popen([str(args.executable), '--listen-port', '0',
                                   '--database-config', str(args.database_config),
                                   '--channel-rules', str(ROOT / 'config/examples/channel_rules.ini')],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, creationflags=subprocess.CREATE_NO_WINDOW)
        lines = queue.Queue()

        def capture():
            for line in server.stdout:
                log.write(line)
                log.flush()
                lines.put(line)
            lines.put(None)

        reader = threading.Thread(target=capture, daemon=True)
        reader.start()

        class Peer:
            def __init__(self, role, port, register=True):
                self.role = role
                self.port = port
                self.handoff_enabled = False
                self.group_companion = False
                self.group_updates = 0
                self.group_actions = 0
                self.channel_category = 60
                self.socket = socket.create_connection(('127.0.0.1', port), timeout=10)
                if register:
                    peers.append(self)
                self.outgoing, self.incoming = Cipher(True), Cipher(False)
                check(self.exact(258) == b'\x04\x00' + bytes(256), 'Original plaintext greeting')

            def exact(self, length):
                result = bytearray()
                while len(result) < length:
                    part = self.socket.recv(length - len(result))
                    if not part:
                        raise RuntimeError('Unexpected connection end: ' + self.role)
                    result.extend(part)
                return bytes(result)

            def send(self, mid, payload=b''):
                self.socket.sendall(self.outgoing.transform(frame(mid, payload), 1))
                note('send', role=self.role, message_id=mid, payload_bytes=len(payload))

            def receive(self, wanted):
                deadline = time.monotonic() + 15
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise RuntimeError(f'{self.role}: timed out waiting for {wanted}')
                    self.socket.settimeout(remaining)
                    header = self.incoming.transform(self.exact(8), -1)
                    check(xor(header) == 0, 'Header checksum')
                    length, mid = struct.unpack_from('<IH', header)
                    check(length <= 1048576, 'Bounded response')
                    payload = self.incoming.transform(self.exact(length), -1)
                    check(xor(payload) == header[6], 'Payload checksum')
                    note('receive', role=self.role, message_id=mid, payload_bytes=length)
                    if mid == 4:
                        self.send(41)
                        continue
                    if mid == 65013 and wanted != 65013:
                        check(self.group_companion, 'Group notifications require explicit subscription')
                        self.group_state = decode_groups(payload, check)
                        self.group_updates += 1
                        continue
                    check(mid in wanted if isinstance(wanted, tuple) else mid == wanted,
                          f'{self.role}: expected {wanted}, received {mid}')
                    self.socket.settimeout(10)
                    return (mid, payload) if isinstance(wanted, tuple) else payload

            def prepare_handoff(self, category):
                grant = self.receive(65021)
                check(len(grant) == 44, 'Exact companion grant length')
                check(struct.unpack_from('<III', grant) == (1, self.character_id, category),
                      'Grant binds the authenticated character and destination category')
                check(any(grant[12:]), 'Nonempty one-use grant')
                check(self.receive(8) == struct.pack('<I4sHI', 0, bytes([127, 0, 0, 1]), self.port, category),
                      'Native success advertises the current endpoint')
                destination = Peer(self.role, self.port, register=False)
                proof = struct.pack('<II', 1, self.character_id) + grant[12:]
                try:
                    destination.send(65022, proof)
                    destination.send(54, struct.pack('<II', self.character_id, 1))
                except BaseException:
                    destination.socket.close()
                    raise
                return destination, proof

            def finish_handoff(self, phase, category, destination, proof):
                crossing = category != self.channel_category
                check(destination.receive(10) == b'', 'Old connection retirement notification')
                check(destination.receive(11) == struct.pack('<III', 1, category, 0),
                      'Native move acceptance restores connection readiness')
                if crossing:
                    check(len(destination.receive(117)) > 100, 'Cross-category destination plaza snapshot')
                self.channel_category = category
                # Preserve the test actor identity while replacing its socket/ciphers.
                self.socket, destination.socket = destination.socket, self.socket
                self.outgoing, destination.outgoing = destination.outgoing, self.outgoing
                self.incoming, destination.incoming = destination.incoming, self.incoming
                # A heartbeat sent before retirement may already be in the old stream.
                destination.socket.settimeout(3)
                while True:
                    first = destination.socket.recv(1)
                    if not first:
                        break
                    header = destination.incoming.transform(first + destination.exact(7), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    check(xor(header) == 0 and length <= 1048576, 'Retired stream header')
                    body = destination.incoming.transform(destination.exact(length), -1)
                    check(xor(body) == header[6] and mid == 4 and body == b'', 'Only an in-flight heartbeat may precede retired EOF')
                check(True, 'Retired connection reaches EOF without dropping the new session')
                replay = Peer(self.role, self.port, register=False)
                try:
                    replay.send(65022, proof)
                    check(replay.socket.recv(1) == b'', 'Consumed grant cannot authenticate a second socket')
                finally:
                    replay.socket.close()
                note('connection-handoff-passed', role=self.role, phase=phase,
                     category=category, crossed_category=crossing, same_session=True, retired_eof=True,
                     replay_refused=True, original_ui=False)

            def handoff(self, phase, during=None, category=None):
                category = self.channel_category if category is None else category
                self.send(65020, struct.pack('<I', 1))
                self.handoff_enabled = True
                self.send(53, struct.pack('<I', category))
                destination, proof = self.prepare_handoff(category)
                try:
                    check(destination.receive(9) == b'', 'Native promotion on proven destination')
                    if during:
                        during()
                    destination.send(55)
                    self.finish_handoff(phase, category, destination, proof)
                finally:
                    destination.socket.close()
                    proof = b''

            def login(self, ticket_path):
                ticket = ticket_path.read_text(encoding='utf-8').strip()
                check(ticket.startswith('BFS1.') and len(ticket) == 69, 'Private ticket format')
                self.send(1, struct.pack('<I', 1256) + bytes(92))
                self.receive(2)
                self.receive(17)
                self.send(3, text(ticket) + text('') + text('') + bytes(12) + text(''))
                ticket = ''
                self.receive(3)
                party = self.receive(201)
                self.send(65020, struct.pack('<I', 1))
                self.handoff_enabled = True
                self.send(521)
                self.receive(105)
                for mid in (117, 806, 837, 802, 810, 170):
                    body = self.receive(mid)
                    if mid == 806:
                        self.character_id = struct.unpack_from('<I', body)[0]
                        self.equipment_items = list(DEFAULT_ITEMS)
                        inventory_view(self, body, check)
                name = {'host': 'RoomPeer', 'guest': 'RoomProbe', 'observer': 'RoomObserver'}[self.role]
                verify_solo_roster(party, self.character_id, name, text, check)
                equipped_party(self, [self], text, check)
                self.send(106)
                self.receive(117)

        try:
            port = None
            for _ in range(10):
                line = lines.get(timeout=10)
                if line is None:
                    raise RuntimeError('Server exited before listening')
                if line.startswith('Listening on 127.0.0.1:'):
                    port = int(line.rsplit(':', 1)[1])
                    break
            check(port is not None, 'Ephemeral loopback listener')
            host, guest = Peer('host', port), Peer('guest', port)
            host.login(args.tickets[0])
            guest.login(args.tickets[1])
            host.receive(503)
            host.receive(501)
            verify_channel_refusals([host, guest], check, note, 'solo-plaza')
            for peer in (host, guest):
                peer.handoff('solo-plaza')
            verify_plaza_social(host, guest, check, note, text)
            verify_friend_groups(host, guest, check, note, text)
            verify_solo_administration(host, guest, check, note, text, 'plaza')
            verify_channel_worlds(host, guest, check, note, text)
            verify_group_handoff(host, guest, check, note, text)
            host.send(402, host_create_payload())
            host.receive(102)
            room = struct.unpack_from('<H', host.receive(117))[0]
            guest.receive(511)
            guest.receive(501)
            verify_invitation_entry(host, guest, check, note, text)
            observer = Peer('observer', port)
            observer.login(args.tickets[2])
            observer.handoff('join-beginner-world', category=20)
            roster(host, check)
            roster(guest, check)
            verify_invitation_guest(guest, observer, host, check, note, text)
            request(observer, text, 0)
            check(not roster(observer, check), 'Observer has enabled social protocol without a friendship')
            # Reset the shared chat/social quota before the next independent phase.
            time.sleep(3.1)
            verify_room_social(host, guest, peers, check, note, text,
                               lambda peer: barrier(peer, check))
            verify_group_removal(host, guest, observer, check, note, text)
            verify_invited_game(peers, check, note, text)
            verify_visitation_entry(peers, check, note, text)
            verify_invited_game(peers, check, note, text)
            note('visited-game-passed', loads=True, shared_start=True,
                 movement=True, explicit_return=True, original_ui=False)
            verify_solo_administration(host, guest, check, note, text, 'waiting_room')
            verify_party_consent(peers, check, note, text)
            from party_channel_network_flow import verify_party_channels
            verify_party_channels(host, guest, observer, check, note, text)
            passed = True
            note('passed', checks=checks)
        finally:
            for peer in peers:
                peer.socket.close()
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)
            reader.join(timeout=3)
            server.stdout.close()
            report = dict(passed=passed, checks=checks, participants=len(peers),
                          friend_group_actions=sum(peer.group_actions for peer in peers),
                          friend_group_updates=sum(peer.group_updates for peer in peers),
                          original_client=False, server_stopped=server.poll() is not None,
                          executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                          events=events)
            args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: three authenticated social TCP peers, {checks} checks.')


if __name__ == '__main__':
    main()
