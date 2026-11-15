#!/usr/bin/env python3
# Extract files from Blue Dragon IPK1 archives (zlib entries), recursively (ipk inside ipk).
# usage: ipk_extract.py <pack dir or .ipk> <out dir> [ext,ext,...]   (default: vso,pso)
import os, struct, sys, zlib
def entries(d):
    if d[:4] != b'IPK1': return
    cnt = struct.unpack_from('<I', d, 8)[0]
    for k in range(cnt):
        e = 0x10 + k * 0x60
        name = d[e:e + 0x40].split(b'\0')[0].decode('latin1')
        fl, csz, off, usz = struct.unpack_from('<IIII', d, e + 0x40)
        raw = d[off:off + csz]
        try: data = zlib.decompress(raw) if fl == 1 else d[off:off + usz]
        except Exception: data = None
        yield name, data
def walk(d, prefix, out, exts, stats):
    for name, data in entries(d):
        if data is None: stats['bad'] += 1; continue
        ext = name.rsplit('.', 1)[-1].lower()
        if data[:4] == b'IPK1' or ext == 'ipk': walk(data, prefix + '/' + name.replace('\\', '_'), out, exts, stats); continue
        if ext in exts:
            p = os.path.join(out, prefix.strip('/'), name.replace('\\', '/'))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(data); stats['n'] += 1
src, out = sys.argv[1], sys.argv[2]
exts = set((sys.argv[3] if len(sys.argv) > 3 else 'vso,pso').split(','))
stats = {'n': 0, 'bad': 0, 'ipk': 0}
files = [src] if os.path.isfile(src) else [os.path.join(r, f) for r, _, fs in os.walk(src) for f in fs if f.lower().endswith('.ipk')]
for f in sorted(files):
    stats['ipk'] += 1
    walk(open(f, 'rb').read(), os.path.relpath(f, src if os.path.isdir(src) else os.path.dirname(f)).replace('\\', '_'), out, exts, stats)
print(stats)
