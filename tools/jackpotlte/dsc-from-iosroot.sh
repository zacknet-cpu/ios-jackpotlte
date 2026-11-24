#!/bin/bash
# Pull the iOS dyld shared cache out of an IOSRoot*.img (HFSX) and lay it out
# under the names blacktop's ipsw expects, for symbolicating phone samples.
#   dsc-from-iosroot.sh IOSRoot.img OUTDIR    (needs 7zz 24+, python3, ~3.5 GB)
# The cache files in the image are HFS+ hard links: 7-Zip shows them as empty
# and keeps the data in "[HFS+ Private Data]/iNodeN", so extract the iNodes
# and name them by the UUIDs in the main cache header.
# Then: ipsw dyld a2s OUTDIR/named/dyld_shared_cache_arm64 ADDR --slide SLIDE
# (SLIDE is printed by the console's "sample" command; use -q with disass).
set -euo pipefail
img=$1; out=$2
mkdir -p "$out/raw" "$out/named"
7zz e -y -aos -o"$out/raw" "$img" -r '*iNode*' >/dev/null
python3 - "$out" <<'PY'
import os, struct, sys
out = sys.argv[1]; raw = os.path.join(out, 'raw'); byuuid = {}; main = None
for f in os.listdir(raw):
    with open(os.path.join(raw, f), 'rb') as fp:
        h = fp.read(0x200)
    if not h.startswith(b'dyld_v1'):
        continue
    byuuid[h[0x58:0x68]] = f
    if 0 < struct.unpack_from('<I', h, 0x18c)[0] < 100:
        main = (f, h)
f, h = main
base = 'dyld_shared_cache_arm64e' if b'arm64e' in h[:16] else 'dyld_shared_cache_arm64'
off, cnt = struct.unpack_from('<II', h, 0x188)
data = open(os.path.join(raw, f), 'rb').read(off + cnt * 56)
moves = {f: base}
for i in range(cnt):
    e = data[off + i * 56: off + (i + 1) * 56]
    moves[byuuid[e[:16]]] = base + e[24:56].split(b'\0')[0].decode()
moves[byuuid[h[0x190:0x1a0]]] = base + '.symbols'
for src, dst in moves.items():
    os.replace(os.path.join(raw, src), os.path.join(out, 'named', dst))
print(len(moves), 'cache files in', os.path.join(out, 'named'))
PY
