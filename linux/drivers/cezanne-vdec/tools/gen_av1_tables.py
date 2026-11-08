#!/usr/bin/env python3
"""Generate vdec_av1_tables.c: the AV1 default CDF tables and the IQ table
in the layouts the MT6885 vdec reads, from the AV1 specification's data.

  gen_av1_tables.py LINUX_SRC DAV1D_SRC > vdec_av1_tables.c

Sources (read as text, nothing is compiled):
- LINUX_SRC/drivers/media/platform/verisilicon/rockchip_av1_entropymode.{h,c}:
  the spec default CDFs (from libaom, BSD-2-Clause), as inverse CDFs
- DAV1D_SRC/src/qm.c, DAV1D_SRC/src/dequant_tables.c (dav1d 1.4.3,
  BSD-2-Clause): the spec quantizer matrices and Dc_Qlookup / Ac_Qlookup
- av1_cdf_layout.txt (next to this script): where each CDF vector sits in
  the hardware's 0x3fa0-byte context buffer

CDF buffer: four sets (q contexts 0..3 of the coefficient CDFs), each
0x1fd0 u16; every vector is followed by a 0 u16. The coefficient tables
(av1_default_*) move by a quarter of their size per q context.

IQ table (0x12200 bytes): 8-bit then 10-bit lookups, each 0x400 bytes of
u16 groups dc_q[i..i+3], ac_q[i..i+3]; then 32 quantizer matrices of
0x8d0 bytes, luma levels 0..15 then chroma 0..15 (level 15 flat 32), each
4x4, 8x8, 32x32, 4x8, 8x4, 16x32, 32x16, 4x16 in raster order.
"""
import os, re, sys

CDF_SET = 0x3fa0 // 2


def strip_comments(t):
    t = re.sub(r'/\*.*?\*/', ' ', t, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', t)


def macros(text):
    m = {}
    # object-like macros only (a function-like one has '(' right after the name)
    for name, val in re.findall(r'^#define\s+(\w+)[ \t]+([^\n]+)$', text, re.M):
        m[name] = val.strip()
    for body in re.findall(r'\benum\s*\w*\s*\{([^}]*)\}', text):
        n = 0
        for item in body.split(','):
            item = item.strip()
            if not item:
                continue
            if '=' in item:
                name, val = [x.strip() for x in item.split('=', 1)]
                n = ev(val, m)
            else:
                name = item
            m[name] = str(n)
            n += 1
    return m


def ev(expr, m):
    expr = re.sub(r'CDF_SIZE\(([^()]+)\)', r'((\1) - 1)', expr)
    for _ in range(10):
        n = re.sub(r'\b([A-Z_][A-Z0-9_]*)\b', lambda x: '(%s)' % m[x.group(1)] if x.group(1) in m else x.group(1), expr)
        if n == expr:
            break
        expr = n
    return int(eval(expr.replace('U', '').strip(), {}))


def tokens(body, m):
    i, out = 0, []
    pat = re.compile(r'\s*(AOM_CDF(\d+)\(((?:[^()]|\([^()]*\))*)\)|(?:AOM_)?ICDF\(((?:[^()]|\([^()]*\))*)\)|\{|\}|,|-?\d+U?)')
    while i < len(body):
        mt = pat.match(body, i)
        if not mt:
            if body[i:].strip() == '':
                break
            raise ValueError('cannot parse near %r' % body[i:i + 40])
        i = mt.end()
        tok = mt.group(1)
        if mt.group(2):
            out.append([32768 - ev(a, m) for a in mt.group(3).split(',')])
        elif mt.group(4) is not None:
            out.append([32768 - ev(mt.group(4), m)])
        elif tok in '{},':
            out.append(tok)
        else:
            out.append([int(tok.rstrip('U'))])
    return out


def nest(toks):
    stack = [[]]
    for t in toks:
        if t == '{':
            stack.append([])
        elif t == '}':
            x = stack.pop()
            stack[-1].append(x)
        elif t != ',':
            stack[-1].extend(t)
    return stack[0][0]


def flatten(v, dims):
    if not dims:
        return [v]
    if dims[0] is None:             # [] : sized by the initializer
        dims = [len(v)] + dims[1:]
    if len(dims) == 1:
        assert all(isinstance(x, int) for x in v)
        return v + [0] * (dims[0] - len(v))
    sub = 1
    for d in dims[1:]:
        sub *= d
    out = []
    for x in v:
        out += flatten(x, dims[1:])
    return out + [0] * (dims[0] * sub - len(out))


def spec_cdfs(linux):
    d = os.path.join(linux, 'drivers/media/platform/verisilicon')
    h = strip_comments(open(os.path.join(d, 'rockchip_av1_entropymode.h')).read())
    c = strip_comments(open(os.path.join(d, 'rockchip_av1_entropymode.c')).read())
    m = macros(h)
    m.update(macros(c))
    tabs = {}
    for name, dims, body in re.findall(r'static const u16 (\w+)\s*((?:\[[^\]]*\]\s*)+)=\s*(\{.*?\});', c, re.S):
        dl = [ev(x, m) if x.strip() else None for x in re.findall(r'\[([^\]]*)\]', dims)]
        tabs[name] = flatten(nest(tokens(body, m)), dl)
    return tabs


def cdf_tables(linux, layout):
    tabs = spec_cdfs(linux)
    out = []
    for q in range(4):
        s = [0] * CDF_SET
        for l in open(layout):
            l = l.split('#')[0].split()
            if not l:
                continue
            st, name, off, ln, cnt = int(l[0]), l[1], int(l[2]), int(l[3]), int(l[4])
            for k in range(cnt):
                p = st + k * (ln + 1)
                if name == 'uniform':
                    # off: 1 = rounded, 0 = truncated
                    n = ln + 1
                    v = [(32768 * (n - 1 - i) + (n // 2 if off else 0)) // n for i in range(ln)]
                else:
                    t = tabs[name]
                    o = off + k * ln + (q * len(t) // 4 if name.startswith('av1_default_') else 0)
                    v = t[o:o + ln]
                s[p:p + ln] = v
        out.append(s)
    return out


def numbers(text, start):
    body = text[text.index(start):]
    body = body[body.index('{'):]
    depth = 0
    for i, ch in enumerate(body):
        depth += ch == '{'
        depth -= ch == '}'
        if not depth:
            break
    return list(map(int, re.findall(r'\b\d+\b', strip_comments(body[:i + 1]))))


def qm_tables(dav1d):
    t = open(os.path.join(dav1d, 'src/qm.c')).read()
    q3216 = numbers(t, 'qm_tbl_32x16[][2][512]')
    q32t = numbers(t, 'qm_tbl_32x32_t[][2][528]')
    assert len(q3216) == 15 * 2 * 512 and len(q32t) == 15 * 2 * 528

    def untriangle(src):
        dst, s = [], 0
        for y in range(32):
            row = src[s:s + y + 1]
            p = s + y
            for x in range(y + 1, 32):
                p += x
                row.append(src[p])
            dst += row
            s += y + 1
        return dst

    def sub(src, off, h, hs, vs):
        return [src[off + y * 32 + x] for y in range(0, h, vs) for x in range(0, 32, hs)]

    def tr(src, w, h):
        dst = [0] * (w * h)
        for y in range(h):
            for x in range(w):
                dst[x * h + y] = src[y * w + x]
        return dst

    out = {}
    for lv in range(15):
        for pl in range(2):
            i = lv * 2 + pl
            a = untriangle(q32t[i * 528:(i + 1) * 528])
            b = q3216[i * 512:(i + 1) * 512]
            m84 = sub(b, 33, 16, 4, 4)
            m164 = sub(b, 32, 16, 2, 4)
            # dav1d keeps coefficients transposed; these are the spec's
            # raster orders (Quantizer_Matrix)
            out[lv, pl] = (sub(a, 99, 32, 8, 8) + sub(a, 33, 32, 4, 4) + a + tr(m84, 8, 4) + m84 +
                           tr(b, 32, 16) + b + tr(m164, 16, 4))
    return out


def iq_table(dav1d):
    dq = numbers(open(os.path.join(dav1d, 'src/dequant_tables.c')).read(), 'dav1d_dq_tbl')
    assert len(dq) == 3 * 256 * 2
    lut = []
    for b in range(2):
        for q in range(0, 256, 4):
            lut += [dq[(b * 256 + q + i) * 2] for i in range(4)] + [dq[(b * 256 + q + i) * 2 + 1] for i in range(4)]
    qm = qm_tables(dav1d)
    mats = []
    for pl in range(2):
        for lv in range(16):
            m = qm[lv, pl] if lv < 15 else [32] * 0x8d0
            assert len(m) == 0x8d0
            mats += m
    return lut, mats


def carray(vals, per, fmt):
    lines = []
    for i in range(0, len(vals), per):
        lines.append('\t' + ' '.join(fmt % v + ',' for v in vals[i:i + per]))
    return '\n'.join(lines)


def main():
    linux, dav1d = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    cdf = cdf_tables(linux, os.path.join(here, 'av1_cdf_layout.txt'))
    lut, mats = iq_table(dav1d)
    print('// SPDX-License-Identifier: GPL-2.0')
    print('/*')
    print(' * Generated by tools/gen_av1_tables.py: AV1 default CDFs (libaom data via')
    print(' * the kernel\'s rockchip_av1_entropymode.c) and dequantizer lookups and')
    print(' * quantizer matrices (dav1d data), in the MT6885 vdec buffer layouts.')
    print(' * Do not edit.')
    print(' */')
    print('#include "vdec.h"\n#include "vdec_av1.h"\n')
    print('const u16 vdec_av1_cdf_default[4][VDEC_AV1_CDF_SIZE / 2] = {')
    for s in cdf:
        print('{\n' + carray(s, 12, '%5d') + '\n},')
    print('};\n')
    print('const u16 vdec_av1_iq_lut[2][512] = {')
    for b in range(2):
        print('{\n' + carray(lut[b * 512:(b + 1) * 512], 8, '%5d') + '\n},')
    print('};\n')
    print('const u8 vdec_av1_qm[32][VDEC_AV1_QM_SIZE] = {')
    for i in range(32):
        print('{\n' + carray(mats[i * 0x8d0:(i + 1) * 0x8d0], 16, '%3d') + '\n},')
    print('};')


if __name__ == '__main__':
    main()
