"""Natural two-player settlement: immutable receipt versus current S105."""
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
from tcp_fixture import Cipher, frame, xor, text, verify_wire, host_create_payload, host_start_payload

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('executable','database-config','mysql','report'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
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
    check(sql('SELECT DATABASE();').lower().startswith('bfprofiletests_'),'Generated schema only')
    verify_wire()
    scope="SELECT c.CharacterId FROM Characters c JOIN Accounts a ON a.AccountId=c.AccountId WHERE a.Username IN ('profile_0_0','profile_0_1')"
    sql('UPDATE Characters SET Level=1,Experience=100,Lucci=7,Cash=3 WHERE CharacterId IN (SELECT t.CharacterId FROM ('+scope+') t);')
    old_receipts=sql('SELECT HEX(MatchId),SHA2(ResultData,256) FROM MatchSettlements ORDER BY MatchId;')
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
                ticket=args.database_config.parent/f'profile_{index}_{len(tickets)}.ticket';tickets.append(ticket)
                result=subprocess.run([str(args.executable),'--login',f'profile_0_{index}','--database-config',str(args.database_config),'--ticket-file',str(ticket)],
                    input='Synthetic-profile-password\n',capture_output=True,text=True,timeout=15,creationflags=flags)
                check(result.returncode==0,'Authenticated ticket')
                secret=ticket.read_text('utf-8').strip();secrets.append(secret)
                self.send(1,struct.pack('<I',1256)+bytes(92));self.receive(2);self.receive(17)
                self.send(3,text(secret)+text('')+text('')+bytes(12)+text(''))
                self.receive(3);self.receive(201)
                self.send(521)
                profile=self.receive(105)
                check(struct.unpack_from('<8I',profile)[1:6]==expected,'Current login statistics')
                for mid in (117,806,837,802,810,170): self.receive(mid)
                self.receive(201)
                self.send(106);self.receive(117)
        try:
            server,port=start()
            host,guest=Peer(port),Peer(port)
            for i,p in enumerate((host,guest)): p.login(i,(100,0,1000,7,3))
            host.send(402,host_create_payload());host.receive(102)
            room=struct.unpack_from('<H',host.receive(117))[0]
            guest.send(403,struct.pack('<I',room)+text(''));guest.receive(103);guest.receive(117)
            guest.send(505,struct.pack('<III',1,0,0))
            guest.receive(503)
            host.send(503,host_start_payload())
            for p in (host,guest): p.receive(585);p.receive(510)
            for p in (host,guest): p.send(578,bytes(16));p.receive(606)
            for p in (host,guest): p.receive(607)
            starts=[p.receive(608) for p in (host,guest)]
            check(starts[0]==starts[1],'Shared start')
            duration,seed=struct.unpack('<II',starts[0])
            check(duration==240000 and seed>0,'Original map duration')
            events.append(dict(event='natural-game-start',duration=duration))
            # Independent receive threads keep both authenticated heartbeat paths
            # active during the unmodified four-minute map.
            failures=[]
            results=[None,None]
            def finish(index,peer):
                try:
                    peer.receive(612,timeout=duration/1000+30)
                    peer.receive(613,timeout=20)
                    score=peer.receive(614)
                    profile=peer.receive(105)
                    results[index]=(score,profile)
                except BaseException as error: failures.append(error)
            threads=[threading.Thread(target=finish,args=(i,p)) for i,p in enumerate((host,guest))]
            for t in threads:t.start()
            for t in threads:t.join(timeout=duration/1000+60)
            check(all(not t.is_alive() for t in threads) and not failures,'Natural result received')
            check(results[0][0]==results[1][0],'Shared historical S614')
            for _,profile in results:
                check(struct.unpack_from('<8I',profile)[1:6]==(200,0,1000,5,1),'S105 uses post-receipt current wallet')
            rows=sql('SELECT Experience,Lucci,Cash FROM Characters WHERE CharacterId IN ('+scope+') ORDER BY CharacterId;')
            check(rows.splitlines()==['200\t5\t1']*2,'Durable current wallet')
            all_receipts=sql('SELECT HEX(MatchId),SHA2(ResultData,256) FROM MatchSettlements ORDER BY MatchId;')
            check(set(old_receipts.splitlines())<set(all_receipts.splitlines()),'Old receipts unchanged plus one natural result')
            latest=sql('SELECT HEX(ResultData) FROM MatchSettlements ORDER BY CommittedAt DESC LIMIT 1;')
            raw=bytes.fromhex(latest);version,count=struct.unpack_from('<II',raw)
            check((version,count)==(1,2),'Existing V1 receipt format')
            at=8
            for _ in range(count):
                at+=20
                units=struct.unpack_from('<I',raw,at)[0];at+=4+units*2+8
                before=struct.unpack_from('<4I',raw,at);after=struct.unpack_from('<4I',raw,at+16);at+=32
                check(before==(1,100,7,3) and after==(1,200,57,3),'Receipt keeps earned rewards and original wallet')
            check(at==len(raw),'Full receipt parsed')
            for p in peers:p.s.close()
            stop(server)
            server,port=start()
            for i in range(2):
                p=Peer(port);p.login(i,(200,0,1000,5,1))
            check(sql('SELECT HEX(MatchId),SHA2(ResultData,256) FROM MatchSettlements ORDER BY MatchId;')==all_receipts,'Restart preserves exact receipts')
            events.append(dict(event='natural-result-current-profile-reconnect',players=2))
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
                fixture='Isolated trigger changes wallet after receipt encoding; not an original-server algorithm or real race.')
            args.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(f'PASS: natural settlement profile TCP {checks} checks')
if __name__=='__main__':
    main()

