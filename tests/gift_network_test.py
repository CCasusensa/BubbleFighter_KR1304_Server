"""Gift lifecycle over authenticated production TCP and isolated MySQL."""
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
        print("PASS: missing gift schema rejected before listening.")
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
        check(result.returncode == 0, 'Isolated gift SQL succeeded')
        return result.stdout.strip()

    check(sql('SELECT DATABASE();').casefold().startswith('bfgifttests_'), 'Generated isolated gift schema only')
    identities = [int(sql("SELECT c.CharacterId FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username='gift_" + role + "';")) for role in ('owner','other')]
    scope = ','.join(map(str,identities))
    sql(f'DELETE p FROM GiftBundleComponents p JOIN Gifts g ON g.GiftId=p.GiftId WHERE g.SenderCharacterId IN ({scope}) OR g.RecipientCharacterId IN ({scope}); DELETE p FROM GiftBundles p JOIN Gifts g ON g.GiftId=p.GiftId WHERE g.SenderCharacterId IN ({scope}) OR g.RecipientCharacterId IN ({scope}); DELETE FROM Gifts WHERE SenderCharacterId IN ({scope}) OR RecipientCharacterId IN ({scope}); DELETE FROM CashInventorySlots WHERE CharacterId IN ({scope}); DELETE FROM CashPurchases WHERE CharacterId IN ({scope}); DELETE FROM InventorySlots WHERE CharacterId IN ({scope}); UPDATE Characters SET Cash=2000000,Lucci=10000 WHERE CharacterId IN ({scope});')

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
                    raise RuntimeError('Gift server exited before listening')
                if line.startswith('Listening on 127.0.0.1:'):
                    events.append({'event': 'server-started', 'pid': server.pid})
                    return server, int(line.rsplit(':', 1)[1])
            raise RuntimeError('Missing gift listener')

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
                        raise RuntimeError('Unexpected gift peer EOF')
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
                    if mid == 819 and wanted != 819:
                        notice = struct.unpack('<II', body)
                        check(notice[0] == 0 and notice[1] <= 200, 'Bounded persisted pending count')
                        self.notices.append(notice[1]); continue
                    if mid == 201 and wanted != 201:
                        check(len(body) > 32, 'Asynchronous authenticated appearance update')
                        continue
                    check(mid == wanted, f'Gift expected {wanted}, received {mid}')
                    return body

            def close(self):
                self.socket.close()

            def closed(self):
                self.socket.settimeout(10)
                while True:
                    first = self.socket.recv(1)
                    if not first:
                        check(True, 'Unauthorized gift request closes connection'); return
                    header = self.incoming.transform(first + self.exact(7), -1)
                    length, mid = struct.unpack_from('<IH', header)
                    check(xor(header) == 0 and length <= 1048576, 'Bounded rejection header')
                    body = self.incoming.transform(self.exact(length), -1)
                    check(xor(body) == header[6] and mid == 4, 'Only preceding heartbeat before rejection')

            def login(self, role):
                ticket = args.database_config.parent / f'gift_{role}_{len(tickets)}.ticket'
                tickets.append(ticket)
                result = subprocess.run([str(args.executable), '--login', 'gift_' + role,
                    '--database-config', str(args.database_config), '--ticket-file', str(ticket)],
                    input='Synthetic-gift-password\n', capture_output=True, text=True, timeout=15,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                check(result.returncode == 0, 'Private authenticated ticket issued')
                secret = ticket.read_text(encoding='utf-8').strip(); secrets.append(secret)
                check(secret.startswith('BFS1.') and len(secret) == 69, 'Private ticket shape')
                self.send(1, struct.pack('<I', 1256) + bytes(92)); self.receive(2); self.receive(17)
                self.send(3, text(secret) + text('') + text('') + bytes(12) + text(''))
                self.receive(3)
                role_index = ('owner', 'other').index(role)
                name = ('GiftOwner', 'GiftOther')[role_index]
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
                check(unknown == 0 and note == '' and flag in (0,1), 'Known cash slot baseline and explicit gift flag')
                slots.append(dict(slot=slot,quantity=quantity,days=days,stock=stock,item=item,flag=flag))
            reader.end()
            return slots


        def send_gift(peer, name='GiftOther', stock=1000139, expected=1, balance=None, cash=True):
            peer.send(818,text(name)+text('')+struct.pack('<I',stock)+text('Enjoy'))
            if expected==1:
                peer.receive(105)
                if cash:
                    check(struct.unpack('<III',peer.receive(834))==(0,balance,0),'Gift committed cash balance')
                else:
                    check(int(sql(f'SELECT Lucci FROM Characters WHERE CharacterId={peer.character};'))==balance,'Gift committed Lucci balance')
            reader=Reader(peer.receive(825))
            check((reader.text(),reader.u32(),reader.u32(),reader.text(),reader.text())==
                  ('',0,expected & 0xffffffff,'',''),'Native gift send result and opaque baseline')
            reader.end()
            if expected==1:
                return int(sql(f'SELECT MAX(GiftId) FROM Gifts WHERE SenderCharacterId={identities[0]};'))

        def received(peer):
            peer.send(815,struct.pack('<II',0,4))
            reader=Reader(peer.receive(822))
            result,total,pending,count=[reader.u32() for _ in range(4)]
            check(result==1 and total<=200 and count<=4 and pending<=total,'Bounded gift inbox')
            rows=[]
            for _ in range(count):
                identity=reader.u32()
                date=struct.unpack('<d',reader.take(8))[0]
                sender=reader.u32();name=reader.text();stock=reader.u32()
                state=struct.unpack('<H',reader.take(2))[0]
                message=reader.text();expiry=struct.unpack('<d',reader.take(8))[0]
                check(expiry-date==30 and name=='GiftOwner' and sender==identities[0],
                      'Original OLE date and authenticated sender fields')
                check(stock in (1000139,2000001,2000105,2000107) and message=='Enjoy' and state in (0,1,2,3),'Saved gift terms')
                rows.append(dict(id=identity,state=state))
            reader.end()
            return total,pending,rows

        def sent(peer):
            peer.send(816,struct.pack('<II',0,4))
            reader=Reader(peer.receive(823))
            check(reader.u32()==1,'Sent list success')
            total,count=reader.u32(),reader.u32();rows=[]
            for _ in range(count):
                identity=reader.u32();date=struct.unpack('<d',reader.take(8))[0]
                opaque=reader.take(8);unknown=reader.u32();name=reader.text();stock=reader.u32()
                state,emotion=struct.unpack('<HH',reader.take(4))
                message,thanks=reader.text(),reader.text()
                check(opaque==bytes(8) and unknown==0 and name=='GiftOther' and stock in (1000139,2000001,2000105,2000107),
                      'Sent recipient and explicit unknown baseline')
                rows.append(dict(id=identity,state=state,emotion=emotion,thanks=thanks))
            reader.end();return total,rows

        def claim(peer, identity, success=True, pending=0, stock=1000139, bonus=False):
            peer.send(819,struct.pack('<I',identity))
            if success and bonus: peer.receive(105)
            if success: peer.receive(802);peer.receive(806)
            check(struct.unpack('<IIII',peer.receive(826))==
                  (0 if success else 0xffffffff,identity,stock if success else 0,pending),
                  'Native signed claim result and remaining count')

        def discard(peer,identity,status,pending):
            peer.send(820,struct.pack('<I',identity))
            check(struct.unpack('<III',peer.receive(827))==(status,identity,pending),'Gift discard result')

        def delete(peer,identity,received_side,status,pending=0):
            peer.send(825 if received_side else 824,struct.pack('<I',identity))
            data=struct.unpack('<III' if received_side else '<II',peer.receive(832 if received_side else 831))
            check(data==((status,identity,pending) if received_side else (status,identity)),'Owned gift record deletion')

        def thank(peer,identity,status=1,message='Thank you'):
            peer.send(821,struct.pack('<I',identity)+text(message)+struct.pack('<I',4))
            check(struct.unpack('<I',peer.receive(828))==(status,),'Thanks result')

        def view(peer,identity,success=True):
            peer.send(822,struct.pack('<I',identity))
            reader=Reader(peer.receive(829))
            flag,key=reader.u32(),reader.u32()
            name,message=reader.text(),reader.text()
            emotion,color=reader.u32(),reader.u32()
            model=reader.u32()
            groups=[[reader.u32() for _ in range(reader.u32())] for _ in range(3)]
            extra_bytes=reader.take(reader.u32())
            for _ in range(reader.u32()):
                reader.u32();reader.u32()
                for _ in range(reader.u32()):reader.u32()
                reader.u32();reader.text()
            reader.end()
            if success:
                check((flag,key,name,message,emotion,color,model)==
                      (1,identity,'GiftOther','Thank you',4,7,300010),'Durable thanks and native appearance')
                check(len(groups[0])>0,'Thanks has owned starter equipment')
            else:
                check((flag,key,name,message,emotion,color,model)==(0,0,'','',0,0,0),'No foreign thanks disclosure')

        def amount():
            return int(sql(f'SELECT COALESCE(SUM(Quantity),0) FROM InventorySlots WHERE CharacterId={identities[1]} AND ItemId=205034;'))

        try:
            server,port=start()
            bad=Peer(port);bad.send(815,struct.pack('<II',0,4));bad.closed()
            alice=Peer(port);alice.login('owner')
            bob=Peer(port);bob.login('other')
            check(received(bob)==(0,0,[]),'Initial empty inbox')
            first=send_gift(alice,balance=1999000)
            check(struct.unpack('<II',bob.receive(819))==(0,1),'Online new gift notification')
            check(received(bob)[2]==[dict(id=first,state=0)],'Pending delivery visible')
            claim(alice,first,False)
            delete(bob,first,True,2,1)
            delete(alice,first,False,1)
            check(sent(alice)==(0,[]),'Sender deletion hides only sender copy')
            claim(bob,first)
            check(amount()==100,'Claim grants ordinary inventory')
            claim(bob,first,False)
            discard(bob,first,2,0)
            check(amount()==100,'Replay and discard cannot grant twice')
            bob.close()
            second=send_gift(alice,balance=1998000)
            alice.close();stop(server)
            server,port=start()
            bob=Peer(port);bob.login('other')
            check(struct.unpack('<II',bob.receive(819))==(0,1),'Offline gift survives process restart')
            total,pending,items=received(bob)
            check(total==2 and pending==1 and items[0]==dict(id=second,state=0),'Persistent descending inbox')
            claim(bob,second)
            thank(bob,second);thank(bob,second,2)
            delete(bob,second,True,1)
            bob.close();stop(server)
            server,port=start()
            alice=Peer(port);alice.login('owner')
            total,items=sent(alice)
            check(total==1 and items==[dict(id=second,state=1,emotion=4,thanks='Thank you')],'Restart preserves sent status and thanks')
            view(alice,second)
            bob=Peer(port);bob.login('other');view(bob,second,False)
            third=send_gift(alice,balance=1997000)
            discard(bob,third,1,0);discard(bob,third,2,0);claim(bob,third,False)
            check(amount()==200,'Discard does not grant')
            fourth=send_gift(alice,balance=1996000)
            expired=int(time.time())-2592001
            sql(f'UPDATE Gifts SET SentAtUnixSeconds={expired} WHERE GiftId={fourth};')
            claim(bob,fourth,False);discard(bob,fourth,2,0);delete(bob,fourth,True,1)
            fifth=send_gift(alice,balance=1995000)
            bob.send(819,struct.pack('<I',fifth));bob.close()
            deadline=time.monotonic()+8
            while sql(f'SELECT GiftState FROM Gifts WHERE GiftId={fifth};')!='1':
                check(time.monotonic()<deadline,'Lost response claim committed');time.sleep(.05)
            bob=Peer(port);bob.login('other')
            claim(bob,fifth,False)
            check(amount()==300,'Lost response replay preserves one grant')
            check(sql('SELECT COUNT(*) FROM CashPurchases;')=='0' and
                  sql('SELECT COUNT(*) FROM CashInventorySlots;')=='0','Received gifts never become refundable purchases')
            send_gift(alice,'GiftOwner',expected=-18)
            send_gift(alice,'Absent',expected=-1)
            # Native C818 also carries StockSet identities. No invented bundle
            # envelope, cash warehouse receipt or refund ownership is involved.
            send_gift(alice,stock=2000812,expected=-15)
            send_gift(alice,stock=2003269,expected=-15)
            bob.close()
            bonus_gift=send_gift(alice,stock=2000001,balance=1990500)
            send_gift(alice,stock=2000001,expected=-15)  # Count-cap overflow uses existing limit refusal.
            check(sql(f'SELECT Lucci FROM Characters WHERE CharacterId={identities[1]};')=='10000','Bundle bonus waits for claim')
            alice.close();stop(server)
            server,port=start()
            alice=Peer(port);alice.login('owner')
            bob=Peer(port);bob.login('other')
            check(struct.unpack('<II',bob.receive(819))==(0,1),'Offline bundle notice survives restart')
            check(received(bob)[2][0]==dict(id=bonus_gift,state=0),'Native received record retains StockSet identity')
            check(sent(alice)[1][0]['id']==bonus_gift,'Native sent record retains StockSet identity')
            for stock in (1000239,2000001):
                sequence=sql('SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1;')
                cash_before=sql(f'SELECT Cash FROM Characters WHERE CharacterId={identities[1]};')
                bob.send(809,struct.pack('<II',stock,0))
                purchase=Reader(bob.receive(816))
                check((purchase.u32(),purchase.u32(),purchase.text(),purchase.u8(),purchase.u32(),purchase.u32(),purchase.u32())==(stock,0xffffff38,'',0,0,0,0),'Pending gift excludes conflicting ordinary/bundle purchase via native refusal')
                purchase.end()
                check(sql('SELECT NextSlotId FROM CashInventorySequence WHERE SequenceId=1;')==sequence and sql(f'SELECT Cash FROM Characters WHERE CharacterId={identities[1]};')==cash_before,'Capacity refusal preserves balance and allocation sequence')
            claim(alice,bonus_gift,False)
            claim(bob,bonus_gift,stock=2000001,bonus=True)
            check(sql(f'SELECT Quantity FROM InventorySlots WHERE CharacterId={identities[1]} AND ItemId=100081;')=='100','Bundle count component delivered')
            check(sql(f'SELECT Lucci FROM Characters WHERE CharacterId={identities[1]};')=='70000','Bundle bonus credited once')
            claim(bob,bonus_gift,False)
            thank(bob,bonus_gift);view(alice,bonus_gift)
            multiple=send_gift(alice,stock=2000105,balance=1986100)
            bob.send(819,struct.pack('<I',multiple));bob.close()
            deadline=time.monotonic()+8
            while sql(f'SELECT GiftState FROM Gifts WHERE GiftId={multiple};')!='1':
                check(time.monotonic()<deadline,'Lost bundle response committed');time.sleep(.05)
            alice.close();stop(server)
            server,port=start()
            alice=Peer(port);alice.login('owner')
            bob=Peer(port);bob.login('other')
            claim(bob,multiple,False)
            check(sql(f'SELECT COUNT(*) FROM InventorySlots WHERE CharacterId={identities[1]} AND ItemId IN (201074,100111,100241,300007);')=='4','All four components survive restart and replay')
            check(sql(f'SELECT COUNT(*) FROM InventorySlots s JOIN Gifts g ON g.GiftId={multiple} WHERE s.CharacterId={identities[1]} AND s.ItemId IN (100111,100241,300007) AND s.ExpiresAtUnixSeconds=g.HandledAtUnixSeconds+604800;')=='3','Duration begins at committed claim time')
            lucci_gift=send_gift(alice,stock=2000107,cash=False,balance=5500)
            claim(bob,lucci_gift,stock=2000107)
            check(sql(f'SELECT COUNT(*) FROM InventorySlots WHERE CharacterId={identities[1]} AND ItemId BETWEEN 207029 AND 207036;')=='8','Lucci bundle gifts all eight components')
            check(bob.cash_slots==[] and sql('SELECT COUNT(*) FROM CashPurchases;')=='0' and sql('SELECT COUNT(*) FROM CashBundles;')=='0','Bundle gifts never enter refundable cash warehouse')
            bob.send(817,text('')+struct.pack('<I',bonus_gift))
            refund=Reader(bob.receive(824))
            check(refund.u32()==bonus_gift and refund.u32()!=1 and refund.text()=='','Gift identity cannot claim a cash refund')
            refund.end()
            check(cash_snapshot(bob.receive(810))==[],'Refund rejection retains empty cash warehouse')
            events.append(dict(event='bundle-gifts-passed',stocks=[2000001,2000105,2000107],cash=True,lucci=True,bonus=True,all_components=True,offline=True,restart=True,lost_response=True,replay=True,no_refundable_receipts=True))
            sql(f'UPDATE Characters SET Cash=0 WHERE CharacterId={identities[0]};')
            send_gift(alice,expected=27)
            bob.send(819,b'\x01\x02');bob.closed()
            events.append(dict(event='gifts-passed',offline=True,restart=True,thanks=True,
                               owner_hiding=True,claim_replay=True,discard=True,expiry=True,
                               lost_response=True,foreign_rejection=True,malformed_rejection=True))
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
    print(f'PASS: gifts authenticated TCP, {checks} checks; claim/drop, thanks, restart, expiry and replay.')


if __name__ == '__main__':
    main()
