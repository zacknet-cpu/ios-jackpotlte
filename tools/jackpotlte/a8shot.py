#!/usr/bin/env python3
"""Turn the last "shot" in a USB console capture (or a decoded kernel log)
into a PNG.

The phone's kernel shell answers "shot [SCALE]" with
    A8SHOT BEGIN W H
    A8SHOT:<base64, 4 characters a pixel, RGB>
    ...
    A8SHOT END
(bsd/dev/a5xelte_usb.c, patch 0186). Lines from other threads may come in
between; only A8SHOT: lines are taken.

usage: a8shot.py serial.log [out.png] [--index N]   (N: 1 = last shot, 2 = the one before, ...)
"""
import base64
import struct
import sys
import zlib


def shots(text):
    found = []
    cur = None
    for line in text.splitlines():
        line = line.strip("\r")
        at = line.find("A8SHOT")
        if at < 0:
            continue
        line = line[at:]
        if line.startswith("A8SHOT BEGIN"):
            parts = line.split()
            cur = [int(parts[2]), int(parts[3]), []]
        elif line.startswith("A8SHOT:") and cur is not None:
            cur[2].append(line[7:].strip())
        elif line.startswith("A8SHOT END") and cur is not None:
            found.append(cur)
            cur = None
    return found


def png(width, height, rgb):
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--index")]
    index = 1
    for a in sys.argv[1:]:
        if a.startswith("--index"):
            index = int(a.split("=", 1)[1]) if "=" in a else 1
    if not args:
        print(__doc__)
        return 2
    text = open(args[0], "rb").read().decode("latin-1")
    out = args[1] if len(args) > 1 else "shot.png"
    found = shots(text)
    if len(found) < index:
        print("no complete shot (%d found)" % len(found))
        return 1
    width, height, lines = found[-index]
    data = "".join(lines)
    rgb = base64.b64decode(data[: len(data) // 4 * 4])
    need = width * height * 3
    if len(rgb) < need:
        print("shot is short: %d of %d bytes (lines lost?); padding with black" % (len(rgb), need))
        rgb += b"\x00" * (need - len(rgb))
    open(out, "wb").write(png(width, height, rgb[:need]))
    print("%s: %dx%d (shot %d of %d)" % (out, width, height, len(found) - index + 1, len(found)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
