"""Independent wire expectations for scheduled disband and cancellation."""
import struct


def exercise(host, guest, active, guest_clan, observer_clan, sql, check, z, due):
    actions = 0
    peers = [active, guest_clan, observer_clan]
    roles = [101, 100, 1]

    def profile(who, deadline):
        expected = (struct.pack('<BII', int(roles[who] >= 100), 9000, roles[who]) +
                    z('NetClan') + struct.pack('<d', 25569.0 + deadline / 86400.0))
        check(peers[who].receive(1016) == expected, 'Disband state uses trusted profile date and role')

    def send(who, action, success=False, available=True, deadline=4102444800):
        nonlocal actions
        actions += 1
        peers[who].send(1029, struct.pack('<I', action), split=True)
        check(peers[who].receive(1030) == struct.pack('<II', int(success), action), 'Native disband result and action')
        if success:
            notice = struct.pack('<II', 1, action)
            check(active.receive(1031) == notice, 'Requester gets successful state transition')
            profile(0, deadline)
            check(guest_clan.receive(1031) == notice, 'Member gets native disband transition')
            profile(2, deadline)  # Pending applicant gets profile only.
        elif available:
            profile(who, deadline)

    send(0, 1, available=False)  # Main menu cannot authorize.
    host.send(106); host.receive(117)
    guest.send(106); guest.receive(117); host.receive(503); host.receive(501)
    send(1, 1)
    send(0, 0)
    send(0, 2)
    send(0, 1, success=True, deadline=due)
    check(sql('SELECT DisbandAtUnixSeconds FROM Clans WHERE ClanId=9000;') == str(due), 'Configured maintenance deadline persisted')
    send(0, 1, deadline=due)
    guest_clan.send(1015); profile(1, due)
    active.send(1057, struct.pack('<I', guest.character))
    check(active.receive(1058) == bytes(5), 'Scheduled clan cannot start master transfer')
    send(0, 2, success=True)
    check(sql('SELECT DisbandAtUnixSeconds FROM Clans WHERE ClanId=9000;') == '0', 'Cancellation persists zero deadline')
    send(0, 2)
    guest.send(105)
    for mid in (105, 117, 806, 837, 802, 810): guest.receive(mid)
    host.receive(511); host.receive(501)
    host.send(105)
    for mid in (105, 117, 806, 837, 802, 810): host.receive(mid)
    return actions
