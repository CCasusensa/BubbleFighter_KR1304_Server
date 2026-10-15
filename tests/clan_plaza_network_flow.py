"""Private C2028 scenes through real production TCP and isolated MySQL."""
from collections import Counter
import struct
import time

from invitation_network_flow import ask, failure
from party_clan_identity_network_flow import NAMES, observe
from social_network_flow import Reader
from tcp_fixture import host_create_payload, text, xor


def descriptor(body, kind, count, check):
    r = Reader(body)
    scene = struct.unpack('<H', r.take(2))[0]
    r.u32(); r.text()
    occupancy = struct.unpack('<HHH', r.take(6))
    r.u32(); r.text()
    check(r.u32() == kind, 'S117 selects the native private/ordinary/menu scene type')
    # Independent descriptor walker; offsets below follow the frozen native
    # descriptor fixtures, not C++ structure layout. Config strings are variable.
    r.take(34)
    for _ in range(4): r.text()
    r.u32()
    for _ in range(6): r.text()
    r.take(12)
    r.take(r.u32() * 8)
    r.u32()
    for _ in range(r.u32()): r.u32(); r.text()
    r.take(20)
    for _ in range(r.u32()): r.take(2); r.take(r.u32() * 4)
    r.take(17)
    r.text(); r.text(); r.u32()
    r.take(12)
    wide = struct.unpack('<Q', r.take(8))[0]
    name = r.text()
    r.take(18)
    if kind == 6:
        check((name, wide, occupancy) == ('NetClan', 0xffffffffffffffff, (count, 0, 16)),
              'Canonical private name, preserved unknown sentinel and trusted occupancy')
    check(len(body) > r.at + 100, 'S117 retains full player/equipment data after descriptor')
    return scene


def packet(game, check):
    while True:
        header = game.incoming.transform(game.exact(8), -1)
        length, mid = struct.unpack_from('<IH', header)
        check(xor(header) == 0 and length <= 1048576, 'Private scene bounded native main header')
        body = game.incoming.transform(game.exact(length), -1)
        check(xor(body) == header[6], 'Private scene native body checksum')
        if mid == 4:
            game.send(41)
            continue
        if mid == 201: observe(game, body, check)
        return mid, body


def batch(game, ids, check):
    remaining, result, order = Counter(ids), {}, []
    while remaining:
        mid, body = packet(game, check)
        check(remaining[mid] > 0, f'Expected private scene batch {dict(remaining)}, received {mid}')
        result.setdefault(mid, []).append(body)
        order.append(mid)
        remaining[mid] -= 1
        if remaining[mid] == 0: del remaining[mid]
    # Party and scene queues have independent publication schedules. Preserve
    # each queue's causal order without imposing an unsupported merged order.
    if 201 in ids and 206 in ids:
        check(order.index(201) < order.index(206), 'Private consent roster precedes its joined notice')
    if 503 in ids and 501 in ids:
        check(order.index(503) < order.index(501), 'Private scene member update precedes descriptor')
    return result


def exercise(host, guest, observer, active, sql, check, z):
    games = [host, guest, observer]
    actions = 0
    for game in games: game.issue()
    before = sql('SELECT CharacterId,ClanId,MembershipStatus FROM ClanMembers ORDER BY CharacterId;')
    for game, target in ((observer,9000),(host,9001),(host,0xffffffffffffffff)):
        game.send(2028, struct.pack('<Q',target)+text('NetClan'), split=True)
        game.receive(509)
        actions += 1
    host.send(2028, struct.pack('<Q',9000)+text('FORGED'))
    private = descriptor(host.receive(117),6,1,check)
    actions += 1
    active.send(1011)
    expected = struct.pack('<I',2)
    for game, scene, role in ((host,6,101),(guest,1,100)):
        expected += struct.pack('<I',game.character)+z(NAMES[game.role])+struct.pack('<III',scene,0,role)
    check(active.receive(1012)==expected,'Auxiliary roster reads committed private main scene6')
    # Approved visitation uses the same full-group authority as invitation.
    guest.send(112,text(NAMES['host']))
    r=Reader(host.receive(124)); key=r.u32()
    check(key>0 and r.text()==NAMES['guest'] and r.u32()==0,'Private visitation identifies authenticated visitor')
    r.end()
    host.send(114,struct.pack('<I',key))
    check(guest.receive(125)==text(NAMES['host'])+bytes(6),'Private visit approval has native address baseline')
    host.party_members=guest.party_members=[host,guest]
    guest.send(115,text(NAMES['host'])+bytes(7))
    joined=batch(guest,[117,201,206],check)
    check(descriptor(joined[117][0],6,2,check)==private,'Approved member enters the same private scene')
    existing=batch(host,[503,501,201,206],check)
    check(joined[206][0]==existing[206][0]==text(NAMES['guest']),'Private consent has exact native joined name')
    actions += 1
    time.sleep(3.1)
    _, accept=ask(host,observer,NAMES['host'],NAMES['observer'],check,lambda *a,**k:None,text)
    observer.send(115,accept)
    failure(observer,122,NAMES['host'],21,check)
    actions += 1
    guest.send(2028,struct.pack('<Q',9000)+text('NetClan'))
    guest.receive(509)  # Project leader-only policy; no fake S2029.
    movement=struct.pack('<IfffBBf',7,1.0,2.0,3.0,0,0,0.5)
    host.send(511,movement)
    check(guest.receive(514)==struct.pack('<I',host.character)+movement,'Private movement preserves trusted sender identity')
    host.send(502,struct.pack('<I',0)+text('Clan plaza message')+text(''))
    first=host.receive(509)
    check(guest.receive(509)==first and text('Clan plaza message') in first,'Private chat reaches admitted members exactly')
    observer.issue()  # Any leaked private movement/chat would fail the barrier.
    actions += 2
    host.send(402,host_create_payload())
    host.receive(102)
    descriptor(host.receive(117),2,2,check);descriptor(guest.receive(117),2,2,check)
    host.send(2028,struct.pack('<Q',9000)+text('WRONG'))
    a=descriptor(host.receive(117),6,2,check)
    b=descriptor(guest.receive(117),6,2,check)
    check(a==b,'Complete waiting-room party commits into one private scene')
    actions += 1
    host.send(106)
    descriptor(host.receive(117),3,2,check);descriptor(guest.receive(117),3,2,check)
    host.send(2028,struct.pack('<Q',9000)+text(''))
    descriptor(host.receive(117),6,2,check);descriptor(guest.receive(117),6,2,check)
    host.send(105)
    for game in (host,guest):
        for mid in (105,117,806,837,802,810): game.receive(mid)
    actions += 3
    # Restore the surrounding suite's solo menu preconditions.
    guest.party_members=[guest];host.party_members=[host]
    guest.send(202);guest.receive(201)
    check(guest.receive(204)==b'','Private flow member returns to solo party')
    host.receive(201);check(host.receive(205)==text(NAMES['guest']),'Private flow leader receives exact departure')
    for game in games: game.issue()
    check(sql('SELECT CharacterId,ClanId,MembershipStatus FROM ClanMembers ORDER BY CharacterId;')==before,
          'Private entry, refused consent and scene movement do not mutate membership')
    return actions


def resident_loss(game, sql, check):
    game.send(521)
    for mid in (105,117,806,837,802,810,170,201): game.receive(mid)
    game.send(2028,struct.pack('<Q',9000)+text('FORGED'))
    descriptor(game.receive(117),6,1,check)
    sql(f'DELETE FROM ClanMembers WHERE CharacterId={game.character};')
    previous=game.socket.gettimeout()
    try:
        game.socket.settimeout(15)
        deadline=time.monotonic()+15
        cleared=False
        while True:
            try: first=game.socket.recv(1)
            except (ConnectionResetError,ConnectionAbortedError): first=b''
            if not first: break
            header=game.incoming.transform(first+game.exact(7),-1)
            length,mid=struct.unpack_from('<IH',header)
            check(xor(header)==0 and length<=1048576,'Private residency bounded valid header')
            body=game.incoming.transform(game.exact(length),-1)
            check(xor(body)==header[6],'Private residency body checksum')
            if mid==201:
                check(not cleared and observe(game,body,check)==[0],'At most one canonical absent-clan update before private revocation')
                cleared=True
            else:
                check(mid==4 and body==b'','Only native heartbeat or clan update precedes private revocation')
                game.send(41)
            check(time.monotonic()<deadline,'Private residency revalidation bounded by policy')
        check(True,'External membership removal closes private main residency')
    finally:
        game.socket.settimeout(previous)
        sql(f'INSERT INTO ClanMembers(CharacterId,ClanId,MembershipStatus) VALUES({game.character},9000,101);')
    return 2
