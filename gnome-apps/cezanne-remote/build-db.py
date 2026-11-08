#!/usr/bin/env python3
"""Build cezanne-remote's database from IRext (https://github.com/irext/database,
MIT): TV remotes with their decoded keys, AC remotes with the protocol binaries
the IRext decoder turns into codes for a given state.

build-db.py irext_db_sqlite3.db irext-binaries.zip out.db

Variants that send identical codes (TV) or share a binary (AC) within a brand
are merged, so trying the next remote always tries something different.
"""
import array, hashlib, sqlite3, sys, zipfile, zlib

CAT_AC, CAT_TV = 1, 2
TV_KEYS = ['power', 'mute', 'up', 'down', 'left', 'right', 'ok', 'vol+', 'vol-', 'back',
           'input', 'menu', 'home', 'set', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9']


def pack(durations):
    """microseconds as LIRC takes them: zero entries merged into their
    neighbours, starting and ending on a pulse, at most 500 ms an entry and
    1023 entries. None for IRext's placeholders ("0") of keys a remote lacks"""
    seq = []
    for i, v in enumerate(int(x) for x in durations.split(',') if x.strip()):
        if v <= 0:
            continue
        if seq and seq[-1][0] == i % 2:
            seq[-1][1] += v
        else:
            seq.append([i % 2, v])
    while seq and seq[0][0] == 1:
        seq.pop(0)
    while seq and seq[-1][0] == 1:
        seq.pop()
    d = [min(v, 500000) for _, v in seq]
    if len(d) < 3 or len(d) > 1023:
        return None
    return zlib.compress(array.array('I', d).tobytes(), 9)


def main():
    src = sqlite3.connect(sys.argv[1])
    bins = zipfile.ZipFile(sys.argv[2])
    names = {n.rsplit('/', 1)[-1]: n for n in bins.namelist() if n.endswith('.bin')}
    out = sqlite3.connect(sys.argv[3])
    out.executescript("""
        drop table if exists brand; drop table if exists remote;
        drop table if exists tv_key; drop table if exists ac_binary;
        create table brand (id integer primary key, category integer, name text, priority integer);
        create table remote (id integer primary key, brand integer, position integer, protocol text, name text);
        create table tv_key (remote integer, key text, code blob, primary key (remote, key));
        create table ac_binary (remote integer primary key, data blob);
    """)
    for cat in (CAT_TV, CAT_AC):
        brands = src.execute("select id, coalesce(nullif(name_en, ''), name), coalesce(priority, 999) "
                             "from brand where category_id = ? and status = 1", (cat,)).fetchall()
        for bid, bname, bprio in brands:
            remotes = src.execute("select id, protocol, remote from remote_index where brand_id = ? "
                                  "and status = 1 order by coalesce(priority, 999), id", (bid,)).fetchall()
            seen, kept = set(), []
            for rid, proto, remote in remotes:
                if cat == CAT_TV:
                    keys = dict(src.execute("select key_name, key_value from decode_remote "
                                            "where remote_index_id = ?", (rid,)).fetchall())
                    payload = {k: pack(keys[k]) for k in TV_KEYS if keys.get(k)}
                    payload = {k: v for k, v in payload.items() if v}
                    if 'power' not in payload:
                        continue
                    sig = hashlib.sha1(b''.join(payload[k] for k in sorted(payload))).digest()
                else:
                    f = names.get('irda_%s_%s.bin' % (proto, remote))
                    if not f:
                        continue
                    payload = bins.read(f)
                    sig = hashlib.sha1(payload).digest()
                if sig in seen:
                    continue
                seen.add(sig)
                kept.append((rid, proto, remote, payload))
            if not kept:
                continue
            out.execute("insert into brand values (?, ?, ?, ?)", (bid, cat, bname.strip(), bprio))
            for pos, (rid, proto, remote, payload) in enumerate(kept, 1):
                out.execute("insert into remote values (?, ?, ?, ?, ?)", (rid, bid, pos, proto, remote))
                if cat == CAT_TV:
                    out.executemany("insert into tv_key values (?, ?, ?)",
                                    [(rid, k, v) for k, v in payload.items()])
                else:
                    out.execute("insert into ac_binary values (?, ?)", (rid, payload))
    out.commit()
    for cat, name in ((CAT_TV, 'TV'), (CAT_AC, 'AC')):
        b, r = out.execute("select count(distinct b.id), count(r.id) from brand b join remote r "
                           "on r.brand = b.id where b.category = ?", (cat,)).fetchone()
        print('%s: %d brands, %d remotes' % (name, b, r))
    out.execute("vacuum")


if __name__ == '__main__':
    main()
