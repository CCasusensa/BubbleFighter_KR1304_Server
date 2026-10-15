"""Bundle lifecycle over authenticated production TCP and isolated MySQL."""
import argparse
import hashlib
import json
from pathlib import Path
import queue
import re
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
    parser.add_argument("--expect-missing-schema", action="store_true")
    args = parser.parse_args()
    if args.expect_missing_schema:
        result = subprocess.run([str(args.executable), "--listen-port", "0", "--once", "--database-config", str(args.database_config)], capture_output=True, timeout=10, creationflags=subprocess.CREATE_NO_WINDOW)
        codes = [int(code) for code in re.findall(rb"\b[0-9]{4}\b", result.stderr)]
        passed = result.returncode == 1 and 1146 in codes and b"Listening on" not in result.stdout
        args.report.write_text(json.dumps(dict(passed=passed,checks=1,server_exit=result.returncode,mysql_codes=codes,listened=b"Listening on" in result.stdout))+"\n",encoding="utf-8",newline="\n")
        if not passed: raise RuntimeError("Missing cash schema rejection failed: " + str(codes))
        print("PASS: missing bundle schema rejected before listening.")
        return
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
        check(result.returncode == 0, 'Isolated bundle SQL succeeded')
        return result.stdout.strip()

    check(sql('SELECT DATABASE();').casefold().startswith('bfbundletests_'), 'Generated isolated bundle schema only')
    identities = [int(sql("SELECT c.CharacterId FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username='bundle_" + role + "';")) for role in ('owner','other')]
    scope = ','.join(map(str,identities))
    sql(f'DELETE c FROM CashBundleComponents c JOIN CashBundles b ON b.SlotId=c.SlotId WHERE b.CharacterId IN ({scope}); DELETE FROM CashBundles WHERE CharacterId IN ({scope}); DELETE FROM CashInventorySlots WHERE CharacterId IN ({scope}); DELETE FROM CashPurchases WHERE CharacterId IN ({scope}); DELETE FROM InventorySlots WHERE CharacterId IN ({scope}); UPDATE Characters SET Cash=2000000,Lucci=100000 WHERE CharacterId IN ({scope});')

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
                    raise RuntimeError('Bundle server exited before listening')
                if line.startswith('Listening on 127.0.0.1:'):
                    events.append({'event': 'server-started', 'pid': server.pid})
                    return server, int(line.rsplit(':', 1)[1])
            raise RuntimeError('Missing bundle listener')

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
                self.character = 0
                self.cash_slots = []
                self.notices = []
                peers.append(self)
                check(self.exact(258) == b'\x04\0' + bytes(256), 'Original plaintext greeting')

            def exact(self, length):
                result = b''
                while len(result) < length:
                    part = self.socket.recv(length - len(result))
                    if not part:
                        raise RuntimeError('Unexpected bundle peer EOF')
                    result += part
                return result

            def send(self, mid, payload=b''):
                self.socket.sendall(self.outgoing.transform(frame(mid, payload), 1))

            def receive(self, wanted):
                deadline = time.monotonic() + 15
                while True:
                    self.socket.settimeout(max(0.1, deadline - time.monotonic()))
                    header = self.incoming.transform(self.exact(8), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    check(xor(header) == 0 and length <= (2097152 if mid == 800 else 1048576), 'Bounded checksummed header')
                    body = self.incoming.transform(self.exact(length), -1)
                    check(xor(body) == header[6], 'Checksummed payload')
                    if mid == 4:
                        self.send(41); continue
                    if mid == 819 and wanted != 819:
                        notice = struct.unpack('<II', body)
                        check(notice[0] == 0 and notice[1] <= 200, 'Bounded persisted pending count')
                        self.notices.append(notice[1]); continue
                    if mid == 201 and wanted != 201:
                        check(len(body) > 32, 'Asynchronous authenticated appearance update')
                        continue
                    check(mid == wanted, f'Bundle expected {wanted}, received {mid}')
                    return body

            def close(self):
                self.socket.close()

            def closed(self):
                self.socket.settimeout(10)
                while True:
                    first = self.socket.recv(1)
                    if not first:
                        check(True, 'Unauthorized bundle request closes connection'); return
                    header = self.incoming.transform(first + self.exact(7), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    check(xor(header) == 0 and length <= 1048576, 'Bounded rejection header')
                    body = self.incoming.transform(self.exact(length), -1)
                    check(xor(body) == header[6] and mid == 4, 'Only preceding heartbeat before rejection')

            def login(self, role):
                ticket = args.database_config.parent / f'bundle_{role}_{len(tickets)}.ticket'
                tickets.append(ticket)
                result = subprocess.run([str(args.executable), '--login', 'bundle_' + role,
                    '--database-config', str(args.database_config), '--ticket-file', str(ticket)],
                    input='Synthetic-bundle-password\n', capture_output=True, text=True, timeout=15,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                check(result.returncode == 0, 'Private authenticated ticket issued')
                secret = ticket.read_text(encoding='utf-8').strip(); secrets.append(secret)
                check(secret.startswith('BFS1.') and len(secret) == 69, 'Private ticket shape')
                self.send(1, struct.pack('<I', 1256) + bytes(92)); self.receive(2); self.receive(17)
                self.send(3, text(secret) + text('') + text('') + bytes(12) + text(''))
                self.receive(3)
                role_index = ('owner', 'other').index(role)
                name = ('BundleOwner', 'BundleOther')[role_index]
                party_base = struct.pack('<III', 2, 1, identities[role_index]) + text(name) + struct.pack('<IIII', 0xffffffff, 0xffffffff, 0, 0)
                check(self.receive(201) == party_base + struct.pack('<I', 0), 'Canonical login party identity')
                self.send(521); self.receive(105)
                for mid in (117, 806, 837, 802, 810, 170):
                    body = self.receive(mid)
                    if mid == 810: self.cash_slots = cash_snapshot(body)
                    if mid == 806:
                        self.character = struct.unpack_from('<I', body)[0]
                        equipment = body
                check(self.receive(201) == party_base + struct.pack('<I', 1) + equipment, 'Menu party equipment matches authenticated inventory')
                check(self.character == identities[('owner', 'other').index(role)], 'Authenticated canonical character')


        expected_catalog = json.loads((Path(__file__).parent/'bundle_publication_cases.json').read_text('utf-8'))
        expected_cards = {c['id']: c for c in expected_catalog['cards']}

        def browse(peer, revision=0, selected=800, refresh=True):
            peer.send(801, struct.pack('<QI', revision, selected))
            body = peer.receive(800)
            reader = Reader(body)
            check((reader.u32(), reader.u32()) == (1, int(refresh)), 'Native catalog popup and refresh flags')
            current = struct.unpack('<Q', reader.take(8))[0]
            check(current != 0 and current != expected_catalog['ordinary_revision'], 'Bundle content invalidates ordinary-only cache')
            count = reader.u32()
            check(count == (7555 if refresh else 0), 'Complete supported catalog or same-session cache hit')
            found, ordinary = {}, 0
            for _ in range(count):
                start = reader.at
                identity, kind, name = reader.u32(), reader.u32(), reader.text()
                order, main, sub, visible = [reader.u32() for _ in range(4)]
                reader.take(16)
                representative = reader.take(26)
                options = reader.u32()
                check(options <= 8192, 'Bounded nested price count')
                reader.take(26*options)
                if kind == 0:
                    ordinary += 1
                    continue
                check(kind == 1 and identity in expected_cards and identity not in found, 'Only unique independent eligible bundle identities')
                check(body[start:reader.at].hex() == expected_cards[identity]['wire_hex'], 'Exact independently frozen bundle card bytes')
                found[identity] = identity
            check(reader.take(8) == bytes(8), 'Opaque outer field remains zero')
            categories = []
            for _ in range(reader.u32()):
                identity, name, order = reader.u32(), reader.text(), reader.u32()
                pages = []
                for _ in range(reader.u32()):
                    pages.append((reader.u32(),reader.text(),reader.u32(),reader.u32()))
                categories.append((identity,name,order,pages))
            check(reader.u32() == 0, 'No catalog exclusions')
            check(reader.u32() == (selected if selected in (100,200,300,400,500,800) else 300), 'Requested native category or stable fallback')
            reader.end()
            if refresh:
                check(ordinary == 7069 and set(found) == set(expected_cards), 'All ordinary and exactly486 supported bundle cards')
                check(len(categories) == 6 and categories[-1] == (800,'礼包',1006,[(800,'全部礼包',1510,2)]), 'Original Packages tab with main-category page')
                check(1048576 < len(body) < 2097152, 'Catalog fits existing S800-only two MiB allowance')
                check(all(identity not in found for identity in expected_catalog['refused']), 'Hidden, special and extended bundles remain unpublished')
            else:
                check(not categories and len(body) == 40, 'Native compact cache-hit payload')
            events.append(dict(event='catalog', refresh=refresh, revision=current, selected=selected, bytes=len(body), bundles=len(found)))
            return current, found

        def cash_snapshot(body):
            reader = Reader(body)
            count = reader.u32()
            check(count <= 1024, 'Bounded pending cash snapshot')
            slots = []
            for _ in range(count):
                slot, unknown = reader.u32(), reader.u32()
                quantity, days = struct.unpack('<HH', reader.take(4))
                stock, item, note, flag = reader.u32(), reader.u32(), reader.text(), reader.u8()
                check(unknown == 0 and note == '' and flag in (0,1), 'Known cash slot baseline and explicit bundle flag')
                slots.append(dict(slot=slot,quantity=quantity,days=days,stock=stock,item=item,flag=flag))
            reader.end()
            return slots


        def buy(peer, stock):
            peer.send(809, struct.pack('<II',stock,0))
            peer.receive(105)
            slots = cash_snapshot(peer.receive(810))
            response = Reader(peer.receive(816))
            check((response.u32(),response.u32(),response.text(),response.u8(),response.u32()) == (stock,1,'',0,0), 'Committed purchase response')
            identity = response.u32(); response.u32(); response.end()
            check(any(s['slot']==identity and s['flag']==1 for s in slots), 'New purchase has a refundable owned slot')
            return identity, slots

        def result(peer, slot, status, balance=None):
            if status == 1:
                peer.receive(105)
                check(struct.unpack('<III',peer.receive(834)) == (0,balance,0), 'Exact committed cash balance')
            reply = Reader(peer.receive(824))
            check((reply.u32(),reply.u32(),reply.text()) == (slot,status,''), 'Original S824 result layout')
            reply.end()
            return cash_snapshot(peer.receive(810))

        def refund(peer, slot, status, balance=None, request_text=''):
            peer.send(817,text(request_text)+struct.pack('<I',slot))
            return result(peer,slot,status,balance)

        def balance(peer, expected):
            peer.send(828)
            check(struct.unpack('<III',peer.receive(834)) == (0,expected,0), 'Fresh persisted cash query')

        def claim(peer, slot, status=1):
            peer.send(829,struct.pack('<I',slot))
            if status==1:
                peer.receive(105);peer.receive(802);peer.receive(806)
            reply=Reader(peer.receive(835))
            check((reply.u32(),reply.u32(),reply.u32(),reply.text())==(slot,status,0,''),'Original bundle claim response')
            reply.end()
            return cash_snapshot(peer.receive(810))

        def counts():
            return tuple(map(int,sql(f'SELECT Cash,Lucci FROM Characters WHERE CharacterId={identities[0]};').split()))

        try:
            server,port=start()
            unauthenticated=Peer(port)
            unauthenticated.send(809,struct.pack('<II',2000001,0));unauthenticated.closed()
            alice=Peer(port);alice.login('owner')
            bob=Peer(port);bob.login('other')
            check(alice.cash_slots==[] and bob.cash_slots==[],'Initial warehouses empty')
            revision, offers = browse(alice, expected_catalog['ordinary_revision'])
            check(browse(alice, revision, 100, False)[0] == revision, 'Same-session cache retains version')
            check(browse(bob, revision)[0] == revision, 'Foreign connection cannot forge a cache hit')
            check(browse(alice, 0, 99999)[0] == revision, 'Invalid selection fallback leaves catalog version stable')
            check(counts()==(2000000,100000),'Browsing does not change balances')
            identity,slots=buy(alice,offers[2000001])
            check(slots==[dict(slot=identity,quantity=0,days=0,stock=2000001,item=2000001,flag=1)],'One native bundle warehouse card')
            check(counts()==(1995500,100000),'Purchase does not grant bonus early')
            check(refund(bob,identity,2)==[],'Foreign refund denied')
            check(claim(bob,identity,2)==[],'Foreign pickup denied')
            check(claim(alice,identity)==[],'Whole bundle claimed')
            check(counts()==(1995500,160000),'Exactly one bonus and debit')
            check(sql(f'SELECT Quantity FROM InventorySlots WHERE CharacterId={identities[0]} AND ItemId=100081;')=='100','Exact component quantity')
            check(claim(alice,identity,2)==[] and refund(alice,identity,2)==[],'Claim replay and later refund denied')
            identity,_=buy(alice,offers[2000105])
            alice.close();bob.close();stop(server)
            server,port=start()
            alice=Peer(port);alice.login('owner')
            check(browse(alice, revision)[0] == revision, 'Restart sends full stable catalog')
            check(len(alice.cash_slots)==1 and alice.cash_slots[0]['slot']==identity,'Restart restores immutable bundle receipt')
            check(refund(alice,identity,1,1995500)==[],'Full price refunded after restart')
            identity,_=buy(alice,offers[2000105])
            alice.send(829,struct.pack('<I',identity));alice.close()
            deadline=time.monotonic()+8
            while sql(f'SELECT BundleState FROM CashBundles WHERE SlotId={identity};')!='1':
                check(time.monotonic()<deadline,'Lost response pickup committed');time.sleep(.05)
            alice=Peer(port);alice.login('owner')
            check(claim(alice,identity,2)==[],'Lost response replay does not duplicate components')
            check(sql(f'SELECT COUNT(*) FROM InventorySlots WHERE CharacterId={identities[0]} AND ItemId IN (201074,100111,100241,300007);')=='4','All four components survive restart')
            alice.send(809,struct.pack('<II',offers[2000107],0))
            alice.receive(105);alice.receive(802);alice.receive(806)
            response=Reader(alice.receive(816))
            check((response.u32(),response.u32(),response.text(),response.u8(),response.u32(),response.u32(),response.u32())==(2000107,1,'',0,0,0,0),'Lucci bundle delivers directly')
            response.end()
            check(counts()==(1991100,155500),'Lucci bundle charges the Lucci balance')
            check(sql(f'SELECT COUNT(*) FROM InventorySlots WHERE CharacterId={identities[0]} AND ItemId BETWEEN 207029 AND 207036;')=='8','All eight Lucci components persisted')
            alice.send(829,b'\x01\x02');alice.closed()
            events.append(dict(event='bundles-passed',cash_bonus=True,multiple_components=True,lucci=True,foreign_rejection=True,restart=True,lost_response=True,replay=True,malformed_rejection=True))
            passed=True
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
    print(f'PASS: bundle authenticated TCP, {checks} checks; atomic rewards, refunds, restart and replay.')


if __name__ == '__main__':
    main()
