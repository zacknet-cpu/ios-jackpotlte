#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn the Wi-Fi/BT firmware's log lines in a console capture into text.

Kernel 0210 prints the firmware's own log (the mxlog stream) as
    A5XELTE MXLOG4 L<level>: <text>                      (phase 4: plain text)
    A5XELTE MXLOG5 L<level> <ts> <offset> <arg> ...      (phase 5, all hex)
A phase-5 record names its format string by its offset in the firmware's
debug/common/log-strings.bin (on the phone's ROM under
/vendor/etc/wifi/mx140/debug/common/; a Samsung file, never put it in git).
Arguments that the format prints with %s are offsets in the same file.

usage: mxlog5.py serial.log log-strings.bin [--all]
       (--all: every record in the file; default: from the last boot that
        printed "stage 1/6 firmware" on)
"""
import re
import sys

LEVELS = ['ERR', 'WARN', 'MAJOR', 'MINOR', 'DETAIL']
CONV = re.compile(r'%([-+ #0]*)(\d+|\*)?(?:\.(\d+))?(hh|h|ll|l|z|j|t|L)?([diouxXcspn%])')


def cstring(table, off):
    if off >= len(table):
        return '<offset 0x%x out of range>' % off
    end = table.find(b'\0', off)
    return table[off:end if end >= 0 else len(table)].decode('latin-1')


def render(fmt, args, table):
    out, ai = [], 0
    pos = 0
    for m in CONV.finditer(fmt):
        out.append(fmt[pos:m.start()])
        pos = m.end()
        flags, width, prec, _length, conv = m.groups()
        if conv == '%':
            out.append('%')
            continue
        if width == '*':
            width = str(args[ai] if ai < len(args) else 0)
            ai += 1
        v = args[ai] if ai < len(args) else 0
        ai += 1
        spec = '%' + (flags or '') + (width or '') + ('.' + prec if prec else '')
        if conv == 's':
            out.append((spec + 's') % cstring(table, v))
        elif conv in 'di':
            out.append((spec + 'd') % (v - (1 << 32) if v & 0x80000000 else v))
        elif conv == 'p':
            out.append('0x%08x' % v)
        elif conv == 'c':
            out.append(chr(v & 0xff))
        elif conv == 'n':
            pass
        else:
            out.append((spec + conv) % v)
    out.append(fmt[pos:])
    return ''.join(out).rstrip('\n')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    text = open(sys.argv[1], 'rb').read().decode('latin-1').replace('\r', '')
    table = open(sys.argv[2], 'rb').read()
    if '--all' not in sys.argv:
        i = text.rfind('stage 1/6 firmware')
        if i >= 0:
            text = text[text.rfind('\n', 0, i) + 1:]
    for line in text.split('\n'):
        m = re.search(r'A5XELTE MXLOG5 L(\d+)((?: [0-9a-f]+)+)', line)
        if m:
            words = [int(w, 16) for w in m.group(2).split()]
            level = int(m.group(1))
            if len(words) < 2:
                print('MXLOG5 short record: %s' % line.strip())
                continue
            ts, off, args = words[0], words[1], words[2:]
            name = LEVELS[level] if level < len(LEVELS) else str(level)
            print('%08x %-6s %s' % (ts, name, render(cstring(table, off), args, table)))
            continue
        m = re.search(r'A5XELTE MXLOG4 L(\d+): (.*)', line)
        if m:
            level = int(m.group(1))
            name = LEVELS[level] if level < len(LEVELS) else str(level)
            print('-------- %-6s %s' % (name, m.group(2)))
            continue
        if 'A5XELTE SCSC:' in line or 'A5XELTE BT:' in line:
            print('# ' + line.strip())
    return 0


if __name__ == '__main__':
    sys.exit(main())
