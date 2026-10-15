"""Native unavailable-channel response preserves authenticated TCP sessions."""
import struct


def verify_channel_refusals(peers, check, note, phase):
    for peer, category in zip(peers, (60, 70, 0xffffffff)):
        if category == 60 and getattr(peer, 'handoff_enabled', False):
            category = 80
        peer.send(53, struct.pack('<I', category))
        payload = peer.receive(8)
        check(payload == struct.pack('<I4sHI', 2, bytes(4), 0, category),
              'Native channel refusal: exact result, zero endpoint, category echo')
    note('channel-refusal-passed', phase=phase, participants=len(peers),
         channel_transfer_enabled=False, original_ui=False)
