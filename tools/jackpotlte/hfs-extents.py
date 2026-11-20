#!/usr/bin/env python3
"""Find a file's data extents in an HFS+/HFSX image (read-only).

usage: hfs-extents.py IMAGE /path/in/volume

Used for the in-place GL helper update (a8-gpu2): the iOS root went to
USERDATA at offset 0, so the extents printed here are the phone's too.

Prints the file's logical size, its extents (start block, block count) and
their byte offsets in the image, plus the sha256 of the file's data as read
through those extents.  Only the catalog B-tree is used; a file with more than
8 extents (overflow extents file) is reported and refused.
"""
import hashlib
import struct
import sys


def main():
    img_path, want = sys.argv[1], sys.argv[2]
    f = open(img_path, 'rb')

    def read_at(off, n):
        f.seek(off)
        return f.read(n)

    vh = read_at(1024, 512)
    sig = vh[:2]
    if sig not in (b'H+', b'HX'):
        sys.exit('not HFS+: %r' % sig)
    block = struct.unpack('>I', vh[0x28:0x2c])[0]
    cat_fork = vh[0x110:0x160]
    cat_ext = []
    for i in range(8):
        s, c = struct.unpack('>II', cat_fork[16 + 8 * i:24 + 8 * i])
        if c:
            cat_ext.append((s, c))

    def cat_read(off, n):
        """Read n bytes at logical offset off of the catalog file."""
        out = b''
        while n > 0:
            blk, inner = divmod(off, block)
            base = 0
            phys = None
            for s, c in cat_ext:
                if blk < base + c:
                    phys = s + (blk - base)
                    break
                base += c
            if phys is None:
                sys.exit('catalog offset %d beyond its extents' % off)
            take = min(n, block - inner)
            out += read_at(phys * block + inner, take)
            off += take
            n -= take
        return out

    hdr = cat_read(0, 512)
    (depth, root, leafrecs, firstleaf, lastleaf, nodesize, maxkey,
     totalnodes, freenodes) = struct.unpack('>HIIIIHHII', hdr[14:14 + 30])
    keycmp = hdr[14 + 37]
    binary = sig == b'HX' and keycmp == 0xCF

    def node(n):
        return cat_read(n * nodesize, nodesize)

    def records(nd):
        nrec = struct.unpack('>H', nd[10:12])[0]
        offs = [struct.unpack('>H', nd[nodesize - 2 * (i + 1):nodesize - 2 * i])[0]
                for i in range(nrec + 1)]
        return [nd[offs[i]:offs[i + 1]] for i in range(nrec)]

    def parse_key(rec):
        klen = struct.unpack('>H', rec[:2])[0]
        parent = struct.unpack('>I', rec[2:6])[0]
        nlen = struct.unpack('>H', rec[6:8])[0]
        name = rec[8:8 + 2 * nlen]
        return klen, parent, name

    def name_units(name_bytes):
        return [struct.unpack('>H', name_bytes[i:i + 2])[0] for i in range(0, len(name_bytes), 2)]

    def fold(units):
        # HFS+ case folding is a big table; for the ASCII names this tool is
        # used for, folding A-Z is enough.
        return [u + 32 if 0x41 <= u <= 0x5a else u for u in units]

    def key_cmp(a, b):
        if a[0] != b[0]:
            return -1 if a[0] < b[0] else 1
        ua, ub = name_units(a[1]), name_units(b[1])
        if not binary:
            ua, ub = fold(ua), fold(ub)
        return (ua > ub) - (ua < ub)

    def lookup(parent, name):
        target = (parent, name.encode('utf-16-be'))
        n = root
        while True:
            nd = node(n)
            kind = struct.unpack('>b', nd[8:9])[0]
            recs = records(nd)
            if kind == 0:  # index
                child = None
                for rec in recs:
                    klen, p, nm = parse_key(rec)
                    if key_cmp((p, nm), target) <= 0:
                        child = struct.unpack('>I', rec[2 + klen:6 + klen])[0]
                    else:
                        break
                if child is None:
                    return None
                n = child
            elif kind == -1:  # leaf
                for rec in recs:
                    klen, p, nm = parse_key(rec)
                    if key_cmp((p, nm), target) == 0:
                        return rec[2 + klen:]
                return None
            else:
                sys.exit('unexpected node kind %d' % kind)

    parent = 2
    parts = [p for p in want.split('/') if p]
    data = None
    for i, part in enumerate(parts):
        data = lookup(parent, part)
        if data is None:
            sys.exit('not found: %s (under parent id %d)' % ('/'.join(parts[:i + 1]), parent))
        rtype = struct.unpack('>H', data[:2])[0]
        if i < len(parts) - 1:
            if rtype != 1:
                sys.exit('%s is not a folder (record type %d)' % (part, rtype))
            parent = struct.unpack('>I', data[8:12])[0]
    rtype = struct.unpack('>H', data[:2])[0]
    if rtype != 2:
        sys.exit('%s is not a file (record type %d)' % (want, rtype))
    file_id = struct.unpack('>I', data[8:12])[0]
    fork = data[88:168]
    size = struct.unpack('>Q', fork[:8])[0]
    tblocks = struct.unpack('>I', fork[12:16])[0]
    ext = []
    for i in range(8):
        s, c = struct.unpack('>II', fork[16 + 8 * i:24 + 8 * i])
        if c:
            ext.append((s, c))
    if sum(c for _, c in ext) != tblocks:
        sys.exit('file has overflow extents (%d blocks, %d in the record): refusing' %
                 (tblocks, sum(c for _, c in ext)))
    h = hashlib.sha256()
    left = size
    for s, c in ext:
        take = min(left, c * block)
        h.update(read_at(s * block, take))
        left -= take
    print('volume %s, block size %d, catalog key compare 0x%02x' % (sig.decode(), block, keycmp))
    print('file id %d, logical size %d, %d blocks' % (file_id, size, tblocks))
    for s, c in ext:
        print('extent start_block %d count %d byte_offset %d' % (s, c, s * block))
    print('sha256 %s' % h.hexdigest())


if __name__ == '__main__':
    main()
