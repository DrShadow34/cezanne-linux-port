# The modem's file service (CCCI channels 14 / 15, /dev/ccci_fs): the MD
# keeps its NVRAM (calibration, IMEI, settings) and configuration files on
# the AP and reaches them with FAT-style calls on drive letters. Behaviour
# (paths, flags, error codes, reply layouts) follows MediaTek's ccci_fsd as
# seen in MIUI 14's ccci_mdinit; see NOTES.md "Modem file service".
#
# Request: header (reserved = request slot), u32 op, u32 argc, arguments.
# Reply: u32 op | 0xffff0000, u32 count, results. Payloads longer than one
# packet are split in 0xd80-byte chunks, each packet carrying the op word
# again, data[0] bit 31 set on all but the last.
import errno
import os
import stat
import struct
import time

from . import ccci
from .ccci import u32, i32

CHUNK = 0xd80

# drive letter -> directory: the MD's own data (NVRAM, its copy of
# Android's partitions, relative to the state root) and read-only modem
# configuration from the stock vendor image (absolute)
FW = '/usr/lib/firmware/mediatek/cezanne/'
DRIVES = {
    'Z': 'mnt/vendor/nvdata/md',
    'X': 'mnt/vendor/protect_f/md',
    'Y': 'mnt/vendor/protect_s/md',
    'W': FW + 'modem',
    'V': 'mnt/vendor/nvdata/md_cmn',
    'U': 'data/vendor/mdlpm',
    'T': FW + 'mdota',
    'S': 'mnt/vendor/nvcfg',
    'R': FW + 'md',
    'Q': 'data/vendor_de/md',
}

# FS error codes (ccci_fsd FS_ErrorConv and friends)
E_PARAM = -2
E_TOO_MANY = -5
E_NO_MORE = -6
E_NOT_FOUND = -9
E_HANDLE = -10
E_ACCESS = -16
E_GENERIC = -18
E_DRIVE = -19
E_PATH_LONG = -49
MAX_FILES = 128
MAX_PATH = 0x1000

OPS = {}
# results per reply (the MD asserts on any other count); default 1
NRES = {0x1003: 3, 0x1004: 2, 0x1009: 2, 0x100e: 2, 0x1011: 2, 0x1012: 3,
        0x1013: 3, 0x1017: 0, 0x101e: 2, 0x101f: 2, 0x1022: 4, 0x1024: 3,
        0x1025: 2}


def op(code):
    def reg(fn):
        OPS[code] = fn
        return fn
    return reg


def err_conv(e):
    return {errno.EMFILE: E_TOO_MANY, errno.EACCES: E_ACCESS,
            errno.ENOENT: E_NOT_FOUND}.get(e, E_GENERIC)


def wstr(b):
    """the MD's UTF-16LE path argument"""
    s = b.decode('utf-16-le', 'replace')
    return s.split('\0', 1)[0]


class FsServer:
    def __init__(self, root, log=None):
        self.root = root
        self.log = log or (lambda *a: None)
        self.files = [None] * (MAX_FILES + 1)	# handle -> (fd, flags, path)
        self.pending = {}			# slot -> partial request
        self.finds = {}

    # --- packets -----------------------------------------------------

    def packet(self, pkt):
        """one packet from the MD; returns the reply packets"""
        d0, _, _, _, slot = ccci.header(pkt)
        body = pkt[16:]
        if slot in self.pending:
            body = self.pending.pop(slot) + body[4:]	# op word repeated
        if d0 & ccci.MORE:
            self.pending[slot] = body
            return []
        opc, argc = struct.unpack_from('<II', body)
        try:
            args = ccci.lv_parse(body, argc, 8)
        except ValueError as e:
            self.log('fs: bad request %#x: %s' % (opc, e))
            return self.reply(slot, opc, self.error(opc))
        fn = OPS.get(opc)
        if fn is None:
            self.log('fs: unknown op %#x' % opc)
            res = [u32(E_PARAM)]
        else:
            try:
                res = fn(self, *args)
            except (TypeError, IndexError, ValueError, struct.error) as e:
                self.log('fs: op %#x bad arguments: %s' % (opc, e))
                res = self.error(opc)
        return self.reply(slot, opc, res)

    @staticmethod
    def error(opc):
        n = NRES.get(opc, 1)
        return ([u32(E_PARAM)] + [b''] * (n - 1)) if n else []

    def reply(self, slot, opc, results):
        stream = u32(len(results)) + ccci.lv_pack(results)
        rop = u32(opc | 0xffff0000)
        out = []
        for i in range(0, len(stream), CHUNK):
            last = i + CHUNK >= len(stream)
            out.append(ccci.make(0 if last else ccci.MORE, slot, rop + stream[i:i + CHUNK]))
        return out

    # --- paths and handles -------------------------------------------

    def path(self, wpath):
        """host path for a drive path, or an FS error code"""
        p = wstr(wpath) if isinstance(wpath, bytes) else wpath
        if len(p) < 2 or p[1] != ':':
            return E_DRIVE
        p = p.replace('\\', '/')
        if '..' in p:
            return E_ACCESS
        base = DRIVES.get(p[0].upper())
        if base is None:
            return E_DRIVE
        full = os.path.join(self.root, base) + p[2:]
        if len(full) + 1 > MAX_PATH:
            return E_PATH_LONG
        return full

    def get(self, h):
        h = i32(h) if isinstance(h, bytes) else h
        if 0 < h <= MAX_FILES and self.files[h]:
            return self.files[h]
        return None

    def open(self, wpath, flags):
        p = self.path(wpath)
        if isinstance(p, int):
            return p
        mode = 0 if flags & 0x100 else os.O_RDWR
        if flags & 0x10000:
            mode = os.O_RDWR | os.O_CREAT
        if flags & 0x20000:
            mode = os.O_RDWR | os.O_CREAT | os.O_TRUNC
        if flags & 0x10000000:
            mode |= os.O_NONBLOCK
        try:
            h = self.files.index(None, 1)
        except ValueError:
            return E_TOO_MANY
        try:
            fd = os.open(p, mode, 0o660)
        except OSError as e:
            return err_conv(e.errno)
        self.files[h] = (fd, flags, p)
        return h

    def close(self, h):
        f = self.get(h)
        if not f:
            return E_HANDLE
        self.files[i32(h) if isinstance(h, bytes) else h] = None
        if f[0] is None:
            return 0
        try:
            os.close(f[0])
        except OSError as e:
            return err_conv(e.errno)
        return 0

    def close_all(self):
        for h in range(1, MAX_FILES + 1):
            if self.files[h]:
                self.close(h)

    # --- operations --------------------------------------------------


@op(0x1001)
def fs_open(s, path, flags, *_):
    return [u32(s.open(path, i32(flags)))]


@op(0x1002)
def fs_seek(s, h, off, whence, *_):
    f = s.get(h)
    w = i32(whence)
    if not f:
        return [u32(E_HANDLE)]
    if not 0 <= w < 3:
        return [u32(E_PARAM)]
    try:
        return [u32(os.lseek(f[0], i32(off), w))]
    except OSError as e:
        return [u32(err_conv(e.errno))]


@op(0x1003)
def fs_read(s, h, length, *_):
    f = s.get(h)
    if not f:
        return [u32(E_HANDLE), u32(0), b'']
    try:
        data = os.read(f[0], i32(length))
    except OSError as e:
        return [u32(err_conv(e.errno)), u32(0), b'']
    return [u32(0), u32(len(data)), data]


@op(0x1004)
def fs_write(s, h, data, length, *_):
    f = s.get(h)
    if not f:
        return [u32(E_HANDLE), u32(0)]
    try:
        n = os.write(f[0], data[:i32(length)])
    except OSError as e:
        return [u32(err_conv(e.errno)), u32(0)]
    return [u32(0), u32(n)]


@op(0x1005)
def fs_close(s, h, *_):
    return [u32(s.close(h))]


@op(0x1006)
def fs_close_all(s, *_):
    s.close_all()
    return [u32(0)]


@op(0x1007)
def fs_mkdir(s, path, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    try:
        os.mkdir(p.rstrip('/'), 0o770)
    except OSError as e:
        return [u32(err_conv(e.errno))]
    return [u32(0)]


@op(0x1008)
def fs_rmdir(s, path, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    try:
        os.rmdir(p.rstrip('/'))
    except OSError as e:
        return [u32(err_conv(e.errno))]
    return [u32(0)]


@op(0x1009)
def fs_size(s, h, *_):
    f = s.get(h)
    if not f:
        return [u32(E_HANDLE), u32(0)]
    try:
        return [u32(0), u32(os.fstat(f[0]).st_size)]
    except OSError as e:
        return [u32(err_conv(e.errno)), u32(0)]


def walk_size(p):
    total = 0
    for d, _, names in os.walk(p):
        for n in names:
            try:
                total += os.lstat(os.path.join(d, n)).st_size
            except OSError:
                pass
    return total


@op(0x100a)
def fs_folder_size(s, path, flags, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    if not os.path.isdir(p):
        return [u32(E_NOT_FOUND)]
    return [u32(walk_size(p))]


@op(0x100b)
def fs_rename(s, path, new, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    n = wstr(new)
    q = os.path.join(os.path.dirname(p), n) if ':' not in n else s.path(n)
    if isinstance(q, int):
        return [u32(q)]
    try:
        os.rename(p, q)
    except OSError as e:
        return [u32(err_conv(e.errno))]
    return [u32(0)]


@op(0x100c)
def fs_move(s, src, dst, flags, *_):
    p, q = s.path(src), s.path(dst)
    for x in (p, q):
        if isinstance(x, int):
            return [u32(x)]
    try:
        os.replace(p, q)
    except OSError as e:
        return [u32(err_conv(e.errno))]
    return [u32(0)]


@op(0x100d)
def fs_count(s, path, flags, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    try:
        return [u32(len(os.listdir(p)))]
    except OSError as e:
        return [u32(err_conv(e.errno))]


@op(0x100e)
def fs_disk_info(s, *_):
    info = bytearray(0x54)
    try:
        st = os.statvfs(s.path('Z:\\'))
    except OSError as e:
        return [u32(err_conv(e.errno)), bytes(info)]
    struct.pack_into('<IIII', info, 0x30, 0x200, st.f_bsize >> 9, st.f_blocks & 0xffffffff, 0)
    struct.pack_into('<I', info, 0x40, 0x200)
    return [u32(0), bytes(info)]


@op(0x100f)
def fs_delete(s, path, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    try:
        os.unlink(p)
    except OSError as e:
        return [u32(err_conv(e.errno))]
    return [u32(0)]


@op(0x1010)
def fs_attributes(s, path, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    try:
        st = os.stat(p)
    except OSError as e:
        return [u32(err_conv(e.errno))]
    ro = not os.access(p, os.W_OK)
    return [u32((1 if ro else 0) | (0x10 if stat.S_ISDIR(st.st_mode) else 0))]


@op(0x1011)
def fs_open_hint(s, path, flags, hint=b'\0' * 8, *_):
    return [u32(s.open(path, i32(flags))), hint[:8].ljust(8, b'\0')]


def attributes(p):
    """FAT attributes of a host path: 0x10 directory, 0x01 read-only"""
    st = os.stat(p)
    return (1 if not os.access(p, os.W_OK) else 0) | (0x10 if stat.S_ISDIR(st.st_mode) else 0)


def dos_time(t):
    t = time.localtime(t)
    return ((t.tm_year - 1980) << 25 | t.tm_mon << 21 | t.tm_mday << 16 |
            t.tm_hour << 11 | t.tm_min << 5 | (t.tm_sec & 0x1f)) & 0xffffffff


def dos_entry(p, complete):
    """FS_DOSDirEntry (0x34 bytes, ccci_fsd FS_EntryLinuxToDos): attributes,
    name-complete flag, access and modification time, size"""
    st = os.stat(p)
    e = bytearray(0x34)
    e[0xb] = attributes(p)
    e[0xc] = 1 if complete else 0
    struct.pack_into('<I', e, 0xe, dos_time(st.st_atime))
    struct.pack_into('<I', e, 0x16, dos_time(st.st_mtime))
    struct.pack_into('<I', e, 0x1c, st.st_size & 0xffffffff)
    return bytes(e)


def find_scan(s, h):
    """next matching entry of search handle h: [ret, entry, name]"""
    _, (d, it, pat, want, mask, maxlen), _ = s.files[h]
    import fnmatch
    for n in it:
        if n in ('.', '..') or not fnmatch.fnmatchcase(n.upper(), pat.upper()):
            continue
        p = os.path.join(d, n)
        try:
            a = attributes(p)
        except OSError:
            continue
        if want & ~a or a & mask:
            continue
        name = n[:maxlen] if maxlen else ''
        return [u32(h), dos_entry(p, len(n) < maxlen),
                (name.encode('utf-16-le') + b'\0\0') if maxlen else b'']
    return None


@op(0x1012)
def fs_find_first(s, pattern, attr, attr_mask, maxlen=b'\0' * 4, *_):
    p = s.path(pattern)
    if isinstance(p, int):
        return [u32(p), bytes(0x34), b'']
    d, pat = os.path.split(p)
    try:
        it = iter(os.listdir(d))
    except OSError as e:
        return [u32(err_conv(e.errno)), bytes(0x34), b'']
    try:
        h = s.files.index(None, 1)
    except ValueError:
        return [u32(E_TOO_MANY), bytes(0x34), b'']
    s.files[h] = (None, (d, it, pat, attr[0], attr_mask[0], i32(maxlen)), p)
    r = find_scan(s, h)
    if r is None:
        s.files[h] = None
        return [u32(E_NO_MORE), bytes(0x34), b'']
    return r


@op(0x1013)
def fs_find_next(s, h, maxlen=None, *_):
    f = s.get(h)
    if not f or f[0] is not None:
        return [u32(E_HANDLE), bytes(0x34), b'']
    if maxlen is not None:
        st = f[1]
        s.files[i32(h)] = (None, st[:5] + (i32(maxlen),), f[2])
    r = find_scan(s, i32(h))
    if r is None:
        return [u32(E_NO_MORE), bytes(0x34), b'']
    r[0] = u32(0)
    return r


@op(0x1014)
def fs_find_close(s, h, *_):
    f = s.get(h)
    if not f or f[0] is not None:
        return [u32(E_HANDLE)]
    s.files[i32(h)] = None
    return [u32(0)]


@op(0x1015)	# LockFAT
@op(0x1019)	# ClearDiskFlag
@op(0x101c)	# SetDiskFlag
def fs_ok(s, *_):
    return [u32(0)]


@op(0x1016)	# UnlockAll
def fs_unlock(s, *_):
    return [u32(1)]


@op(0x1017)	# Shutdown
def fs_shutdown(s, *_):
    s.close_all()
    return []


@op(0x1018)
def fs_xdelete(s, path, flags, *_):
    import shutil
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p)]
    try:
        if os.path.isdir(p):
            n = sum(len(f) + len(d) for _, d, f in os.walk(p))
            shutil.rmtree(p)
        else:
            os.unlink(p)
            n = 1
    except OSError as e:
        return [u32(err_conv(e.errno))]
    return [u32(n)]


@op(0x101a)
def fs_get_drive(s, typ, *_):
    return [u32(ord('Z') if i32(typ) == 4 else E_PARAM)]


@op(0x101b)
def fs_cluster_size(s, *_):
    return [u32(0x200)]


@op(0x1021)	# Restore (from the binary NVRAM backup): not kept on Arch
def fs_restore(s, *_):
    return [u32(E_NOT_FOUND)]


@op(0x1022)
def fs_cmpt_read(s, path, params, *_):
    """open, size, seek, read, close in one: params u32[10] = opid_map,
    -, -, open flags, -, offset, whence, -, length; returns
    [step done, error], file size, read count, data"""
    p = struct.unpack_from('<10I', params.ljust(40, b'\0'))
    opid, flags, off, whence, length = p[0], p[3], p[5], p[6], p[8]
    size = count = 0
    data = b''

    def result(step, err):
        return [struct.pack('<Ii', step, err), u32(size), u32(count), data]

    if not opid & 1:
        return result(0, -7)
    h = s.open(path, flags)
    if h < 0:
        return result(0, h)
    fd = s.files[h][0]
    step = 1
    if opid & 2:
        try:
            size = os.fstat(fd).st_size
        except OSError as e:
            s.close(h)
            return result(1, err_conv(e.errno))
        step = 3
    if opid & 4:
        try:
            os.lseek(fd, struct.unpack('<i', struct.pack('<I', off))[0], whence if whence < 3 else 0)
        except OSError as e:
            s.close(h)
            return result(step, err_conv(e.errno))
        step |= 4
    if opid & 8:
        try:
            data = os.read(fd, length)
        except OSError as e:
            s.close(h)
            return result(step, err_conv(e.errno))
        count = len(data)
        step |= 8
    if not opid & 0x10:
        s.close(h)
        return result(step, -6)
    r = s.close(h)
    if r < 0:
        return result(step, r)
    return result(step | 0x10, 0)


@op(0x1023)	# BinRegionAccess
def fs_bin_region(s, *_):
    return [u32(E_PARAM)]


@op(0x1024)
def fs_cmpt_write(s, path, params, data=b'', *_):
    """the write counterpart: params opid_map, -, -, flags, offset, whence,
    -, length; returns [step done, error], written, -"""
    p = struct.unpack_from('<10I', params.ljust(40, b'\0'))
    opid, flags, off, whence, length = p[0], p[3], p[4], p[5], p[7]
    written = 0

    def result(step, err):
        return [struct.pack('<Ii', step, err), u32(written), u32(0)]

    h = s.open(path, flags)
    if h < 0:
        return result(0, h)
    fd = s.files[h][0]
    step = 1
    try:
        if opid & 4:
            os.lseek(fd, struct.unpack('<i', struct.pack('<I', off))[0], whence if whence < 3 else 0)
            step |= 4
        if opid & 0x20:
            written = os.write(fd, data[:length])
            step |= 0x20
    except OSError as e:
        s.close(h)
        return result(step, err_conv(e.errno))
    r = s.close(h)
    return result(step | 0x10, 0 if r >= 0 else r)


@op(0x1025)
def fs_file_detail(s, path, *_):
    p = s.path(path)
    if isinstance(p, int):
        return [u32(p), bytes(24)]
    try:
        st = os.stat(p)
    except OSError as e:
        return [u32(err_conv(e.errno)), bytes(24)]
    return [u32(0), struct.pack('<qqq', int(st.st_mtime), int(st.st_atime), int(st.st_ctime))]
