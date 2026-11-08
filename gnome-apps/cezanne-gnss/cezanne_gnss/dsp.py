# The CONNSYS GNSS DSP link (/dev/gpsdl0 = L1, /dev/gpsdl1 = L5): framing,
# descrambling and the MVCD boot of the DSP RAM code (see NOTES "GNSS").
#
# Link framing (both directions):
#   AA F0 | word u16 | id u16 | payload | sum u16 | AA 0F
# word: bits 0..11 = 2 + payload + 2, bits 12..15 = a per-type counter on
# the DSP's reports; sum = 16-bit sum of word, id and payload; between the
# markers 0xDE escapes: DE DF = AA, DE E0 = DE. The DSP's reports (all but
# the boot handshake FE31 / FE32) are scrambled: plain = ror8(raw, 3) ^ 0x59
# per payload byte (a zero arrives as 0xCA).
import os
import struct
import sys
import threading
import time

DEV = {'L1': '/dev/gpsdl0', 'L5': '/dev/gpsdl1'}
FRAG = 1400				# code bytes per 0xFE0C fragment
MAX_FRAME = 1425			# longest stuffed fragment frame sent whole
LOAD = {'L1': 0x7400, 'L5': 0x6000}	# DSP load address (exec = load + 4)
# 0xFE08 boot header (as recorded from the stock software 2026-10-29); words
# 7 (code size in 16-bit words), 8 / 9 (load / exec address) and 11
# (fragment count) are set per image, word 10 is the cipher key (0x7B), the
# rest is DSP configuration still to be understood
HEADER = bytes.fromhex(
    '1a0000000000c301a502000000003f88007404747b003200000000000500c800'
    '6f3c2e86e7050200d200b7002800594a1e00c3372800594a8f65070001002800'
    'ff008000470064005000d8005000bb0003003c006f00890088000200fb000100'
    '8568000a000003004e4f'
)
PLAIN_RX = {0xfe31, 0xfe32}
DESCRAMBLE = bytes((((c >> 3) | (c << 5)) & 0xff) ^ 0x59 for c in range(256))

LOG = None		# open frame log: "utc link dir id hex" per frame
LOG_LOCK = threading.Lock()


def log(name, d, mid, payload):
    if LOG:
        with LOG_LOCK:
            LOG.write(f'{time.time():.3f} {name} {d} {mid:04x} {payload.hex()}\n')


def stuff(b):
    return b.replace(b'\xde', b'\xde\xe0').replace(b'\xaa', b'\xde\xdf')


def unstuff(b):
    out, i = bytearray(), 0
    while i < len(b):
        if b[i] == 0xde and i + 1 < len(b):
            out.append(0xaa if b[i + 1] == 0xdf else 0xde)
            i += 2
        else:
            out.append(b[i])
            i += 1
    return bytes(out)


def frame(mid, payload=b''):
    body = struct.pack('<HH', len(payload) + 4, mid) + payload
    body += struct.pack('<H', sum(body) & 0xffff)
    return b'\xaa\xf0' + stuff(body) + b'\xaa\x0f'


class Link:
    """one DSP link; frames: (monotonic s since open, id, counter, payload)
    in arrival order, consumed by the caller under cond"""

    def __init__(self, name):
        self.name = name
        self.fd = os.open(DEV[name], os.O_RDWR)	# the driver has no poll, ignores O_NONBLOCK
        self.buf = b''
        self.frames = []
        self.cond = threading.Condition()
        self.t0 = time.monotonic()
        self.alive = True
        self.reader = threading.Thread(target=self._reader, daemon=True)
        self.reader.start()

    def close(self):
        """stop the reader (its blocking read returns with the DSP's next
        1 Hz report) and close the device (the kernel powers the link down
        with the last close)"""
        self.alive = False
        self.reader.join(3)
        os.close(self.fd)

    def _reader(self):
        while self.alive:
            try:
                data = os.read(self.fd, 8192)
            except OSError as e:
                if self.alive:
                    print(f'{self.name} read: {e}', file=sys.stderr)
                return
            if data and self.alive:
                self._parse(data)

    def _parse(self, data):
        with self.cond:
            self.buf += data
            while True:
                j = self.buf.find(b'\xaa\xf0')
                k = self.buf.find(b'\xaa\x0f', j + 2) if j >= 0 else -1
                if k < 0:
                    break
                body = unstuff(self.buf[j + 2:k])
                self.buf = self.buf[k + 2:]
                if len(body) < 6:
                    continue
                word, mid = struct.unpack_from('<HH', body)
                n = word & 0xfff
                if len(body) != n + 2 or sum(body[:n]) & 0xffff != struct.unpack_from('<H', body, n)[0]:
                    print(f'{self.name} bad frame {body[:16].hex()}', file=sys.stderr)
                    continue
                payload = body[4:n]
                if mid not in PLAIN_RX:
                    payload = payload.translate(DESCRAMBLE)
                self.frames.append((time.monotonic() - self.t0, mid, word >> 12, payload))
                log(self.name, 'rx', mid, payload)
                self.cond.notify_all()

    def take(self):
        """all frames received so far"""
        with self.cond:
            new, self.frames = self.frames, []
        return new

    def send(self, mid, payload=b''):
        data = frame(mid, payload)
        while data:
            data = data[os.write(self.fd, data):]
        if mid != 0xfe0c:		# not the image download
            log(self.name, 'tx', mid, payload)

    def wait(self, mid, timeout=1.0, match=None):
        end = time.monotonic() + timeout
        with self.cond:
            while True:
                for i, f in enumerate(self.frames):
                    if f[1] == mid and (match is None or f[3] == match):
                        return self.frames.pop(i)
                left = end - time.monotonic()
                if left <= 0:
                    return None
                self.cond.wait(left)


def pieces(image):
    """the image in FRAG-byte chunks; a chunk whose stuffed frame would be
    longer than MAX_FRAME goes as two halves (the L5 image has one such
    chunk). Fragment numbers count the pieces from 1."""
    out = []
    for i in range(0, len(image), FRAG):
        chunk = image[i:i + FRAG]
        if len(frame(0xfe0c, b'\0\0' + chunk)) > MAX_FRAME:
            half = len(chunk) // 2
            out += [chunk[:half], chunk[half:]]
        else:
            out.append(chunk)
    return out


def boot(link, image, header=HEADER, verbose=True):
    """MVCD: header, hello, fragments each acked; True when all are in"""
    header = bytearray(header)
    parts = pieces(image)
    load = LOAD[link.name]
    struct.pack_into('<H', header, 14, len(image) // 2)	# code size, 16-bit words
    struct.pack_into('<HH', header, 16, load, load + 4)	# load / exec address
    struct.pack_into('<H', header, 22, len(parts))
    words = struct.unpack_from('<12H', header)
    if verbose:
        print(f'{link.name}: image {len(image)} B, {len(parts)} fragments, load {words[8]:#x}, '
              f'exec {words[9]:#x}, key {words[10]:#x}')
    link.send(0xfe08, bytes(header))
    hello = link.wait(0xfe31)
    if verbose:
        print(f'{link.name}: hello {hello[3].hex() if hello else "none"}')
    if not hello:
        return False
    for i, part in enumerate(parts):
        num = struct.pack('<H', i + 1)
        link.send(0xfe0c, num + part)
        if not link.wait(0xfe32, 1.0, num):
            print(f'{link.name}: no ack for fragment {i + 1}', file=sys.stderr)
            return False
    if verbose:
        print(f'{link.name}: download done at {time.monotonic() - link.t0:.3f} s')
    return True
