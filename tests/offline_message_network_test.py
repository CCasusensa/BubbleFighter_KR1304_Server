"""Durable mailbox over authenticated production TCP and an isolated MySQL schema."""
import argparse
import hashlib
import json
from pathlib import Path
import queue
import socket
import struct
import subprocess
import threading
import time

from tcp_fixture import Cipher, frame, xor, text, verify_wire
from social_network_flow import Reader


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('executable', 'database-config', 'mysql', 'report'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    verify_wire()
    checks, events, peers, servers, tickets, secrets = 0, [], [], [], [], []
    passed = False
    log_path = args.report.with_suffix('.server.log')

    def check(ok, message):
        nonlocal checks
        checks += 1
        if not ok:
            raise RuntimeError(message)

    def sql(statement):
        result = subprocess.run([str(args.mysql), '--defaults-file=' + str(args.database_config),
                                 '--no-login-paths', '--batch', '--skip-column-names'],
                                input=statement, capture_output=True, text=True, timeout=10,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        check(result.returncode == 0, 'Isolated mailbox SQL succeeded')
        return result.stdout.strip()

    check(sql('SELECT DATABASE();').casefold().startswith('bfsocialtests_'), 'Generated isolated schema only')
    identities = [int(sql("SELECT c.CharacterId FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username='mail_" + name + "';"))
                  for name in ('alice', 'bob', 'carol')]
    a_id, b_id, c_id = identities
    scope = ','.join(map(str, identities))
    sql(f'DELETE FROM SocialMessages WHERE RecipientCharacterId IN ({scope}); DELETE FROM SocialChatBlocks WHERE OwnerCharacterId IN ({scope}); '
        f'DELETE FROM SocialFriendships WHERE FirstCharacterId IN ({scope}) OR SecondCharacterId IN ({scope}); '
        f'INSERT INTO SocialFriendships (FirstCharacterId, SecondCharacterId, RequestedBy, IsAccepted) VALUES ({a_id},{b_id},{a_id},1),({a_id},{c_id},{a_id},1);')

    with log_path.open('x', encoding='utf-8') as log:
        def start():
            server = subprocess.Popen([str(args.executable), '--listen-port', '0', '--database-config', str(args.database_config)],
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                      creationflags=subprocess.CREATE_NO_WINDOW)
            lines = queue.Queue()
            def capture():
                for line in server.stdout:
                    log.write(line); log.flush(); lines.put(line)
                lines.put(None)
            thread = threading.Thread(target=capture, daemon=True)
            thread.start()
            servers.append((server, thread))
            for _ in range(15):
                line = lines.get(timeout=10)
                if line is None:
                    raise RuntimeError('Mailbox server exited before listening')
                if line.startswith('Listening on 127.0.0.1:'):
                    events.append({'event': 'server-started', 'pid': server.pid})
                    return server, int(line.rsplit(':', 1)[1])
            raise RuntimeError('Missing mailbox listener')

        def stop(server):
            if server.poll() is None:
                server.terminate(); server.wait(timeout=10)
            for process, thread in servers:
                if process is server:
                    thread.join(timeout=3)
            events.append({'event': 'server-stopped', 'pid': server.pid})

        class Peer:
            def __init__(self, port):
                self.port = port
                self.socket = socket.create_connection(('127.0.0.1', port), timeout=10)
                self.outgoing, self.incoming = Cipher(True), Cipher(False)
                self.times = []
                self.character = 0
                self.presence = {}
                peers.append(self)
                check(self.exact(258) == b'\x04\0' + bytes(256), 'Original plaintext greeting')

            def exact(self, length):
                result = b''
                while len(result) < length:
                    part = self.socket.recv(length - len(result))
                    if not part:
                        raise RuntimeError('Unexpected mailbox peer EOF')
                    result += part
                return result

            def send(self, mid, payload=b'', throttle=True):
                if throttle and (mid in (65010, 65012) or (mid == 65016 and payload[4] == 0)):
                    if len(self.times) >= 4:
                        time.sleep(max(0, 3.1 - (time.monotonic() - self.times[-4])))
                    self.times = self.times[-3:] + [time.monotonic()]
                self.socket.sendall(self.outgoing.transform(frame(mid, payload), 1))

            def receive(self, wanted):
                deadline = time.monotonic() + 15
                while True:
                    self.socket.settimeout(max(0.1, deadline - time.monotonic()))
                    header = self.incoming.transform(self.exact(8), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    check(xor(header) == 0 and length <= 1048576, 'Bounded checksummed header')
                    body = self.incoming.transform(self.exact(length), -1)
                    check(xor(body) == header[6], 'Checksummed payload')
                    if mid == 4:
                        self.send(41); continue
                    if mid == 65011:
                        roster = Reader(body)
                        version, status, action = roster.u32(), roster.u32(), roster.u8()
                        roster.text(); count = roster.u32(); entries = {}
                        check(version == 1 and status <= 9 and action <= 8 and count <= 250, 'Bounded social roster header')
                        for _ in range(count):
                            identity, name, relationship, online = roster.u32(), roster.text(), roster.u8(), roster.u8()
                            check(identity not in entries and relationship <= 4 and online <= 1, 'Unique roster identity and presence')
                            entries[identity] = bool(online)
                        roster.end()
                        if action != 1 and not (status != 0 and count == 0):
                            self.presence = entries
                    if mid in (65011, 65013) and mid != wanted:
                        check(struct.unpack_from('<II', body) == (1, 0), 'Canonical asynchronous social snapshot')
                        continue
                    check(mid == wanted, f'Mailbox expected {wanted}, received {mid}')
                    return body

            def close(self):
                self.socket.close()

            def closed(self):
                self.socket.settimeout(10)
                while True:
                    first = self.socket.recv(1)
                    if not first:
                        check(True, 'Unauthorized mailbox request closes connection'); return
                    header = self.incoming.transform(first + self.exact(7), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    body = self.incoming.transform(self.exact(length), -1)
                    check(xor(header) == 0 and xor(body) == header[6] and mid in (4, 65011, 65013), 'Only preceding heartbeat or social snapshot before rejection')

            def login(self, role):
                ticket = args.database_config.parent / f'mailbox_{role}_{len(tickets)}.ticket'
                tickets.append(ticket)
                result = subprocess.run([str(args.executable), '--login', 'mail_' + role,
                    '--database-config', str(args.database_config), '--ticket-file', str(ticket)],
                    input='Synthetic-mailbox-password\n', capture_output=True, text=True, timeout=15,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                check(result.returncode == 0, 'Private authenticated ticket issued')
                secret = ticket.read_text(encoding='utf-8').strip(); secrets.append(secret)
                check(secret.startswith('BFS1.') and len(secret) == 69, 'Private ticket shape')
                self.send(1, struct.pack('<I', 1256) + bytes(92)); self.receive(2); self.receive(17)
                self.send(3, text(secret) + text('') + text('') + bytes(12) + text(''))
                self.receive(3)
                role_index = ('alice', 'bob', 'carol').index(role)
                name = ('MailRenamed', 'MailBob', 'MailCarol')[role_index]
                party_base = struct.pack('<III', 2, 1, identities[role_index]) + text(name) + struct.pack('<IIII', 0xffffffff, 0xffffffff, 0, 0)
                check(self.receive(201) == party_base + struct.pack('<I', 0), 'Canonical login party identity')
                self.send(521); self.receive(105)
                for mid in (117, 806, 837, 802, 810, 170):
                    body = self.receive(mid)
                    if mid == 806:
                        self.character = struct.unpack_from('<I', body)[0]
                        equipment = body
                check(self.receive(201) == party_base + struct.pack('<I', 1) + equipment, 'Menu party equipment matches authenticated inventory')
                check(self.character == identities[('alice', 'bob', 'carol').index(role)], 'Authenticated canonical character')

            def subscribe(self, mailbox=True):
                self.social(0); self.receive(65011)
                if mailbox:
                    self.send(65012, struct.pack('<IBIII', 1, 0, 0, 0, 0) + text('')); self.receive(65013)
                    return self.fetch()

            def social(self, action, target=0, content=''):
                self.send(65010, struct.pack('<IBI', 1, action, target) + text('') + text(content))

            def batch(self):
                reader = Reader(self.receive(65017))
                check((reader.u32(), reader.u32()) == (1, 0), 'Mailbox version and successful status')
                count = reader.u32(); check(count <= 20, 'Bounded delivery batch')
                messages = []
                for _ in range(count):
                    identity, sender, timestamp = struct.unpack('<QIQ', reader.take(20))
                    name, body = reader.text(), reader.text()
                    check(identity > (messages[-1][0] if messages else 0) and sender > 0 and 0 < timestamp <= 4102444800, 'Ordered full-width identity and server timestamp')
                    messages.append((identity, sender, timestamp, name, body))
                reader.end()
                return messages

            def fetch(self):
                self.send(65016, struct.pack('<IBQ', 1, 0, 0)); return self.batch()

            def ack(self, identity):
                self.send(65016, struct.pack('<IBQ', 1, 1, identity))

            def saved(self, target, peer_name, content):
                self.social(8, target, content)
                reader = Reader(self.receive(65015))
                check((reader.u32(), reader.u32(), reader.text(), reader.text(), reader.text()) ==
                      (1, target, peer_name, 'MailRenamed', content), 'Canonical sender echo only after persistence')
                reader.end(); self.receive(509)

            def barrier(self):
                self.send(828); check(len(self.receive(834)) == 12, 'Live transport barrier without unsolicited mailbox data')

            def handoff(self):
                self.send(106); self.receive(117)
                self.send(65020, struct.pack('<I', 1)); self.send(53, struct.pack('<I', 60))
                grant = self.receive(65021)
                check(len(grant) == 44 and struct.unpack_from('<III', grant) == (1, self.character, 60), 'Grant binds current mailbox owner')
                self.receive(8)
                destination = Peer(self.port)
                destination.send(65022, struct.pack('<II', 1, self.character) + grant[12:])
                destination.send(54, struct.pack('<II', self.character, 1)); destination.receive(9)
                destination.send(55); destination.receive(10); destination.receive(11)
                self.socket, destination.socket = destination.socket, self.socket
                self.outgoing, destination.outgoing = destination.outgoing, self.outgoing
                self.incoming, destination.incoming = destination.incoming, self.incoming
                destination.closed(); destination.close()
                events.append({'event': 'mailbox-session-handoff', 'character': self.character})

        try:
            server, port = start()
            early = Peer(port); early.send(65016, struct.pack('<IBQ', 1, 0, 0)); early.closed(); early.close()
            alice = Peer(port); alice.login('alice'); check(alice.subscribe() == [], 'Empty sender mailbox')
            alice.saved(b_id, 'MailBob', 'offline😀')
            message_id = int(sql(f'SELECT MessageId FROM SocialMessages WHERE RecipientCharacterId={b_id};'))
            check(message_id > 9007199254740992, 'Stored identity exceeds JavaScript safe integer')
            bob = Peer(port); bob.login('bob'); bob.subscribe(False); bob.barrier()
            original = bob.subscribe()
            check(len(original) == 1 and original[0][0:2] == (message_id, a_id) and original[0][3:] == ('MailRenamed', 'offline😀'), 'Offline message delivered after explicit opt-in')
            check(bob.fetch() == original, 'Read without ack is repeatable')
            bob.close(); alice.barrier()
            bob = Peer(port); bob.login('bob'); check(bob.subscribe() == original, 'Connection loss replays same record')
            bob.ack(message_id + 1); bob.closed(); bob.close()
            alice.ack(message_id); alice.closed(); alice.close()
            check(sql(f'SELECT COUNT(*) FROM SocialMessages WHERE MessageId={message_id};') == '1', 'Unknown and foreign acknowledgements preserve message')
            stop(server); server, port = start()
            bob = Peer(port); bob.login('bob'); bob.ack(message_id); bob.closed(); bob.close()
            bob = Peer(port); bob.login('bob'); check(bob.subscribe() == original, 'Process restart preserves durable message and resets issued permissions')
            bob.ack(message_id); check(bob.batch() == [], 'Acknowledgement deletes only after authenticated issue')
            bob.ack(message_id); bob.barrier()
            check(sql(f'SELECT COUNT(*) FROM SocialMessages WHERE MessageId={message_id};') == '0', 'Duplicate ack is idempotent')
            now = int(time.time())
            values = ','.join(f"({a_id},{b_id},{a_id},{b_id},{now},X'62006100740063006800')" for _ in range(21))
            sql('INSERT INTO SocialMessages (SenderCharacterId, RecipientCharacterId, FirstCharacterId, SecondCharacterId, SentAtUnixSeconds, TextUtf16) VALUES ' + values + ';')
            batch = bob.fetch(); check(len(batch) == 20, 'First page exactly twenty messages')
            for message in batch:
                bob.ack(message[0])
            last = bob.batch(); check(len(last) == 1 and last[0][0] > batch[-1][0], 'Twenty acknowledgements pass shared send quota and advance page')
            bob.ack(last[0][0]); check(bob.batch() == [], 'Last page drained')
            alice = Peer(port); alice.login('alice'); check(alice.subscribe() == [], 'Sender reconnects without pending inbox')
            alice.saved(b_id, 'MailBob', 'online queued')
            inflight = bob.batch(); check(len(inflight) == 1 and inflight[0][4] == 'online queued', 'Online opted-in recipient receives persisted message')
            bob.handoff(); bob.ack(inflight[0][0]); check(bob.batch() == [], 'Channel handoff retains issued acknowledgement authority')
            alice.saved(b_id, 'MailBob', 'revoked'); inflight = bob.batch()
            alice.social(6, b_id); check(struct.unpack_from('<I', alice.receive(170))[0] == 1, 'Native block list precedes companion result'); alice.receive(65011)
            check(sql(f'SELECT COUNT(*) FROM SocialMessages WHERE RecipientCharacterId={b_id};') == '0', 'Block revokes durable messages')
            check(bob.fetch() == [], 'Explicit read reflects revocation')
            bob.ack(inflight[0][0]); bob.barrier()
            alice.social(7, b_id); check(alice.receive(170) == bytes(4), 'Native block list clears before companion result'); alice.receive(65011); bob.close(); alice.barrier()
            carol = Peer(port); carol.login('carol'); carol.subscribe(False)
            alice.saved(c_id, 'MailCarol', 'waiting for capability'); carol.barrier()
            carol.social(8, b_id, 'legacy offline'); result = Reader(carol.receive(65011))
            # Carol is not Bob's friend: the durable feature must not change this authorization.
            check((result.u32(), result.u32(), result.u8()) == (1, 6, 8), 'Legacy nonfriend rejection remains intact')
            carol.social(8, a_id, 'legacy online'); carol.receive(65015); alice.receive(65015)
            alice.close(); carol.barrier()
            # A barrier on Carol's socket does not order Alice's independent disconnect.
            while carol.presence.get(a_id) is not False:
                carol.receive(65011)
            check(carol.presence[a_id] is False, 'Server observed sender disconnect before legacy offline request')
            carol.social(8, a_id, 'legacy offline friend'); result = Reader(carol.receive(65011))
            check((result.u32(), result.u32(), result.u8()) == (1, 7, 8), 'Legacy offline friend still reports unsent')
            pending = carol.subscribe(); check(len(pending) == 1 and pending[0][4] == 'waiting for capability', 'Legacy recipient upgrades and recovers queued message')
            carol.ack(pending[0][0]); check(carol.batch() == [], 'Upgraded recipient confirms saved delivery')
            check(sql(f'SELECT COUNT(*) FROM SocialMessages WHERE RecipientCharacterId IN ({scope});') == '0', 'All exercised durable messages acknowledged or revoked')
            events.append({'event': 'durable-mailbox-passed', 'restart': True, 'reconnect': True,
                           'owner_ack': True, 'ack_loss': True, 'paged': True, 'handoff': True,
                           'block_revocation': True, 'legacy_opt_in': True, 'original_ui': False})
            passed = True
        finally:
            for peer in peers:
                peer.close()
            for server, _ in servers:
                stop(server)
                server.stdout.close()
            for ticket in tickets:
                check(ticket.resolve().parent == args.database_config.resolve().parent, 'Private ticket cleanup boundary')
                ticket.unlink(missing_ok=True)
            report = dict(passed=passed, checks=checks, original_client=False,
                          server_processes=len(servers), server_stopped=all(p.poll() is not None for p, _ in servers),
                          private_tickets_removed=all(not p.exists() for p in tickets), events=events,
                          executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest())
            args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    public = log_path.read_text(encoding='utf-8') + args.report.read_text(encoding='utf-8')
    check(all(secret not in public for secret in secrets), 'Private tickets absent from public logs')
    report['checks'] = checks
    report['public_log_privacy_checked'] = True
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'PASS: durable mailbox authenticated TCP, {checks} checks; restart, replay, handoff and owner acknowledgements.')


if __name__ == '__main__':
    main()
