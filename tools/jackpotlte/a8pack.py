#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack a rebuilt Mesa for the GL shim (glca18+) to unpack on the phone.

The console's fput takes at most 4 MiB a file, so a library ships compressed,
in parts: OUT.0, OUT.1, ... joined are "A8Z1", the library's size (8 bytes,
little-endian), its SHA-256 (32 bytes), then a zlib stream (window bits 15).
The shim (jackpotlte_gl_osmesa.c, mesa_next()) inflates the parts once into
libOSMesa.8.next.dylib and keeps the hash beside it; a package with another
hash is unpacked again.

usage: a8pack.py LIBRARY OUT_PREFIX     (e.g. ... libOSMesa.8.next.z)
"""
import hashlib
import struct
import sys
import zlib

PART = 4 * 1024 * 1024 - 4096


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    data = open(sys.argv[1], 'rb').read()
    blob = (b'A8Z1' + struct.pack('<Q', len(data)) + hashlib.sha256(data).digest() +
            zlib.compress(data, 9))
    # check the round trip before anything ships
    assert blob[:4] == b'A8Z1' and zlib.decompress(blob[44:]) == data
    parts = [blob[i:i + PART] for i in range(0, len(blob), PART)]
    for n, p in enumerate(parts):
        with open('%s.%d' % (sys.argv[2], n), 'wb') as f:
            f.write(p)
        print('%s.%d: %d bytes, sha256 %s' % (sys.argv[2], n, len(p), hashlib.sha256(p).hexdigest()))
    print('library %d bytes, sha256 %s; %d part(s)' % (len(data), hashlib.sha256(data).hexdigest(), len(parts)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
