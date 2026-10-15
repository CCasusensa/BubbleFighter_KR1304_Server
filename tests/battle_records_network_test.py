"""Authenticated reset cards, current counters and restart persistence."""
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
from social_network_flow import Reader
from tcp_fixture import Cipher, frame, xor, text, verify_wire, host_create_payload, host_start_payload

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('executable','database-config','mysql','report'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--expect-missing-schema',action='store_true')
    args=parser.parse_args()
    if args.expect_missing_schema:
        p=subprocess.run([str(args.executable),'--listen-port','0','--once','--database-config',str(args.database_config)],capture_output=True,timeout=15,creationflags=subprocess.CREATE_NO_WINDOW)
        passed=p.returncode==1 and b'1054' in p.stderr and b'Listening on' not in p.stdout
        args.report.write_text(json.dumps(dict(passed=passed,checks=1,server_exit=p.returncode,listened=b'Listening on' in p.stdout))+'\n',encoding='utf8')
        if not passed: raise RuntimeError('Battle schema guard failed')
        print('PASS: missing battle column rejected before listening')
        return
    checks=0
    peers=[]; servers=[]; tickets=[]; secrets=[]; events=[]
    passed=False
    flags=subprocess.CREATE_NO_WINDOW
    def check(ok,why):
        nonlocal checks
        checks+=1
        if not ok: raise RuntimeError(why)
    def sql(statement):
        result=subprocess.run([str(args.mysql),'--defaults-file='+str(args.database_config),'--no-login-paths','--batch','--skip-column-names'],
            input=statement,capture_output=True,text=True,timeout=15,creationflags=flags)
        check(result.returncode==0,'Isolated profile SQL')
        return result.stdout.strip()
    check(sql('SELECT DATABASE();').lower().startswith('bfbattletests_'),'Generated schema only')
    verify_wire()
    fixtures=[tuple(map(int,line.split())) for line in (Path(__file__).parent/'battle_records_cases.txt').read_text().splitlines() if line and not line.startswith('#')]
    initial={offset:value for offset,kind,value in fixtures}
    counters=dict(initial)
    player_ids=[int(sql("SELECT c.CharacterId FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username='battle_"+str(i)+"';")) for i in range(2)]
    old_receipts=sql('SELECT HEX(MatchId),SHA2(ResultData,256) FROM MatchSettlements ORDER BY MatchId;')
    old_wallet=sql('SELECT CharacterId,Level,Experience,Lucci,Cash FROM Characters ORDER BY CharacterId;')
    def profile(body, expected):
        r=Reader(body)
        r.take(32);r.text();r.take(2);r.u32();r.text()
        small=[r.u32() for _ in range(3)]
        actual={40:small[2]}
        check(r.u8()==0xba,'Absent optional block')
        for offset in range(80,296,4): actual[offset]=r.u32()
        check(r.u32()==0,'No opaque collection')
        r.take(8+4+1+4);check(r.u8()==0xba,'Absent byte optional')
        r.take(4+1+4+48)
        for offset in range(404,436,4): actual[offset]=r.u32()
        check(r.u32()==0,'No area ranks');r.take(12)
        check(r.u32()==0,'No escape ranks');r.text();r.u32()
        for offset in [464,472,468,476,480,484,488,492,496,500]:actual[offset]=r.u32()
        check(r.u32()==0,'No skill groups')
        for _ in range(r.u32()):r.take(6)
        r.take(12);check(r.u32()==0,'No opaque groups');r.end()
        check({key:actual[key] for key in expected}==expected,'Independent full native counter mapping')
        check(all(value==0 for key,value in actual.items() if key not in expected),'Unknown fields preserved at baseline')
    with args.report.with_suffix('.server.log').open('x',encoding='utf-8') as log:
        def start():
            server=subprocess.Popen([str(args.executable),'--listen-port','0','--database-config',str(args.database_config)],
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,creationflags=flags)
            lines=queue.Queue()
            def capture():
                for line in server.stdout:
                    log.write(line);log.flush();lines.put(line)
                lines.put(None)
            thread=threading.Thread(target=capture,daemon=True);thread.start()
            servers.append((server,thread))
            for _ in range(20):
                line=lines.get(timeout=10)
                if line is None: raise RuntimeError('Profile listener exited')
                if line.startswith('Listening on 127.0.0.1:'):
                    events.append(dict(event='started',pid=server.pid))
                    return server,int(line.rsplit(':',1)[1])
            raise RuntimeError('No profile listener')
        def stop(server):
            if server.poll() is None: server.terminate();server.wait(timeout=10)
            for process,thread in servers:
                if process is server: thread.join(timeout=3)
            events.append(dict(event='stopped',pid=server.pid))
        class Peer:
            def __init__(self,port):
                self.s=socket.create_connection(('127.0.0.1',port),timeout=10)
                self.out=Cipher(True);self.inc=Cipher(False);peers.append(self)
                check(self.exact(258)==b'\x04\0'+bytes(256),'Plain greeting')
            def exact(self,n):
                result=b''
                while len(result)<n:
                    data=self.s.recv(n-len(result))
                    if not data: raise RuntimeError('Profile peer EOF')
                    result+=data
                return result
            def send(self,mid,body=b''):
                self.s.sendall(self.out.transform(frame(mid,body),1))
            def receive(self,wanted,timeout=20):
                deadline=time.monotonic()+timeout
                while True:
                    self.s.settimeout(max(0.1,deadline-time.monotonic()))
                    header=self.inc.transform(self.exact(8),-1)
                    n,mid=struct.unpack_from('<IH',header)
                    check(xor(header)==0 and n<=1048576,'Bounded header')
                    body=self.inc.transform(self.exact(n),-1)
                    check(xor(body)==header[6],'Payload checksum')
                    if mid==4: self.send(41);continue
                    if mid in (201,819,501,503,511) and mid!=wanted: continue
                    check(mid==wanted,f'Expected {wanted}, received {mid}')
                    return body
            def login(self,index,expected):
                ticket=args.database_config.parent/f'battle_{index}_{len(tickets)}.ticket';tickets.append(ticket)
                result=subprocess.run([str(args.executable),'--login',f'battle_{index}','--database-config',str(args.database_config),'--ticket-file',str(ticket)],
                    input='Synthetic-battle-password\n',capture_output=True,text=True,timeout=15,creationflags=flags)
                check(result.returncode==0,'Authenticated ticket')
                secret=ticket.read_text('utf-8').strip();secrets.append(secret)
                self.send(1,struct.pack('<I',1256)+bytes(92));self.receive(2);self.receive(17)
                self.send(3,text(secret)+text('')+text('')+bytes(12)+text(''))
                self.receive(3);self.receive(201)
                self.send(521)
                profile(self.receive(105),expected)
                for mid in (117,806,837,802,810,170): self.receive(mid)
                self.receive(201)

        def barrier(peer):
            peer.send(828);peer.receive(834)
        def use(peer,item,result):
            peer.send(806,struct.pack('<III',item,1,0))
            if result==0:
                kind=item-100089
                for offset,scope,value in fixtures:
                    if scope==kind:counters[offset]=0
                profile(peer.receive(105),counters)
            check(peer.receive(813)==struct.pack('<III',result,item,0),'Native S813 exact result/item/empty args')
            if result==0:peer.receive(802)
            barrier(peer)
        def quantity(item):
            return int(sql(f'SELECT Quantity FROM InventorySlots WHERE CharacterId={player_ids[0]} AND ItemId={item};'))
        try:
            server,port=start()
            peer=Peer(port);peer.login(0,counters)
            check(quantity(100090)==2 and quantity(100091)==2,'Initial owned cards')
            use(peer,100090,0);check(quantity(100090)==1,'Consumed one result card')
            use(peer,100090,9);check(quantity(100090)==1,'Already-zero retry retains card')
            use(peer,100091,0);check(quantity(100091)==1,'Consumed one escape card')
            use(peer,100091,9);check(quantity(100091)==1,'Already-zero escape retry retains card')
            # A second account is unaffected; scene entry remains usable.
            other=Peer(port);other.login(1,initial)
            peer.send(106);peer.receive(117)
            peer.send(402,host_create_payload());peer.receive(102);peer.receive(117)
            use(peer,100090,9)
            for p in peers:p.s.close()
            stop(server)
            server,port=start()
            peer=Peer(port);peer.login(0,counters)
            use(peer,100090,9);use(peer,100091,9)
            check(sql('SELECT CharacterId,Level,Experience,Lucci,Cash FROM Characters ORDER BY CharacterId;')==old_wallet,'Reset preserves all wallets and levels')
            check(sql('SELECT HEX(MatchId),SHA2(ResultData,256) FROM MatchSettlements ORDER BY MatchId;')==old_receipts,'Reset/restart preserve immutable historical receipts')
            raw=bytes.fromhex(sql(f'SELECT HEX(BattleRecords) FROM Characters WHERE CharacterId={player_ids[0]};'))
            check(struct.unpack('<44I',raw)==(1,)+tuple(counters[offset] for offset,_,_ in fixtures),'Persistent version and every independent field')
            # C806 before authentication cannot mutate another player.
            stranger=Peer(port);stranger.send(806,struct.pack('<III',100090,1,0));stranger.s.settimeout(10)
            check(stranger.s.recv(1)==b'','Unauthenticated C806 rejected')
            check(quantity(100090)==1 and quantity(100091)==1,'Only committed successes consumed cards')
            peer.send(106);peer.receive(117)
            peer.send(402,host_create_payload());peer.receive(102);peer.receive(117)
            # Waiting-room use commits while its response is deliberately left unread.
            values=[counters[offset] for offset,_,_ in fixtures]
            values[1]=1
            saved=struct.pack('<44I',1,*values).hex()
            sql(f"UPDATE Characters SET BattleRecords=0x{saved} WHERE CharacterId={player_ids[0]};")
            peer.send(806,struct.pack('<III',100090,1,0))
            deadline=time.monotonic()+10
            while quantity(100090)!=0 and time.monotonic()<deadline:time.sleep(0.05)
            check(quantity(100090)==0,'Unobserved success consumed last card')
            peer.s.close()
            stop(server);server,port=start()
            peer=Peer(port);peer.login(0,counters)
            peer.send(806,struct.pack('<III',100090,1,0));peer.s.settimeout(10)
            check(peer.s.recv(1)==b'','Retry cannot consume an absent card')
            check(quantity(100090)==0,'Lost response/reconnect did not consume twice')
            events.append(dict(event='reset-both-cards-retry-owner-separation-room-restart',players=2))
            passed=True
        finally:
            for p in peers:
                try:p.s.close()
                except OSError:pass
            for server,_ in servers:stop(server)
            for ticket in tickets:
                assert ticket.parent.resolve()==args.database_config.parent.resolve()
                ticket.unlink(missing_ok=True)
            log.flush()
            check(not any(s in args.report.with_suffix('.server.log').read_text('utf-8') for s in secrets),'No ticket in server log')
            report=dict(passed=passed,checks=checks,events=events,original_ui=False,
                executable_sha256=hashlib.sha256(args.executable.read_bytes()).hexdigest(),
                server_stopped=all(s.poll() is not None for s,_ in servers),
                ticket_files_removed=all(not t.exists() for t in tickets),
                fixture='Independent native-offset/reset-policy fixture; synthetic authenticated peers, no original UI.')
            args.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(f'PASS: battle records TCP {checks} checks')
if __name__=='__main__':
    main()

