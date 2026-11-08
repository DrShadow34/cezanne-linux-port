#!/usr/bin/env python3
# nfc-probe.py [seconds]: bring nfc0 up through the kernel NFC netlink API
# (NCI reset + init on the controller), poll for a tag as reader and print
# what the kernel reports (protocols, NFCID1, SENS_RES / SEL_RES), then
# stop and power down. A bring-up check without neard. Run as root.
import os
import select
import socket
import struct
import sys
import time

NETLINK_GENERIC = 16
GENL_ID_CTRL = 0x10
CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_ID, CTRL_ATTR_FAMILY_NAME, CTRL_ATTR_MCAST_GROUPS = 3, 1, 2, 7
NLM_F_REQUEST, NLM_F_ACK = 1, 4
NLMSG_ERROR = 2

(CMD_GET_DEVICE, CMD_DEV_UP, CMD_DEV_DOWN, CMD_START_POLL, CMD_STOP_POLL,
 EV_TARGETS_FOUND, EV_TARGET_LOST, CMD_GET_TARGET) = 1, 2, 3, 6, 7, 9, 12, 8
A_DEV_INDEX, A_DEV_NAME, A_PROTOCOLS, A_TARGET_INDEX = 1, 2, 3, 4
A_SENS_RES, A_SEL_RES, A_NFCID1, A_SENSB, A_SENSF = 5, 6, 7, 8, 9
A_POWERED, A_IM_PROTOCOLS, A_TM_PROTOCOLS = 12, 13, 14
A_ISO15693_UID, A_ATS = 27, 32
PROTO = {1: 'Jewel', 2: 'MIFARE', 3: 'FeliCa', 4: 'ISO14443-4', 5: 'NFC-DEP',
         6: 'ISO14443-B', 7: 'ISO15693'}


def attr(t, data):
    n = 4 + len(data)
    return struct.pack('HH', n, t) + data + b'\0' * ((4 - n % 4) % 4)


def attrs(buf):
    out = {}
    while len(buf) >= 4:
        n, t = struct.unpack_from('HH', buf)
        out[t & 0x3fff] = buf[4:n]
        buf = buf[(n + 3) & ~3:]
    return out


class Genl:
    def __init__(self):
        self.s = socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, NETLINK_GENERIC)
        self.s.bind((0, 0))
        self.seq = 0

    def send(self, fam, cmd, payload=b'', flags=NLM_F_REQUEST | NLM_F_ACK):
        self.seq += 1
        body = struct.pack('BBH', cmd, 1, 0) + payload
        self.s.send(struct.pack('IHHII', 16 + len(body), fam, flags, self.seq, 0) + body)
        return self.seq

    def recv(self, timeout=None):
        r, _, _ = select.select([self.s], [], [], timeout)
        if not r:
            return []
        data, msgs = self.s.recv(65536), []
        while len(data) >= 16:
            n, typ, _, seq, _ = struct.unpack_from('IHHII', data)
            msgs.append((typ, seq, data[16:n]))
            data = data[(n + 3) & ~3:]
        return msgs

    # wait for the ack (or error) of request seq; returns (errno, replies)
    def call(self, fam, cmd, payload=b''):
        seq, replies = self.send(fam, cmd, payload), []
        while True:
            for typ, s, body in self.recv(10):
                if s != seq:
                    continue
                if typ == NLMSG_ERROR:
                    return -struct.unpack_from('i', body)[0], replies
                replies.append(attrs(body[4:]))


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 20
    g = Genl()
    err, rep = g.call(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, attr(CTRL_ATTR_FAMILY_NAME, b'nfc\0'))
    if err:
        sys.exit('no nfc genl family: %s' % os.strerror(err))
    fam = struct.unpack('H', rep[0][CTRL_ATTR_FAMILY_ID][:2])[0]
    group = None
    for grp in attrs(rep[0][CTRL_ATTR_MCAST_GROUPS]).values():
        a = attrs(grp)
        if a.get(1, b'').rstrip(b'\0') == b'events':
            group = struct.unpack('I', a[2])[0]
    g.s.setsockopt(270, 1, group)	# SOL_NETLINK, NETLINK_ADD_MEMBERSHIP
    dev = attr(A_DEV_INDEX, struct.pack('I', 0))

    def show(tag):
        err, rep = g.call(fam, CMD_GET_DEVICE, dev)
        d = rep[0] if rep else {}
        print('%s: %s powered=%s protocols=%#x' % (tag, d.get(A_DEV_NAME, b'?').rstrip(b'\0').decode(),
              d.get(A_POWERED, b'\0')[0], struct.unpack('I', d.get(A_PROTOCOLS, b'\0' * 4))[0]))
    show('before')
    t0 = time.monotonic()
    err, _ = g.call(fam, CMD_DEV_UP, dev)
    print('dev up: %s (%.0f ms)' % (os.strerror(err) if err else 'ok', (time.monotonic() - t0) * 1000))
    if err and err != 114:	# EALREADY
        sys.exit(1)
    show('after')
    err, _ = g.call(fam, CMD_START_POLL, dev + attr(A_IM_PROTOCOLS, struct.pack('I', 0xfe)))
    print('poll: %s; hold a tag or card to the back, %g s' % (os.strerror(err) if err else 'ok', secs))
    end = time.monotonic() + secs
    try:
        while time.monotonic() < end:
            for typ, _, body in g.recv(end - time.monotonic()):
                if typ != fam:
                    continue
                cmd, a = body[0], attrs(body[4:])
                if cmd == EV_TARGETS_FOUND:
                    g.send(fam, CMD_GET_TARGET, dev, flags=NLM_F_REQUEST | 0x300)	# NLM_F_DUMP
                elif cmd == CMD_GET_TARGET and A_TARGET_INDEX in a:
                    p = struct.unpack('I', a.get(A_PROTOCOLS, b'\0' * 4))[0]
                    print('target %d: %s' % (struct.unpack('I', a[A_TARGET_INDEX])[0],
                          ', '.join(v for k, v in PROTO.items() if p & (1 << k)) or hex(p)))
                    for k, name in ((A_NFCID1, 'NFCID1'), (A_SENS_RES, 'SENS_RES'), (A_SEL_RES, 'SEL_RES'),
                                    (A_SENSB, 'SENSB_RES'), (A_SENSF, 'SENSF_RES'),
                                    (A_ISO15693_UID, 'ISO15693 UID'), (A_ATS, 'ATS')):
                        if k in a:
                            print('  %s %s' % (name, a[k].hex()))
                    end = 0
                elif cmd == EV_TARGET_LOST:
                    print('target lost')
    finally:
        g.call(fam, CMD_STOP_POLL, dev)
        err, _ = g.call(fam, CMD_DEV_DOWN, dev)
        print('dev down: %s' % (os.strerror(err) if err else 'ok'))


main()
