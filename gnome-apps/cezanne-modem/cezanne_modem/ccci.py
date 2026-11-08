# CCCI packets as the cezanne-ccci driver passes them on its user-header
# ports: a 16-byte header (u32 data[0], u32 length, u16 channel, u16 seq,
# u32 reserved) and the payload. Requests and replies of the FS and RPC
# services carry an op word and a list of arguments, each u32 length +
# data padded to 4 bytes.
import struct

HDR = struct.Struct('<IIHHI')
MORE = 0x80000000		# data[0]: more fragments follow


def header(pkt):
    """(data0, length, channel, seq, reserved)"""
    return HDR.unpack_from(pkt)


def make(data0, reserved, payload, channel=0):
    """packet with a header; the driver fills in length and channel"""
    return HDR.pack(data0, 16 + len(payload), channel, 0, reserved) + payload


def lv_parse(buf, count, off=0):
    """count arguments from buf: list of bytes"""
    out = []
    for _ in range(count):
        if off + 4 > len(buf):
            raise ValueError('argument list truncated')
        n = struct.unpack_from('<I', buf, off)[0]
        if off + 4 + n > len(buf):
            raise ValueError('argument longer than packet')
        out.append(bytes(buf[off + 4:off + 4 + n]))
        off += 4 + ((n + 3) & ~3)
    return out


def lv_pack(items):
    out = bytearray()
    for b in items:
        out += struct.pack('<I', len(b)) + b + bytes(-len(b) & 3)
    return bytes(out)


def u32(v):
    return struct.pack('<I', v & 0xffffffff)


def i32(b):
    return struct.unpack('<i', b[:4])[0]
