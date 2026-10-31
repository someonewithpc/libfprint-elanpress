#!/usr/bin/env python3
# Derives the recordings of the elanpress-* driver tests from the one of
# the elanpress test (0c3d, firmware 0x0165), for cases no device was
# around to record.
#
# Usage: elanpress-recordings.py [--check]
#
# Without arguments it writes each test's custom.pcapng. With --check it
# only compares them with what it would write, and fails if one differs.

import os
import struct
import sys

TESTS = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(TESTS, 'elanpress', 'custom.pcapng')

CMD_GET_FW_VER = b'\x40\x19'
CMD_GET_CALIB_MEAN = b'\x40\x24'
CMD_GET_IMAGE = b'\x00\x09'
CMD_STOP = b'\x00\x0b'
CMD_GET_DIM = b'\x00\x0c'
CMD_LED_ON = b'\x40\x31'
CMD_PRE_SCAN = b'\x40\x3f'
EP_CTRL = 0x80
EP_CMD_OUT = 0x01
EP_IMG_IN = 0x82
EP_CMD_IN = 0x83


def blocks(data):
    off = 0
    while off < len(data):
        btype, blen = struct.unpack_from('<II', data, off)
        assert blen >= 12 and off + blen <= len(data), 'bad block at %d' % off
        yield btype, data[off:off + blen]
        off += blen


# An Enhanced Packet Block holds a usbmon mmapped record: a 64-byte header,
# then the payload. The header has the URB type at 8, the endpoint at 10,
# and the URB length and captured length at 32 and 36
def parse(blk):
    caplen = struct.unpack_from('<I', blk, 20)[0]
    pkt = blk[28:28 + caplen]
    return chr(pkt[8]), pkt[10], pkt[64:]


# The block with its reply cut to payload
def replied(blk, payload):
    caplen = struct.unpack_from('<I', blk, 20)[0]
    padded = (caplen + 3) & ~3
    hdr = bytearray(blk[28:28 + 64])
    struct.pack_into('<II', hdr, 32, len(payload), len(payload))
    pkt = bytes(hdr) + payload
    body = bytearray(blk[:28]) + pkt + bytes(-len(pkt) % 4)
    body += blk[28 + padded:-4]
    struct.pack_into('<II', body, 20, len(pkt), len(pkt))
    struct.pack_into('<I', body, 4, len(body) + 4)
    return bytes(body) + struct.pack('<I', len(body) + 4)


def emptied(blk):
    return replied(blk, b'')


# Packets as (block type, block, URB type, endpoint, payload, last command)
def packets(data):
    cmd = None
    for btype, blk in blocks(data):
        if btype != 6:
            yield btype, blk, None, None, None, cmd
            continue
        urb, ep, payload = parse(blk)
        if urb == 'S' and ep == EP_CMD_OUT:
            cmd = payload
        yield btype, blk, urb, ep, payload, cmd


# The source starts with the device descriptors, then the exchanges below
SOURCE_START = [
    ('fw_cmd', EP_CMD_OUT, CMD_GET_FW_VER),
    ('fw_read', EP_CMD_IN, CMD_GET_FW_VER),
    ('dim_cmd', EP_CMD_OUT, CMD_GET_DIM),
    ('dim_read', EP_CMD_IN, CMD_GET_DIM),
    ('stop', EP_CMD_OUT, CMD_STOP),
    ('led', EP_CMD_OUT, CMD_LED_ON),
    ('calib_cmd', EP_CMD_OUT, CMD_GET_CALIB_MEAN),
    ('calib_read', EP_CMD_IN, CMD_GET_CALIB_MEAN),
    ('bg_cmd', EP_CMD_OUT, CMD_GET_IMAGE),
    ('bg_read', EP_IMG_IN, CMD_GET_IMAGE),
    ('pre_scan_cmd', EP_CMD_OUT, CMD_PRE_SCAN),
    ('pre_scan_read', EP_CMD_IN, CMD_PRE_SCAN),
    ('frame_cmd', EP_CMD_OUT, CMD_GET_IMAGE),
    ('frame_read', EP_IMG_IN, CMD_GET_IMAGE),
]


# The blocks before the first exchange, and each exchange as its submit
# and completion
def source_start(data):
    head = []
    pk = []
    for btype, blk, urb, ep, payload, cmd in packets(data):
        if not pk and (btype != 6 or ep == EP_CTRL):
            head.append(blk)
            continue
        assert btype == 6
        pk.append((blk, urb, ep, payload, cmd))
        if len(pk) == 2 * len(SOURCE_START):
            break

    ex = {}
    for i, (name, ep, cmd) in enumerate(SOURCE_START):
        (submit, s_urb, s_ep, s_payload, s_cmd), \
            (complete, c_urb, c_ep, _, _) = pk[2 * i:2 * i + 2]
        assert (s_urb, s_ep, c_urb, c_ep) == ('S', ep, 'C', ep), name
        assert s_cmd == cmd, name
        assert ep != EP_CMD_OUT or s_payload == cmd, name
        ex[name] = [submit, complete]
    return head, ex


# Opening the device and five failures, each ending in the stop command
# the driver sends as it fails:
#
# - on open the firmware version read gets an empty reply first, which
#   the driver reads past, as it would a cancelled pre-scan's answer
# - two captures read past an empty calibration mean reply and a 1-byte
#   one, then fail on an empty background reply
# - a capture fails on two empty calibration mean replies
# - a capture fails on an empty frame during the touch
# - after a close, an open fails on an empty firmware version reply with
#   nothing after it, which would otherwise read as firmware 0x0000
def short_reply(data):
    head, ex = source_start(data)
    s, c = ex['fw_read']
    out = head + ex['fw_cmd'] + [s, emptied(c)] + ex['fw_read']
    out += ex['dim_cmd'] + ex['dim_read']

    start = ex['stop'] + ex['led'] + ex['calib_cmd']
    s, c = ex['calib_read']
    bg_s, bg_c = ex['bg_read']
    for stale in (b'', b'\x55'):
        out += start + [s, replied(c, stale)] + ex['calib_read']
        out += ex['bg_cmd'] + [bg_s, emptied(bg_c)] + ex['stop']
    out += start + [s, emptied(c), s, emptied(c)] + ex['stop']

    fr_s, fr_c = ex['frame_read']
    out += start + ex['calib_read'] + ex['bg_cmd'] + ex['bg_read']
    out += ex['pre_scan_cmd'] + ex['pre_scan_read']
    out += ex['frame_cmd'] + [fr_s, emptied(fr_c)] + ex['stop']

    s, c = ex['fw_read']
    out += ex['fw_cmd'] + [s, emptied(c)]
    return out


DERIVED = {
    'elanpress-short-reply': short_reply,
}


def main():
    check = sys.argv[1:] == ['--check']
    if sys.argv[1:] and not check:
        sys.exit('usage: elanpress-recordings.py [--check]')

    data = open(SOURCE, 'rb').read()
    stale = []
    for test, derive in DERIVED.items():
        path = os.path.join(TESTS, test, 'custom.pcapng')
        recording = b''.join(derive(data))
        if not check:
            open(path, 'wb').write(recording)
        elif not os.path.exists(path) or open(path, 'rb').read() != recording:
            stale.append(path)

    if stale:
        sys.exit('out of date, run elanpress-recordings.py: ' +
                 ', '.join(stale))


main()
