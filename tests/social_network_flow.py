"""Project companion + native blocking integration in authenticated scene sessions."""
import struct
import time


class Reader:
    def __init__(self, payload): self.payload, self.at = payload, 0
    def take(self, n):
        value = self.payload[self.at:self.at+n]
        if len(value) != n: raise RuntimeError('Truncated social notification')
        self.at += n
        return value
    def u32(self): return struct.unpack('<I', self.take(4))[0]
    def u8(self): return self.take(1)[0]
    def text(self): return self.take(self.u32()*2).decode('utf-16-le')
    def end(self):
        if self.at != len(self.payload): raise RuntimeError('Trailing social notification')


def roster(peer, check, status=0, action=0):
    r = Reader(peer.receive(65011))
    check((r.u32(), r.u32(), r.u8()) == (1,status,action), 'Companion social version/status/action')
    r.text()
    result = {}
    for _ in range(r.u32()):
        identity, name, relationship, online = r.u32(), r.text(), r.u8(), r.u8()
        check(identity not in result and relationship <= 4 and online <= 1, 'Unique bounded roster entry')
        result[identity] = (name,relationship,online)
    r.end()
    return result


def throttle(peer):
    # Keep transport checks independent of the deliberate five-per-three-second policy.
    previous = getattr(peer,'social_times',[])
    if len(previous) >= 4:
        time.sleep(max(0,3.1-(time.monotonic()-previous[-4])))
    peer.social_times = previous[-3:]+[time.monotonic()]


def request(peer, text, action, target=0, name='', message=''):
    throttle(peer)
    peer.send(65010,struct.pack('<IBI',1,action,target)+text(name)+text(message))


def message(peer, check, target, name, speaker, content):
    r=Reader(peer.receive(65015))
    check((r.u32(),r.u32(),r.text(),r.text(),r.text()) == (1,target,name,speaker,content),
          'Authoritative peer/sender and exact Unicode private message')
    r.end()


def barrier(peer, check):
    peer.send(828)
    check(len(peer.receive(834)) == 12,'Blocked traffic absent before live connection barrier')


def verify_plaza_social(host,guest,check,note,text):
    for peer in (host,guest):
        request(peer,text,0)
        check(not roster(peer,check),'Fresh generated social roster')
    request(host,text,8,guest.character_id,message='unapproved')
    roster(host,check,6,8);barrier(guest,check)
    request(host,text,1,name='RoomProbe')
    check(roster(host,check,0,1)=={guest.character_id:('RoomProbe',0,0)},'Exact role lookup without presence leakage')
    request(host,text,2,name='RoomProbe')
    check(roster(guest,check)[host.character_id][1]==2,'Recipient pending request')
    check(roster(host,check,0,2)[guest.character_id][1]==3,'Sender pending request')
    request(host,text,3,guest.character_id)
    roster(host,check,5,3)
    request(guest,text,3,host.character_id)
    check(roster(host,check)[guest.character_id]==('RoomProbe',1,1),'Accepted friend online on sender')
    check(roster(guest,check,0,3)[host.character_id]==('RoomPeer',1,1),'Accepted friend online on recipient')
    request(host,text,8,guest.character_id,name='RoomPeer',message='spoofed destination')
    roster(host,check,1,8);barrier(guest,check)
    content='中文 private 😀'
    request(host,text,8,guest.character_id,message=content)
    message(guest,check,host.character_id,'RoomPeer','RoomPeer',content)
    message(host,check,guest.character_id,'RoomProbe','RoomPeer',content)
    # Native C147 and S171 coexist with the local companion.
    time.sleep(3.1)
    guest.send(147,b'\x3a'+struct.pack('<I',host.character_id)+text('RoomPeer'))
    guest.social_times.append(time.monotonic())
    check(guest.receive(171)==b'\x3a'+struct.pack('<I',host.character_id)+text('RoomPeer'),'Original block-add acknowledgement')
    check(roster(host,check)[guest.character_id][2]==0,'Blocked peer presence concealed')
    check(roster(guest,check)[host.character_id][1:]==(4,0),'Block owner state concealed')
    request(host,text,8,guest.character_id,message='blocked message')
    roster(host,check,3,8);barrier(guest,check)
    host.send(502,struct.pack('<I',0)+text('blocked plaza')+text(''))
    host.receive(509);barrier(guest,check)
    guest.send(148,b'\xba'+text('RoomPeer'))
    guest.social_times.append(time.monotonic())
    check(guest.receive(172)==struct.pack('<I',host.character_id)+text('RoomPeer'),'Original block-remove acknowledgement by name')
    check(roster(host,check)[guest.character_id][2]==1,'Unblock restores accepted online friend')
    check(roster(guest,check)[host.character_id][1:]==(1,1),'Unblock retains friendship')
    host.send(502,struct.pack('<I',0)+text('unblocked plaza')+text(''))
    check(host.receive(509)==guest.receive(509),'Unblocked plaza text delivered')
    note('social-plaza-passed',NativeBlock=True,PrivateUnicode=True,PendingAuthorization=True,BlockedDelivery=False)


def verify_room_social(host,guest,peers,check,note,text,balance):
    # Drain unrelated login presence notifications through balance barriers.
    for peer in (host,guest):barrier(peer,check)
    request(guest,text,6,host.character_id)
    roster(host,check)
    check(struct.unpack_from('<I',guest.receive(170))[0]==1,'Companion block updates native list')
    roster(guest,check,0,6)
    host.send(502,struct.pack('<I',0)+text('blocked room')+text(''))
    expected=host.receive(509)
    for peer in peers[2:]:check(peer.receive(509)==expected,'Unblocked room observer still receives text')
    balance(guest)
    request(guest,text,8,host.character_id,message='blocked reverse')
    roster(guest,check,3,8);barrier(host,check)
    request(guest,text,7,host.character_id)
    roster(host,check)
    check(guest.receive(170)==bytes(4),'Native block list cleared')
    roster(guest,check,0,7)
    request(guest,text,8,host.character_id,message='room private')
    message(host,check,guest.character_id,'RoomProbe','RoomProbe','room private')
    message(guest,check,host.character_id,'RoomPeer','RoomProbe','room private')
    for peer in peers[2:]:barrier(peer,check)
    request(host,text,5,guest.character_id)
    check(not roster(guest,check),'Friend removal updates other endpoint')
    check(not roster(host,check,0,5),'Friend removal updates sender')
    request(guest,text,8,host.character_id,message='removed friend')
    roster(guest,check,6,8);barrier(host,check)
    note('social-room-passed',PrivateIsolation=True,BlocksRespected=True,RemoveRevokesMessaging=True)
