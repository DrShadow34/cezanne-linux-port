# The modem's RPC requests to the AP (CCCI channels 32 / 33, /dev/ccci_rpc):
# board facts (GPIO numbers and levels, SIM hot-plug EINT setup, device
# tree attributes, system properties). Request and reply layout as the FS
# service: header (reserved = request slot), u32 op, u32 argc, arguments;
# reply op | 0xffff0000. Semantics follow MediaTek's port_rpc.c (kernel
# side ops) and ccci_rpcd (0x400f / 0x4010).
import glob
import re
import struct

from . import board, ccci
from .ccci import u32, i32

FS_NO_OP = -1
FS_PARAM_ERROR = -2
FS_FUNC_FAIL = -5
ERR_QUERY_STRING = -11
OPS = {}


def op(code):
    def reg(fn):
        OPS[code] = fn
        return fn
    return reg


def cstr(b):
    return b.split(b'\0', 1)[0].decode('ascii', 'replace')


def gpio_level(pin):
    """input level of an AP pin from the pinctrl debugfs, or -1"""
    for f in glob.glob('/sys/kernel/debug/pinctrl/*pinctrl*/pins'):
        try:
            for line in open(f):
                m = re.match(r'\s*(\d+):.*?din\s*[:=]?\s*(\d)', line)
                if m and int(m.group(1)) == pin:
                    return int(m.group(2))
        except OSError:
            pass
    return -1


def gpio_lookup(name):
    name = board.GPIO_ALIASES.get(name, name)
    return board.GPIOS.get(name)


class RpcServer:
    def __init__(self, log=None):
        self.log = log or (lambda *a: None)

    def packet(self, pkt):
        _, _, _, _, slot = ccci.header(pkt)
        body = pkt[16:]
        opc, argc = struct.unpack_from('<II', body)
        try:
            args = ccci.lv_parse(body, argc, 8)
        except ValueError as e:
            self.log('rpc: bad request %#x: %s' % (opc, e))
            args, res = [], [u32(FS_PARAM_ERROR)] * 2
        else:
            fn = OPS.get(opc)
            if fn is None:
                self.log('rpc: unknown op %#x (%d args)' % (opc, len(args)))
                res = [u32(FS_NO_OP)]
            else:
                try:
                    res = fn(self, *args)
                except (TypeError, IndexError, struct.error) as e:
                    self.log('rpc: op %#x bad arguments: %s' % (opc, e))
                    res = [u32(FS_PARAM_ERROR)] * 2
        payload = u32(opc | 0xffff0000) + u32(len(res)) + ccci.lv_pack(res)
        return [ccci.make(0, slot, payload)]


@op(0x4001)	# GET_TDD_EINT_NUM: no TDD EINTs on this board
@op(0x4003)	# GET_ADC_NUM: no MD ADC channels
def rpc_no_num(s, name, *_):
    s.log('rpc: %s: none' % cstr(name))
    return [u32(FS_FUNC_FAIL)] * 2


@op(0x4002)
def rpc_gpio_num(s, name, *_):
    g = gpio_lookup(cstr(name))
    n = g[1] if g else FS_FUNC_FAIL
    s.log('rpc: GPIO %s -> %d' % (cstr(name), n))
    return [u32(n)] * 2


@op(0x4004)	# GET_EMI_CLK_TYPE: not provided (as stock)
def rpc_emi_clk(s, *_):
    return [u32(FS_FUNC_FAIL)] * 3


@op(0x4005)
def rpc_eint_attr(s, name, _len, typ, *_):
    vals = board.EINTS.get(cstr(name))
    t = i32(typ)
    if vals is None or not 0 <= t < len(vals):
        r = ERR_QUERY_STRING if vals is None else -12
        return [u32(r)] * 2
    v = vals[t]
    s.log('rpc: EINT %s attr %d -> %d' % (cstr(name), t, v))
    if v < 0:
        return [u32(ERR_QUERY_STRING)] * 2
    return [u32(0), u32(v)]


@op(0x4006)
def rpc_gpio_val(s, num, *_):
    v = gpio_level(i32(num))
    s.log('rpc: GPIO %d level %d' % (i32(num), v))
    return [u32(v)] * 2


@op(0x4007)	# GET_ADC_VAL
def rpc_adc_val(s, num, *_):
    return [u32(-1)] * 2


@op(0x4009)
def rpc_gpio_adc(s, req, *_):
    """pin numbers / levels for up to 3 (v1) or 10 (v2) named GPIOs"""
    v2 = len(req) != 2 + 3 * 34 + 12 + 33 + 8
    n, hdr = (10, '<HH') if v2 else (3, '<BB')
    mask, valid = struct.unpack_from(hdr, req)
    off = struct.calcsize(hdr)
    names = [cstr(req[off + 34 * i:off + 34 * (i + 1)]) for i in range(n)]
    nums = list(struct.unpack_from('<%dI' % n, req, off + 34 * n))
    out_num, out_val = [0] * n, [0] * n
    for i in range(n):
        if not valid & (1 << i):
            continue
        g = gpio_lookup(names[i])
        if mask & 1 and g:
            out_num[i] = g[1]
        if mask & 2:
            pin = g[0] if (mask & 1 and g) else nums[i]
            out_val[i] = gpio_level(pin) & 0xffffffff
    s.log('rpc: GPIO_ADC %s -> %s %s' % ([x for x in names if x], out_num, out_val))
    return [u32(0), struct.pack('<%dI%dIII' % (n, n), *out_num, *out_val, 0, 0)]


@op(0x400d)	# CCCI_LHIF_MAPPING: no ccmni queue remap needed yet
def rpc_lhif(s, *_):
    return [u32(0)] * 2


@op(0x400e)
def rpc_dtsi(s, req, *_):
    name = cstr(req[2:66])
    out = bytearray(b'\x0f' * 68)
    if req[0] == 1 and name in board.DTSI:
        struct.pack_into('<I', out, 0, board.DTSI[name])
    s.log('rpc: DTSI %s -> %#x' % (name, struct.unpack_from('<I', out)[0]))
    return [u32(0), bytes(out)]


@op(0x400f)
def rpc_property(s, key, *_):
    v = board.PROPERTIES.get(cstr(key), '')
    s.log('rpc: property %s = "%s"' % (cstr(key), v))
    return [u32(len(v)), v.encode() + b'\0']


@op(0x4010)	# SAR_TABLE_IDX_QUERY: no SAR sensor input; MD keeps its default
def rpc_sar(s, *_):
    return [u32(0), u32(0)]


@op(0x4013)	# QUERY_CARD_TYPE: the stock kernel echoes the request
def rpc_card_type(s, *args):
    return list(args)


@op(0x4321)	# IT_OP: loopback test
def rpc_it(s, *_):
    return [u32(1), u32(0xa5a5)]
