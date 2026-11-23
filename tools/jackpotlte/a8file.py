#!/usr/bin/env python3
"""a8file.py: files on the phone through the USB console (kernel 0190+).

Run it next to serial.log and to-phone.txt, with the a8-serial.ps1 bridge open.

  a8file.py get PHONEPATH LOCAL      fget, decoded and CRC-checked
  a8file.py put LOCAL PHONEPATH [MODE UID GID]
  a8file.py cmd 'LINE' [SECONDS]     send a shell line, print what comes back
"""
import base64, os, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PHONE = os.environ.get('A8PHONE') or (HERE if os.path.exists(os.path.join(HERE, 'serial.log'))
                                      else os.path.expanduser('~/mnt/platform-tools/phone'))
LOG = os.path.join(PHONE, 'serial.log')
INBOX = os.path.join(PHONE, 'to-phone.txt')


def send(lines):
    with open(INBOX, 'ab') as f:
        for line in lines:
            f.write(line.encode() + b'\r\n')


def log_size():
    return os.path.getsize(LOG)


def read_from(pos):
    with open(LOG, 'rb') as f:
        f.seek(pos)
        return f.read().decode('latin-1')


def wait_for(pos, done, timeout):
    end = time.time() + timeout
    text = ''
    while time.time() < end:
        text = read_from(pos)
        if done(text):
            return text
        time.sleep(0.5)
    return text


def clean(text):
    out = []
    for raw in text.replace('\r', '\n').split('\n'):
        out.append(raw.strip())
    return out


def get(path, local, timeout=120):
    pos = log_size()
    send(['fget ' + path])
    text = wait_for(pos, lambda t: ('A8F END ' + path) in t or 'A8F GET error' in t, timeout)
    lines = clean(text)
    head = [l for l in lines if l.startswith('A8F GET ')]
    if not head or 'error' in head[0]:
        print('\n'.join(l for l in lines if 'A8F' in l or 'A8SH' in l)[-2000:])
        return False
    h = head[-1].split()
    crc = int(h[h.index('crc') + 1], 16)
    length = int(h[h.index('len') + 1])
    start = lines.index(head[-1])
    data = b''
    for l in lines[start + 1:]:
        if l.startswith('A8F END'):
            break
        if l.startswith('A8F: '):
            data += base64.b64decode(l[5:])
    ok = len(data) == length and (zlib.crc32(data) & 0xffffffff) == crc
    print('get %s: %d bytes, crc %08x %s' % (path, len(data), zlib.crc32(data) & 0xffffffff,
          'OK' if ok else 'MISMATCH (want %d %08x)' % (length, crc)))
    if ok:
        open(local, 'wb').write(data)
    return ok


def put(local, path, owner=None, timeout=120):
    data = open(local, 'rb').read()
    crc = zlib.crc32(data) & 0xffffffff
    pos = log_size()
    first = 'fput %s %d %08x' % (path, len(data), crc)
    if owner:
        first += ' ' + ' '.join(owner)
    send([first])
    text = wait_for(pos, lambda t: 'A8F PUT ready' in t or 'A8F PUT error' in t, 30)
    if 'A8F PUT ready' not in text:
        print('\n'.join(l for l in clean(text) if 'A8F' in l)[-2000:])
        return False
    b64 = base64.b64encode(data).decode()
    chunks = ['fd ' + b64[i:i + 160] for i in range(0, len(b64), 160)]
    for i in range(0, len(chunks), 16):
        send(chunks[i:i + 16])
        time.sleep(0.6)
    send(['fend'])
    text = wait_for(pos, lambda t: 'A8F PUT ok' in t or 'A8F PUT error' in t.split('A8F PUT ready', 1)[-1], timeout)
    res = [l for l in clean(text) if l.startswith('A8F PUT') or 'dropped' in l]
    print('\n'.join(res[-4:]))
    return any(l.startswith('A8F PUT ok') for l in res)


def cmd(line, seconds=4):
    pos = log_size()
    send([line])
    time.sleep(seconds)
    print(read_from(pos)[-6000:])


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'get':
        sys.exit(0 if get(a[1], a[2]) else 1)
    if a and a[0] == 'put':
        sys.exit(0 if put(a[1], a[2], a[3:6] if len(a) >= 6 else None) else 1)
    if a and a[0] == 'cmd':
        cmd(a[1], float(a[2]) if len(a) > 2 else 4)
        sys.exit(0)
    print(__doc__)
