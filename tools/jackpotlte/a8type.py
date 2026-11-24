#!/usr/bin/env python3
"""a8type.py: type on the phone's keyboard with the console's tap command and
measure how far the screen falls behind (a8type.py TEXT [GAP_S] [ROUNDS])."""
import os, sys, time, re, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, 'serial.log'); INBOX = os.path.join(HERE, 'to-phone.txt')
# iOS 16 keyboard on 1080x2220 (Spotlight search), panel pixels
ROW1 = dict(zip('qwertyuiop', [108, 216, 324, 432, 540, 648, 760, 868, 976, 1062]))
ROW2 = dict(zip('asdfghjkl', [168, 276, 384, 488, 600, 708, 812, 920, 1028]))
ROW3 = dict(zip('zxcvbnm', [276, 384, 488, 600, 708, 812, 920]))
def key(c):
    if c == ' ': return (600, 1920)
    for row, y in ((ROW1, 1432), (ROW2, 1600), (ROW3, 1752)):
        if c in row: return (row[c], y)
    raise SystemExit('no key for %r' % c)
def send(line):
    with open(INBOX, 'ab') as f: f.write(line.encode() + b'\r\n')
text = sys.argv[1]; gap = float(sys.argv[2]) if len(sys.argv) > 2 else 0.33
rounds = int(sys.argv[3]) if len(sys.argv) > 3 else 1
t0 = time.time()
for _ in range(rounds):
    for c in text:
        x, y = key(c); send('tap %d %d' % (x, y)); time.sleep(gap)
print('sent %d taps in %.1f s' % (len(text) * rounds, time.time() - t0))
