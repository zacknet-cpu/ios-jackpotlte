#!/usr/bin/env python3
"""Cut the ACPM "FVP" plug-in out of the phone's own stock BOOT image.

The Exynos 7885's power manager (ACPM) takes clock requests through a plug-in
that S-Boot does not start: stock Linux carries it as built-in firmware
(firmware/exynos7885_acpm_fvp.fw.ihex in Samsung's kernel source) and copies
it into the APM's SRAM at boot.  Kernel 0192's "acpm fvp PATH" does the same
from a file on the iOS data volume, and refuses anything but these 12288
bytes.  The file is not in this repository: it comes from the owner's phone.

    acpm-fvp-from-boot.py BOOT.img exynos7885_acpm_fvp.fw
    python a8file.py put exynos7885_acpm_fvp.fw /private/var/root/a8-acpm-fvp.fw 644 0 0

Then on the console:  acpm fvp /private/var/root/a8-acpm-fvp.fw
"""
import hashlib
import sys
import zlib

SIZE = 12288
CRC32 = 0x14F4D437
SHA256 = "3793478b6df52fe760a89cbcfb258a9c489365ee2d13d9dae8df2f161abadb61"
# How the plug-in begins (Thumb code): enough to find candidates quickly.
HEAD = bytes.fromhex("01229c2108b5064b")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    boot = open(sys.argv[1], "rb").read()
    at = -1
    while True:
        at = boot.find(HEAD, at + 1)
        if at < 0:
            sys.exit("no plug-in with crc32 %08x in %s" % (CRC32, sys.argv[1]))
        blob = boot[at:at + SIZE]
        if len(blob) == SIZE and zlib.crc32(blob) == CRC32:
            break
    digest = hashlib.sha256(blob).hexdigest()
    if digest != SHA256:
        sys.exit("crc32 matches at 0x%x but sha256 is %s" % (at, digest))
    open(sys.argv[2], "wb").write(blob)
    print("%s: %d bytes from 0x%x of %s, crc32 %08x, sha256 %s" % (
        sys.argv[2], SIZE, at, sys.argv[1], CRC32, digest))


main()
