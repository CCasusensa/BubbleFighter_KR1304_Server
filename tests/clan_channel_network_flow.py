"""Trusted category80 entry, whole-party handoff and resident loss on real TCP."""
import struct
import time

from invitation_network_flow import ask
from party_clan_identity_network_flow import NAMES, observe
from tcp_fixture import text, xor


def move(games, category, Peer, port, check, secrets):
    pending = []
    for game in games:
        game.send(65020, struct.pack('<I', 1))
    games[0].send(53, struct.pack('<I', category))
    try:
        for game in games:
            grant = game.receive(65021)
            check(len(grant) == 44 and struct.unpack_from('<III', grant) == (1, game.character, category),
                  'Clan channel grant binds each authenticated participant')
            secrets.append(grant[12:].hex())
            check(game.receive(8) == struct.pack('<I4sHI', 0, bytes([127,0,0,1]), port, category),
                  'Native clan channel success gives the configured endpoint')
            destination = Peer(port)
            destination.role, destination.character = game.role, game.character
            destination.party_equipped = game.party_equipped
            destination.party_members = game.party_members if hasattr(game, 'party_members') else [game]
            proof = struct.pack('<II', 1, game.character) + grant[12:]
            destination.send(65022, proof)
            destination.send(54, struct.pack('<II', game.character, 1))
            pending.append((game, destination, proof))
        for _, destination, _ in pending:
            check(destination.receive(9) == b'', 'Every clan channel participant passes native entry barrier')
        for _, destination, _ in pending:
            destination.send(55)
        scenes = []
        for game, destination, proof in pending:
            check(destination.receive(10) == b'', 'Clan channel retires the previous connection')
            check(destination.receive(11) == struct.pack('<III',1,category,0), 'Native S11 commits requested clan category')
            snapshot = destination.receive(117)
            check(len(snapshot) > 100, 'Clan channel destination has a complete plaza snapshot')
            scenes.append(struct.unpack_from('<H', snapshot)[0])
            game.socket, destination.socket = destination.socket, game.socket
            game.incoming, destination.incoming = destination.incoming, game.incoming
            game.outgoing, destination.outgoing = destination.outgoing, game.outgoing
            destination.closed()
            replay = Peer(port)
            replay.send(65022, proof); replay.closed(); replay.socket.close()
        check(len(set(scenes)) == 1, 'Complete clan party is admitted into one destination plaza')
    finally:
        for _, destination, _ in pending:
            destination.socket.close()


def exercise(host, guest, observer, active, guest_clan, Peer, port, sql, check, z, secrets):
    games = [host,guest,observer]
    for game in games:
        game.issue()
        game.send(65020,struct.pack('<I',1))
    observer.send(53,struct.pack('<I',80))
    check(observer.receive(8) == bytes.fromhex('0300000000000000000050000000'),
          'Pending applicant cannot anchor clan channel: native result3')
    observer.send(2025,struct.pack('<III',9000,0,101))
    observer.send(53,struct.pack('<I',80))
    check(observer.receive(8) == bytes.fromhex('0300000000000000000050000000'),
          'Forged main clan report cannot authorize category80')
    for joining in (guest,observer):
        time.sleep(3.1)
        _, accept = ask(host,joining,NAMES['host'],NAMES[joining.role],check,lambda *a,**k:None,text)
        current = games[:2] if joining is guest else games
        for peer in current: peer.party_members = current
        joining.send(115,accept); joining.receive(117)
        for peer in current:
            peer.receive(201)
            check(peer.receive(206) == text(NAMES[joining.role]), 'Native consent forms complete clan-channel party')
    move(games,80,Peer,port,check,secrets)
    for game in games: game.issue()
    # Pending observer is the one configured mercenary; this is not a clan battle.
    for mid, body in ((1036,b''),(1108,struct.pack('<I',0xffffffec))):
        guest_clan.send(mid,body)
        check(struct.unpack_from('<BII',guest_clan.receive(1016)) == (1,9000,100),
              'Manual clan exit is refused with current canonical membership in80')
    active.send(1029,struct.pack('<I',1))
    check(active.receive(1030) == struct.pack('<II',0,1), 'Clan channel plaza cannot authorize disband')
    check(sql(f'SELECT ClanId,MembershipStatus FROM ClanMembers WHERE CharacterId={guest.character};') == '9000\t100',
          'Refused native leave commands do not alter persistent membership')
    check(sql('SELECT DisbandAtUnixSeconds FROM Clans WHERE ClanId=9000;') == '0',
          'Clan channel disband refusal has no persistent write')
    active.send(1015)
    check(struct.unpack_from('<BII',active.receive(1016)) == (1,9000,101), 'No invented refusal packet remains queued')
    move(games,60,Peer,port,check,secrets)
    # Restore the existing isolated suite's solo/menu preconditions exactly.
    observer.party_members=[observer]
    host.party_members=guest.party_members=[host,guest]
    observer.send(202); observer.receive(201)
    check(observer.receive(204)==b'', 'Mercenary leaves the restored ordinary party')
    for peer in (host,guest):
        peer.receive(201); check(peer.receive(205)==text(NAMES['observer']), 'Complete mercenary departure publication')
    guest.party_members=[guest]; host.party_members=[host]
    guest.send(202); guest.receive(201)
    check(guest.receive(204)==b'', 'Member returns to an ordinary solo party')
    host.receive(201); check(host.receive(205)==text(NAMES['guest']), 'Leader returns to an ordinary solo party')
    for i, game in enumerate(games):
        game.send(105)
        for mid in (105,117,806,837,802,810): game.receive(mid)
        for remaining in games[i+1:]:
            remaining.receive(511); remaining.receive(501)
    for game in games: game.issue()
    return 8  # Two native refusals, six participant cross-category handoffs.


def resident_loss(game, Peer, port, sql, check, secrets):
    game.send(521)
    for mid in (105,117,806,837,802,810,170,201): game.receive(mid)
    move([game],80,Peer,port,check,secrets)
    game.issue()
    sql(f'DELETE FROM ClanMembers WHERE CharacterId={game.character};')
    previous=game.socket.gettimeout()
    try:
        game.socket.settimeout(15)
        deadline=time.monotonic()+15
        cleared = False
        while True:
            try: first=game.socket.recv(1)
            except (ConnectionResetError,ConnectionAbortedError): first=b''
            if not first: break
            header=game.incoming.transform(first+game.exact(7),-1)
            length,mid=struct.unpack_from('<IH',header)
            check(xor(header)==0 and length<=1048576, 'Resident stream header remains bounded and valid')
            body=game.incoming.transform(game.exact(length),-1)
            check(xor(body)==header[6], 'Resident stream body checksum')
            if mid == 201:
                check(not cleared and observe(game,body,check)==[0],
                      'At most one exact absent-clan display update may precede residency revocation')
                cleared = True
            else:
                check(mid==4 and body==b'', 'Only native heartbeat or canonical clan clearing precedes revocation')
                game.send(41)
            check(time.monotonic()<deadline,'Resident revalidation has bounded delay')
        check(True,'External database membership loss closes the clan-channel main session')
    finally:
        game.socket.settimeout(previous)
        sql(f'INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES({game.character},9000,101);')
    return 2  # One solo handoff and its subsequent external revocation.
