"""Separate native clan frames and trusted main-game credentials over real loopback TCP."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import queue
import socket
import struct
import subprocess
import threading
import time

from tcp_fixture import ROOT, Cipher, frame, xor, text, verify_wire
from clan_creation_network_flow import exercise as exercise_creation, confirm as confirm_creation
from clan_directory_network_flow import exercise as exercise_directory
from clan_roster_network_flow import exercise as exercise_roster
from clan_mutation_network_flow import exercise as exercise_mutation
from clan_master_transfer_network_flow import exercise as exercise_transfer
from clan_disband_network_flow import exercise as exercise_disband
from clan_training_network_flow import exercise as exercise_training
from clan_presence_network_flow import exercise as exercise_presence
from party_clan_identity_network_flow import observe as observe_party, expect as expect_party, exercise as exercise_party_clan
from clan_channel_network_flow import exercise as exercise_channel, resident_loss
from clan_plaza_network_flow import exercise as exercise_plaza, resident_loss as plaza_resident_loss

EVIDENCE = json.loads((ROOT / 'docs/evidence/clan_transport_ida.json').read_text(encoding='utf-8'))
TABLE = bytes.fromhex(next(row['bytes'] for row in EVIDENCE['data_ranges'] if row['va'] == '0x1891f28'))


def transform(mid, body):
    seed = TABLE[(len(body) + mid) % 255]
    return bytes(b ^ ((seed + seed % (i + 1)) & 255) for i, b in enumerate(body))


def clan_frame(mid, body=b''):
    return struct.pack('<HH', len(body), mid) + transform(mid, body)


def z(value):
    return value.encode('utf-16le') + b'\0\0'


def bridge(body):
    character, length = struct.unpack_from('<II', body)
    end = 8 + length * 2
    name = body[8:end].decode('utf-16le')
    version, reserved, size = struct.unpack_from('<III', body, end)
    assert len(body) == end + 12 + size and size == 40
    assert version == 1 and reserved == 0 and character > 0
    return struct.pack('<I', character) + z(name) + body[end:], character


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--database-config', type=Path, required=True)
    parser.add_argument('--mysql', type=Path)
    parser.add_argument('--tickets', type=Path, nargs=3)
    parser.add_argument('--expect-missing-schema', action='store_true')
    parser.add_argument('--expect-missing-recruitment', action='store_true')
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    command = [str(args.executable), '--listen-port', '0', '--clan-port', '0',
               '--database-config', str(args.database_config)]
    if args.expect_missing_schema or args.expect_missing_recruitment:
        result = subprocess.run(command, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
        expected = b'Clan recruitment migration is required.' if args.expect_missing_recruitment else b'1146'
        passed = result.returncode == 1 and expected in result.stderr and b'Listening on' not in result.stdout
        args.report.write_text(json.dumps(dict(passed=passed, checks=1, listened=False, expected_error=expected.decode())) + '\n')
        assert passed, 'Missing clan migration must fail before either listener'
        print('PASS: missing clan schema rejected before either listener.')
        return
    assert args.mysql and args.tickets
    verify_wire()
    fixtures = 0
    for line in (ROOT / 'tests/clan_packets.txt').read_text(encoding='utf-8').splitlines():
        if not line or line.startswith('#'): continue
        _, mid, plain, encoded = line.split()
        assert clan_frame(int(mid), b'' if plain == '-' else bytes.fromhex(plain)).hex() == encoded
        fixtures += 1
    maintenance_at = int(time.time()) + 3600
    command += ["--clan-maintenance-unix", str(maintenance_at)]
    channel_rules = args.report.with_suffix('.channel.ini')
    channel_rules.write_text('[80]\nMinimumLevel=1\nMaximumLevel=260\nMaximumMercenaries=1\n',encoding='utf-8')
    command += ['--channel-rules',str(channel_rules)]
    checks, peers, secrets = 0, [], []
    directory_queries = 0
    roster_queries = 0
    mutation_actions = 0
    transfer_actions = 0
    disband_actions = 0
    training_actions = 0
    presence_queries = 0
    channel_actions = 0
    plaza_actions = 0
    maintenance_checks = 0
    passed = False
    log_path = args.report.with_suffix('.server.log')

    def check(ok, message):
        nonlocal checks
        checks += 1
        if not ok: raise RuntimeError(message)

    def sql(statement):
        result = subprocess.run([str(args.mysql), '--defaults-file=' + str(args.database_config),
                                 '--no-login-paths', '--batch', '--skip-column-names'],
                                input=statement, capture_output=True, text=True, timeout=10,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode: raise RuntimeError('Isolated clan fixture SQL failed')
        return result.stdout.strip()

    assert sql('SELECT DATABASE();').casefold().startswith('bfclantests_')
    with log_path.open('x', encoding='utf-8') as log:
        server = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, creationflags=subprocess.CREATE_NO_WINDOW)
        lines = queue.Queue()
        def capture():
            for line in server.stdout:
                log.write(line); log.flush(); lines.put(line)
            lines.put(None)
        reader = threading.Thread(target=capture, daemon=True)
        reader.start()

        class Peer:
            def __init__(self, port, clan=False):
                self.clan = clan
                self.socket = socket.create_connection(('127.0.0.1', port), timeout=5)
                peers.append(self)
                self.outgoing, self.incoming = Cipher(True), Cipher(False)
                if clan: check(self.exact(4) == b'\0\0\xea\x03', 'Native clan S1002 greeting')
                else: check(self.exact(258) == b'\x04\0' + bytes(256), 'Native main greeting')

            def exact(self, n):
                body = b''
                while len(body) < n:
                    part = self.socket.recv(n - len(body))
                    if not part: raise RuntimeError('Unexpected peer EOF')
                    body += part
                return body

            def send(self, mid, body=b'', split=False):
                wire = clan_frame(mid, body) if self.clan else self.outgoing.transform(frame(mid, body), 1)
                if split:
                    for byte in wire: self.socket.sendall(bytes([byte]))
                else: self.socket.sendall(wire)

            def receive(self, wanted):
                while True:
                    if self.clan:
                        length, mid = struct.unpack('<HH', self.exact(4))
                        check(length <= 65531, 'Bounded clan response')
                        body = transform(mid, self.exact(length))
                    else:
                        header = self.incoming.transform(self.exact(8), -1)
                        check(xor(header) == 0, 'Main header checksum')
                        length, mid = struct.unpack_from('<IH', header)
                        check(length <= 1048576, 'Bounded main response')
                        body = self.incoming.transform(self.exact(length), -1)
                        check(xor(body) == header[6], 'Main body checksum')
                        if mid == 4: self.send(41); continue
                        if mid == 806: self.party_equipped = True
                        if mid == 201:
                            observe_party(self, body, check)
                            if wanted != 201: continue
                    check(mid == wanted, f'Expected message {wanted}, received {mid}')
                    return body

            def closed(self):
                self.socket.settimeout(5)
                while True:
                    try: data = self.socket.recv(1)
                    except (ConnectionResetError, ConnectionAbortedError): data = b''
                    if not data: break
                    check(not self.clan, 'Revoked clan stream has no remaining messages')
                    # A heartbeat may race the final valid response and rejection.
                    # Decode it exactly; no other queued response is accepted here.
                    header = self.incoming.transform(data + self.exact(7), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    check(xor(header) == 0 and length == 0 and mid == 4 and header[6] == 0,
                          'Only native empty heartbeat may precede main EOF')
                check(True, 'Rejected or revoked stream closes')

            def issue(self):
                self.send(25)
                credential, character = bridge(self.receive(21))
                secrets.append(credential[-40:].hex())
                self.character = character
                return credential

            def expect_clan(self, clan_id, timeout=3):
                expect_party(self, clan_id, check, timeout)

            def login(self, ticket_path, role=None):
                self.role = role or ('host' if ticket_path.stem == 'maintenance' else ticket_path.stem)
                check(self.role in ('host', 'guest', 'observer'), 'Known isolated network login role')
                self.character = int(sql(f"SELECT c.CharacterId FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username='net_{self.role}';"))
                ticket = ticket_path.read_text().strip()
                secrets.append(ticket)
                self.send(1, struct.pack('<I', 1256) + bytes(92))
                self.receive(2); self.receive(17)
                self.send(3, text(ticket) + text('') + text('') + bytes(12) + text(''))
                self.receive(3); self.receive(201)

        try:
            ports = {}
            while len(ports) != 2:
                line = lines.get(timeout=10)
                if line is None: raise RuntimeError('Server exited before dual listen')
                if line.startswith('Clan Listening on 127.0.0.1:'): ports['clan'] = int(line.rsplit(':', 1)[1])
                if line.startswith('Listening on 127.0.0.1:'): ports['main'] = int(line.rsplit(':', 1)[1])
            check(ports['main'] != ports['clan'], 'Distinct native services')
            def clan(): return Peer(ports['clan'], True)
            def auth(credential, result=1, split=False):
                peer = clan()
                peer.send(1005, credential, split)
                check(peer.receive(1006) == struct.pack('<I', result), 'Authentication result')
                if result != 1: peer.closed()
                return peer
            def profile(peer, cid=0, role=0, details=0):
                peer.send(1015)
                body = peer.receive(1016)
                check(struct.unpack_from('<BII', body) == (details, cid, role), 'Authoritative membership')
                check(body[-8:] == struct.pack('<d', 73051.0), 'Native 2100 date sentinel')
                return body

            early_report = Peer(ports['main']); early_report.send(2025, bytes(12)); early_report.closed()
            early_confirmation = Peer(ports['main']); early_confirmation.send(806, struct.pack('<III',100819,1,0)); early_confirmation.closed()
            preauth = Peer(ports['main']); preauth.send(25); preauth.closed()
            early = clan(); early.send(1015); early.closed()
            early_presence = clan(); early_presence.send(1045, bytes(8)); early_presence.closed()
            for mid, body in ((1011,b''),(1013,struct.pack('<I',1)),(1069,b'')):
                early_roster = clan(); early_roster.send(mid,body); early_roster.closed()
            early_search = clan(); early_search.send(1019, struct.pack('<II',6,1)+z('')); early_search.closed()
            malformed = clan(); malformed.socket.sendall(struct.pack('<HH', 65532, 1005)); malformed.closed()
            truncated = clan(); truncated.socket.sendall(b'\1'); truncated.socket.shutdown(socket.SHUT_WR); truncated.closed()
            mixed = clan(); mixed.socket.sendall(frame(25)); mixed.closed()
            waiting = clan(); waiting_since = time.monotonic()
            games = [Peer(ports['main']) for _ in args.tickets]
            for game, ticket in zip(games, args.tickets): game.login(ticket)
            host, guest, observer = games
            first = host.issue(); active = auth(first, split=True)
            profile(active)
            auth(first, 0xffffffff)
            second = host.issue(); third = host.issue()
            auth(second, 0xffffffff)
            bad = bytearray(third); bad[0] ^= 1
            auth(bytes(bad), 0xffffffff); auth(third, 0xffffffff)
            profile(active)  # Failed replacement attempts do not revoke it.
            current = host.issue(); newer = auth(current); active.closed()
            newer.socket.sendall(clan_frame(1003) + clan_frame(1015) + clan_frame(1017))
            check(newer.receive(1016) == newer.receive(1016), 'Coalesced queries resynchronize absent membership')
            race_credential = host.issue()
            competitors = [clan() for _ in range(8)]
            barrier = threading.Barrier(8)
            def attempt(peer):
                barrier.wait(); peer.send(1005, race_credential)
                return struct.unpack('<I', peer.receive(1006))[0]
            with concurrent.futures.ThreadPoolExecutor(8) as pool: results = list(pool.map(attempt, competitors))
            check(results.count(1) == 1 and results.count(0xffffffff) == 7, 'One success in real concurrent credential consumption')
            active = competitors[results.index(1)]
            for peer, result in zip(competitors, results):
                if result != 1: peer.closed()
            newer.closed()
            guest_clan = auth(guest.issue()); observer_clan = auth(observer.issue())
            guest_clan, training_actions = exercise_training(guest, guest_clan, auth, sql, check, z)
            for game in games: game.issue()
            exercise_creation(host, guest, active, guest_clan, sql, check, z, profile)
            for game in games: game.issue()
            sql("INSERT INTO Clans(ClanId,NameUtf16,IntroductionUtf16,MemberCapacity,CreatedAtUnixSeconds) VALUES(9000,UNHEX('4E006500740043006C0061006E00'),UNHEX('48006900'),30,1704067200);")
            for game, role in zip(games, (101, 100, 1)):
                sql(f'INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES({game.character},9000,{role});')
            profile(active, 9000, 101, 1); profile(guest_clan, 9000, 100, 1); profile(observer_clan, 9000, 1, 0)
            active.send(1017)
            expected = (z('NetClan') + struct.pack('<I', 0) + z('RoomPeer') + struct.pack('<I', 0) + z('') +
                        struct.pack('<IIdQIII', 1, 0, 45292.0, 0, 0, 2, 0) + z('') + z('Hi') + struct.pack('<II', 0, 30))
            check(active.receive(1018) == expected, 'Persisted profile exact native field order')
            guest_clan, directory_queries = exercise_directory(host, guest, active, guest_clan, observer_clan, auth, sql, check, z)
            guest_clan, roster_queries = exercise_roster(host, guest, observer, active, guest_clan, observer_clan, auth, sql, check, z)
            mutation_actions = exercise_mutation(host, guest, observer, active, guest_clan, observer_clan, sql, check, z)
            guest_clan, transfer_actions = exercise_transfer(host, guest, observer, active, guest_clan, observer_clan, auth, sql, check, z)
            guest_clan, presence_queries = exercise_presence(host, guest, active, guest_clan, auth, sql, check, z)
            exercise_party_clan(host, guest, observer, active, guest_clan, observer_clan, sql, check, z)
            observer_clan = auth(observer.issue())
            channel_actions += exercise_channel(host, guest, observer, active, guest_clan, Peer, ports['main'], sql, check, z, secrets)
            plaza_actions += exercise_plaza(host, guest, observer, active, sql, check, z)
            disband_actions = exercise_disband(host, guest, active, guest_clan, observer_clan, sql, check, z, maintenance_at)
            observer_clan.send(1017)
            check(observer_clan.receive(1016)[0] == 0, 'Applicant cannot obtain member details')
            active.send(1027, z('Create') + struct.pack('<I', 0) + z('Intro') + struct.pack('<II', 100819, 30))
            check(active.receive(1028) == struct.pack('<III',0xffffff97,0,0), 'Existing member cannot create another clan')
            check(sql('SELECT COUNT(*) FROM Clans;') == '1', 'Refused creation made no database writes')
            sql(f'DELETE FROM ClanMembers WHERE CharacterId={guest.character};')
            guest_clan.send(1017)
            check(struct.unpack_from('<BII', guest_clan.receive(1016)) == (0, 0, 0), 'Membership loss is reread')
            sql("UPDATE Accounts SET IsDisabled=1 WHERE Username='net_guest';")
            guest_clan.send(1015); guest_clan.closed()
            auth(guest.issue(), 0xffffffff)
            sql("UPDATE Accounts SET IsDisabled=0 WHERE Username='net_guest';")
            revived = auth(guest.issue()); profile(revived)
            # The longer transfer exercise may leave a native heartbeat queued
            # on this idle main stream. Drain through a known response first.
            observer.issue()
            observer.send(2025, b'\0'); observer.closed(); observer.socket.close(); observer_clan.closed()
            # Native C25 remains auxiliary while a same-category main handoff is pending.
            if not getattr(host, "initialized", False):
                host.send(521)
                for mid in (105,117,806,837,802,810,170,201): host.receive(mid)
            host.send(65020, struct.pack('<I',1)); host.send(53, struct.pack('<I',60))
            grant = host.receive(65021); host.receive(8)
            destination = Peer(ports['main'])
            destination.role, destination.character = host.role, host.character
            destination.party_equipped = host.party_equipped
            destination.send(65022, struct.pack('<II',1,host.character) + grant[12:])
            destination.send(54,struct.pack('<II',host.character,1)); destination.receive(9)
            profile(active,9000,101,1)
            active.send(1045, struct.pack('<II', 260, 6))
            active.send(1011)
            check(active.receive(1012) == struct.pack('<II',1,host.character)+z('RoomPeer')+struct.pack('<III',0,0,101), 'Pending handoff publishes unknown despite forged clan scene')
            presence_queries += 1
            destination.send(55); destination.receive(10); destination.receive(11)
            active.send(1011)
            check(active.receive(1012) == struct.pack('<II',1,host.character)+z('RoomPeer')+struct.pack('<III',1,0,101), 'Committed same-category handoff restores native menu scene')
            presence_queries += 1
            destination.character = host.character
            confirm_creation(destination, sql, check)
            profile(active,9000,101,1)
            handoff_credential = destination.issue(); after_handoff = auth(handoff_credential); active.closed()
            destination.socket.close(); after_handoff.closed()
            # A new authenticated main must load membership in its very first
            # S201, before requesting any auxiliary clan credentials.
            reconnect_ticket = args.database_config.parent / 'party_reconnect.ticket'
            try:
                issued = subprocess.run([str(args.executable), '--login', 'net_host', '--database-config', str(args.database_config), '--ticket-file', str(reconnect_ticket)],
                                        input='Synthetic-network-password\n', text=True, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                check(issued.returncode == 0, 'Issue trusted party membership reconnect')
                reconnect = Peer(ports['main'])
                reconnect.login(reconnect_ticket, role='host')
                reconnect.expect_clan(9000)
                channel_actions += resident_loss(reconnect, Peer, ports['main'], sql, check, secrets)
                reconnect.socket.close()
                reconnect_ticket.unlink()
                issued = subprocess.run([str(args.executable), '--login', 'net_host', '--database-config', str(args.database_config), '--ticket-file', str(reconnect_ticket)],
                                        input='Synthetic-network-password\n', text=True, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                check(issued.returncode == 0, 'Issue trusted private plaza residency reconnect')
                private_reconnect = Peer(ports['main'])
                private_reconnect.login(reconnect_ticket, role='host')
                private_reconnect.expect_clan(9000)
                plaza_actions += plaza_resident_loss(private_reconnect, sql, check)
                private_reconnect.socket.close()
            finally:
                reconnect_ticket.unlink(missing_ok=True)
            # Heartbeats must not extend the 10-second authentication window.
            while time.monotonic() - waiting_since < 10.2:
                try: waiting.send(1003)
                except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError): break
                time.sleep(0.2)
            waiting.closed()
            # Occupied clan endpoint prevents the main listener starting.
            occupied = subprocess.run([str(args.executable),'--listen-port','0','--clan-port',str(ports['clan']),
                                       '--database-config',str(args.database_config)],capture_output=True,
                                      timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
            check(occupied.returncode == 1 and b'Listening on' not in occupied.stdout, 'Clan bind failure is atomic at startup')
            maintenance_command = [str(args.executable), '--clan-maintenance', '--database-config', str(args.database_config)]
            blocked = subprocess.run(maintenance_command, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
            check(blocked.returncode == 1 and b'already running' in blocked.stderr, 'Maintenance refuses running clan service')
            second_service = subprocess.run(command, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
            check(second_service.returncode == 1 and b'Listening on' not in second_service.stdout, 'Database-scoped guard rejects duplicate clan service')
            server.terminate(); server.wait(timeout=10); reader.join(timeout=5)
            sql('UPDATE Clans SET DisbandAtUnixSeconds=1704067300 WHERE ClanId=9000;')
            before_receipts = sql('SELECT COUNT(*) FROM ClanCreationReceipts;')
            for expected in (1, 0):
                maintained = subprocess.run(maintenance_command, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                check(maintained.returncode == 0 and f'Clans disbanded: {expected}'.encode() in maintained.stdout and b'Listening on' not in maintained.stdout,
                      'Offline maintenance deletes due clan exactly once without listener')
                maintenance_checks += 1
            check(sql('SELECT COUNT(*) FROM Clans WHERE ClanId=9000;') == '0', 'Due clan deleted')
            check(sql('SELECT COUNT(*) FROM ClanMembers WHERE ClanId=9000;') == '0', 'Membership cascade committed')
            check(sql('SELECT COUNT(*) FROM ClanDisbandNotices WHERE ClanId=9000;') == '2', 'Both accepted and pending members have durable notices')
            check(sql('SELECT COUNT(*) FROM ClanCreationReceipts;') == before_receipts, 'All historical creation receipts survive maintenance')
            with socket.socket() as occupied_main:
                occupied_main.bind(('127.0.0.1', 0)); occupied_main.listen()
                main_occupied = subprocess.run([str(args.executable),'--listen-port',str(occupied_main.getsockname()[1]),'--clan-port','0',
                                                '--database-config',str(args.database_config)],capture_output=True,
                                               timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
            check(main_occupied.returncode == 1 and b'Clan Listening on' in main_occupied.stdout,
                  'Main bind failure unwinds and joins the started clan worker')
            orphan_port = int(main_occupied.stdout.split(b'Clan Listening on 127.0.0.1:')[1].splitlines()[0])
            with socket.socket() as probe:
                probe.settimeout(1)
                check(probe.connect_ex(('127.0.0.1',orphan_port)) != 0, 'Unwound clan listener closed')
            once_log = args.report.with_suffix('.once.log')
            with once_log.open('x',encoding='utf-8') as once_output:
                once = subprocess.Popen(command + ['--once'],stdout=once_output,stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
                try:
                    endpoint = {}
                    deadline = time.monotonic() + 10
                    while len(endpoint) != 2 and time.monotonic() < deadline:
                        for line in once_log.read_text(encoding='utf-8').splitlines():
                            if line.startswith('Clan Listening on 127.0.0.1:'): endpoint['clan'] = int(line.rsplit(':',1)[1])
                            if line.startswith('Listening on 127.0.0.1:'): endpoint['main'] = int(line.rsplit(':',1)[1])
                        if len(endpoint) != 2: time.sleep(0.05)
                    check(len(endpoint) == 2,'Once server started both listeners')
                    once_clan = Peer(endpoint['clan'],True)
                    once_main = Peer(endpoint['main'])
                    recovery_ticket = args.database_config.parent / 'maintenance.ticket'
                    try:
                        issue = subprocess.run([str(args.executable), '--login', 'net_host', '--database-config', str(args.database_config), '--ticket-file', str(recovery_ticket)],
                                               input='Synthetic-network-password\n', text=True, capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
                        check(issue.returncode == 0, 'Issue trusted recovery login')
                        once_main.login(recovery_ticket)
                    finally:
                        recovery_ticket.unlink(missing_ok=True)
                    once_clan.send(1005, once_main.issue()); check(once_clan.receive(1006) == struct.pack('<I',1), 'Recovery clan authentication')
                    once_clan.send(1015)
                    check(once_clan.receive(1016) == struct.pack('<BII',0,9000,0xffffffd8)+z('NetClan')+struct.pack('<d',73051.0), 'Restart receives native -40 cache clearing event')
                    absent = struct.pack('<BII',0,0,0)+z('')+struct.pack('<d',73051.0)
                    check(once_clan.receive(1016) == absent, 'Removal followed by canonical absent profile')
                    once_clan.send(1015); check(once_clan.receive(1016) == absent, 'No duplicate removal popup in same connection')
                    again = Peer(endpoint['clan'],True)
                    again.send(1005,once_main.issue()); again.receive(1006); once_clan.closed()
                    again.send(1017); check(struct.unpack_from('<BII',again.receive(1016)) == (0,9000,0xffffffd8), 'Reconnect recovers durable notice')
                    check(again.receive(1016) == absent, 'Reconnect recovers canonical profile')
                    once_main.socket.close()
                    check(once.wait(timeout=5) == 0,'Normal main completion joins clan workers')
                    again.closed()
                finally:
                    if once.poll() is None: once.terminate()
                    once.wait(timeout=5)
            for options in (['--clan-port','-1'], ['--clan-port','65536'], ['--clan-port','1'],
                            ['--listen-port','17000','--clan-port','17000','--database-config',str(args.database_config)],
                            ['--clan-maintenance'], ['--clan-maintenance','--database-config',str(args.database_config),'--once'],
                            ['--listen-port','0','--clan-maintenance-unix','0'], ['--listen-port','0','--clan-maintenance-unix','-1'],
                            ['--listen-port','0','--clan-maintenance-unix','4102444800'], ['--listen-port','0','--clan-maintenance-unix','1704067200'],
                            ['--listen-port','0','--clan-maintenance-unix',str(maintenance_at)]):
                result = subprocess.run([str(args.executable),*options],capture_output=True,timeout=10,creationflags=subprocess.CREATE_NO_WINDOW)
                check(result.returncode == 2 and b'Listening on' not in result.stdout, 'Invalid clan CLI fails before listen')
            passed = True
        finally:
            for peer in peers: peer.socket.close()
            if server.poll() is None: server.terminate()
            server.wait(timeout=10); reader.join(timeout=5)
            log.flush()
            output = log_path.read_text(encoding='utf-8')
            check(all(secret not in output for secret in secrets), 'No credentials in logs')
            if channel_actions == 10:
                check('Clan channel membership is no longer eligible.' in output,
                      'Resident EOF is an authoritative eligibility rejection, not a heartbeat timeout')
            if plaza_actions:
                check('Clan plaza membership is no longer eligible.' in output,
                      'Private resident EOF has an authoritative rejection cause')
            args.report.write_text(json.dumps(dict(passed=passed, checks=checks, frozen_wire_fixtures=fixtures,
                server_pid=server.pid, server_stopped=server.poll() is not None, original_ui=False,
                one_use_concurrent_peers=8, trusted_main_accounts=3, same_session_handoff=True,
                credential_values_logged=False, atomic_creation=True, receipt_replay_after_handoff=True,
                party_clan_updates=sum(getattr(p,'party_updates',0) for p in peers),
                party_clan_assertions=sum(getattr(p,'party_assertions',0) for p in peers),
                clan_channel_actions=channel_actions, clan_channel_rules=str(channel_rules),
                clan_plaza_actions=plaza_actions,
                directory_queries=directory_queries, roster_queries=roster_queries, mutation_actions=mutation_actions, transfer_actions=transfer_actions, disband_actions=disband_actions, maintenance_checks=maintenance_checks, training_actions=training_actions, presence_queries=presence_queries, authenticated_main_clan_reports=5), indent=2) + '\n')
    print(f'PASS: clan dual-port TCP, {checks} checks, {fixtures} frozen frames.')


if __name__ == '__main__': main()
