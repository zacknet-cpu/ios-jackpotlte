#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Two-slot BOOT payload for the jackpotlte loader (Phase 2).

The loader (shim/jackpotlte/loader.c) takes a "SWEETSLT" container from the
ramdisk. Slot A is the known-good kernel, slot B the one under test. Each slot
holds one ordinary SWEETXNU v1 payload (tools/mkpayload.py), raw-deflated.
The loader boots A unless the RAM flag at 0xed3ff000 asks for B once; it
clears the flag before it decodes B, so any failure of B ends on A.

Container (little endian), 4096-byte header. Every record sits in its own
512-byte sector, so the phone rewrites one slot with a single sector write and
a torn write can only spoil that one record (its CRC then fails).
  0    char[8] "SWEETSLT", u32 version (2), u32 header size (4096),
       u32 slot count (2), u32 flags (0), u32 crc32 of bytes 0..23
  512  slot 0 record, 1024 slot 1 record, 128 bytes each:
         u64 offset (from the container start, 4 KiB aligned), u64 capacity,
         u64 compressed size (0 = empty), u64 payload size,
         u32 codec (1 raw deflate), u32 crc32 compressed, u32 crc32 payload,
         u32 generation, u8[32] sha256 compressed, char[32] name,
         u32 good, u32 0, u32 0, u32 crc32 of record bytes 0..123
The loader boots the good slot with the highest generation (the default). A
"try once" RAM flag can name the other slot; it is wiped before that slot is
read, so any failure ends back on the default. Promotion after a passing score
is one record write: good = 1 and a generation above the default's.

  mkslots.py container OUT --a PAYLOAD_A [--a-name N] [--b PAYLOAD_B]
             [--b-name N] [--cap BYTES]
  mkslots.py blob OUT.slot PAYLOAD [--name N]   (a slot image for "bput")
"""
import argparse
import hashlib
import struct
import sys
import zlib

MAGIC = b"SWEETSLT"
VERSION = 2
HDR = 4096
REC = 128
ALIGN = 4096
CODEC_DEFLATE = 1
# Both slots get the same capacity: the compressed kernel plus headroom, so
# either slot can take the next kernel after a promotion. 2 x 7.6 MB keeps the
# ramdisk below the 16.7 MB that S-Boot is known to load (0193e).
HEADROOM = 3 << 19
CAP_ALIGN = 1 << 16


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return c.compress(data) + c.flush()


def slot_record(off, cap, comp, payload, gen, name, good):
    if comp is None:
        rec = struct.pack("<QQQQIIII32s32sIII", off, cap, 0, 0, 0, 0, 0, 0,
                          b"", b"", 0, 0, 0)
    else:
        rec = struct.pack("<QQQQIIII32s32sIII", off, cap, len(comp),
                          len(payload), CODEC_DEFLATE, zlib.crc32(comp),
                          zlib.crc32(payload), gen,
                          hashlib.sha256(comp).digest(), name.encode()[:31],
                          good, 0, 0)
    return rec + struct.pack("<I", zlib.crc32(rec))


def check_payload(data, what):
    if data[:8] != b"SWEETXNU" or struct.unpack_from("<I", data, 8)[0] != 1:
        sys.exit("%s is not a SWEETXNU v1 payload" % what)


def roundup(n):
    return (n + ALIGN - 1) // ALIGN * ALIGN


def container(args):
    a = open(args.a, "rb").read()
    check_payload(a, args.a)
    ca = deflate(a)
    b = cb = None
    if args.b:
        b = open(args.b, "rb").read()
        check_payload(b, args.b)
        cb = deflate(b)
    need = max(len(ca), len(cb or b""))
    cap = args.cap or (need + HEADROOM + CAP_ALIGN - 1) // CAP_ALIGN * CAP_ALIGN
    if cap % ALIGN or need > cap:
        sys.exit("capacity %d: must be 4 KiB aligned and hold %d" % (cap, need))
    cap_a = cap_b = cap
    off_a = HDR
    off_b = HDR + cap_a
    hdr = bytearray(HDR)
    struct.pack_into("<8sIIII", hdr, 0, MAGIC, VERSION, HDR, 2, 0)
    struct.pack_into("<I", hdr, 24, zlib.crc32(bytes(hdr[:24])))
    hdr[512:512 + REC] = slot_record(off_a, cap_a, ca, a, 1, args.a_name, 1)
    hdr[1024:1024 + REC] = slot_record(off_b, cap_b, cb, b, 0, args.b_name, 0)
    out = bytearray(hdr)
    out += ca + bytes(cap_a - len(ca))
    out += (cb or b"") + bytes(cap_b - len(cb or b""))
    open(args.out, "wb").write(out)
    print("container %s: %d bytes; A %d -> %d (cap %d); B %s (cap %d at +%d)" % (
        args.out, len(out), len(a), len(ca), cap_a,
        "%d -> %d" % (len(b), len(cb)) if b else "empty", cap_b, off_b))


def blob(args):
    """A slot image: a 128-byte record (offset, capacity, generation and
    good left 0; the phone fills them in and redoes the record CRC) followed
    by the compressed payload."""
    p = open(args.payload, "rb").read()
    check_payload(p, args.payload)
    c = deflate(p)
    rec = slot_record(0, 0, c, p, 0, args.name, 0)
    open(args.out, "wb").write(rec + c)
    print("slot image %s: payload %d -> %d bytes, sha256 %s" % (
        args.out, len(p), len(c), hashlib.sha256(c).hexdigest()))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("container")
    c.add_argument("out")
    c.add_argument("--a", required=True)
    c.add_argument("--a-name", default="A")
    c.add_argument("--b")
    c.add_argument("--b-name", default="B")
    c.add_argument("--cap", type=int, default=0)
    b = sub.add_parser("blob")
    b.add_argument("out")
    b.add_argument("payload")
    b.add_argument("--name", default="B")
    args = ap.parse_args()
    container(args) if args.cmd == "container" else blob(args)


if __name__ == "__main__":
    main()
