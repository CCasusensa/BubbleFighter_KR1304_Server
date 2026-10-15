"""Cash refund lifecycle over authenticated production TCP and isolated MySQL."""
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
        print("PASS: missing cash purchase schema rejected before listening.")
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
        check(result.returncode == 0, 'Isolated refund SQL succeeded')
        return result.stdout.strip()

    check(sql('SELECT DATABASE();').casefold().startswith('bfrefundtests_'), 'Generated isolated refund schema only')
    identities = [int(sql("SELECT c.CharacterId FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username='refund_" + role + "';")) for role in ('owner','other')]
    scope = ','.join(map(str,identities))
    sql(f'DELETE FROM CashInventorySlots WHERE CharacterId IN ({scope}); DELETE FROM CashPurchases WHERE CharacterId IN ({scope}); DELETE FROM InventorySlots WHERE CharacterId IN ({scope}); UPDATE Characters SET Cash=2000000 WHERE CharacterId IN ({scope});')

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
                    raise RuntimeError('Refund server exited before listening')
                if line.startswith('Listening on 127.0.0.1:'):
                    events.append({'event': 'server-started', 'pid': server.pid})
                    return server, int(line.rsplit(':', 1)[1])
            raise RuntimeError('Missing refund listener')

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
                peers.append(self)
                check(self.exact(258) == b'\x04\0' + bytes(256), 'Original plaintext greeting')

            def exact(self, length):
                result = b''
                while len(result) < length:
                    part = self.socket.recv(length - len(result))
                    if not part:
                        raise RuntimeError('Unexpected refund peer EOF')
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
                    check(xor(header) == 0 and length <= 1048576, 'Bounded checksummed header')
                    body = self.incoming.transform(self.exact(length), -1)
                    check(xor(body) == header[6], 'Checksummed payload')
                    if mid == 4:
                        self.send(41); continue
                    check(mid == wanted, f'Refund expected {wanted}, received {mid}')
                    return body

            def close(self):
                self.socket.close()

            def closed(self):
                self.socket.settimeout(10)
                while True:
                    first = self.socket.recv(1)
                    if not first:
                        check(True, 'Unauthorized refund request closes connection'); return
                    header = self.incoming.transform(first + self.exact(7), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    check(xor(header) == 0 and length <= 1048576, 'Bounded rejection header')
                    body = self.incoming.transform(self.exact(length), -1)
                    check(xor(body) == header[6] and mid == 4, 'Only preceding heartbeat before rejection')

            def login(self, role):
                ticket = args.database_config.parent / f'refund_{role}_{len(tickets)}.ticket'
                tickets.append(ticket)
                result = subprocess.run([str(args.executable), '--login', 'refund_' + role,
                    '--database-config', str(args.database_config), '--ticket-file', str(ticket)],
                    input='Synthetic-refund-password\n', capture_output=True, text=True, timeout=15,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                check(result.returncode == 0, 'Private authenticated ticket issued')
                secret = ticket.read_text(encoding='utf-8').strip(); secrets.append(secret)
                check(secret.startswith('BFS1.') and len(secret) == 69, 'Private ticket shape')
                self.send(1, struct.pack('<I', 1256) + bytes(92)); self.receive(2); self.receive(17)
                self.send(3, text(secret) + text('') + text('') + bytes(12) + text(''))
                self.receive(3)
                role_index = ('owner', 'other').index(role)
                name = ('RefundOwner', 'RefundOther')[role_index]
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


        def cash_snapshot(body):
            reader = Reader(body)
            count = reader.u32()
            check(count <= 1024, 'Bounded pending cash snapshot')
            slots = []
            for _ in range(count):
                slot, unknown = reader.u32(), reader.u32()
                quantity, days = struct.unpack('<HH', reader.take(4))
                stock, item, note, flag = reader.u32(), reader.u32(), reader.text(), reader.u8()
                check(unknown == 0 and note == '' and flag in (0,1), 'Known cash slot baseline and explicit refund flag')
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

        try:
            server,port = start()
            unauthenticated = Peer(port)
            unauthenticated.send(817,struct.pack('<II',0,1)); unauthenticated.closed()
            alice = Peer(port); alice.login('owner')
            bob = Peer(port); bob.login('other')
            check(alice.cash_slots == [] and bob.cash_slots == [], 'Initial owned cash warehouses empty')
            identity, slots = buy(alice,1010816)
            check([s['quantity'] for s in slots] == [65535,34465], 'Original two-part purchase quantity')
            check(all(s['flag']==1 for s in slots), 'Complete purchase exposes both cancellation entries')
            check(refund(bob,identity,2) == [], 'Foreign slot rejected without disclosure')
            balance(bob,2000000)
            check(len(refund(alice,identity,2,request_text='not a credential')) == 2, 'Unknown text never authorizes a refund')
            check(len(refund(alice,0,2)) == 2, 'Unknown ID preserves owned purchase')
            check(refund(alice,identity+1,1,2000000) == [], 'Tail slot cancels entire unclaimed purchase')
            check(refund(alice,identity,2) == [], 'Duplicate request cannot credit twice')
            identity,slots = buy(alice,1010816)
            alice.send(829,struct.pack('<I',identity+1))
            alice.receive(802);alice.receive(806)
            pickup = Reader(alice.receive(835))
            check((pickup.u32(),pickup.u32(),pickup.u32(),pickup.text()) == (identity+1,1,0,''), 'Original pickup response after commit'); pickup.end()
            remaining = cash_snapshot(alice.receive(810))
            check(len(remaining)==1 and remaining[0]['flag']==0, 'Claiming one part removes all refund eligibility')
            check(refund(alice,identity,2)==remaining, 'Cannot refund paid part after claiming free-price tail')
            balance(alice,1000001)
            pending,slots = buy(alice,1000139)
            alice.close();bob.close();stop(server)
            server,port = start()
            alice = Peer(port);alice.login('owner')
            check(any(s['slot']==pending and s['flag']==1 for s in alice.cash_slots), 'Restart restores purchase ownership')
            check(any(s['slot']==identity and s['flag']==0 for s in alice.cash_slots), 'Restart preserves claimed-state refund denial')
            check(len(refund(alice,pending,1,1000001))==1, 'Unclaimed purchase refunded after server restart')
            pending,_ = buy(alice,1000139)
            alice.send(817,struct.pack('<II',0,pending));alice.close()
            deadline = time.monotonic()+8
            while sql(f'SELECT PurchaseState FROM CashPurchases WHERE PurchaseId={pending};') != '2':
                check(time.monotonic()<deadline,'Closed connection refund reached committed receipt')
                time.sleep(0.05)
            alice = Peer(port);alice.login('owner')
            balance(alice,1000001)
            check(all(s['slot']!=pending for s in alice.cash_slots), 'Lost response reload cannot restore a refunded slot')
            check(len(refund(alice,pending,2))==1,'Lost response replay cannot duplicate cash')
            expired,_ = buy(alice,1000139)
            observed=int(time.time())-604801
            sql(f'UPDATE CashPurchases SET PurchasedAtUnixSeconds={observed} WHERE PurchaseId={expired}; UPDATE CashInventorySlots SET PurchasedAtUnixSeconds={observed} WHERE SlotId={expired};')
            expired_slots=refund(alice,expired,4)
            check(any(s['slot']==expired and s['flag']==0 for s in expired_slots),'Expired purchase kept claimable with refund hidden')
            balance(alice,999001)
            events.append(dict(event='cash-refund-passed',split_purchase=True,partial_claim_denied=True,foreign_denied=True,restart=True,lost_response=True,duplicate_denied=True,expiry=True))
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
    print(f'PASS: cash refund authenticated TCP, {checks} checks; split purchases, refunds, claim exclusion, restart and replay.')


if __name__ == '__main__':
    main()
