#!/usr/bin/env python3
"""a8slot.py: the two kernel slots in BOOT through the USB console (0194+).

Run it next to serial.log and to-phone.txt, with the a8-serial.ps1 bridge open
(same as a8file.py).

  a8slot.py status                 the phone's "slots" report
  a8slot.py put FILE.slot          bput a slot image (make-slots.sh slot ...)
                                   into the slot that is not the default
  a8slot.py try [SECONDS]          date + tryb, wait for the phone to come back,
                                   then watch the new boot for SECONDS (default
                                   300) and score it into scores.jsonl
  a8slot.py score                  score the boot that is running now
  a8slot.py tryok | promote        judge the try (only on the try boot)

The phone enforces the hard limits itself (never the default slot, 4 tries an
hour, stop after 2 failed tries in a row, promote only from the try boot after
120 s); this tool stops at the first error and never retries a write.
"""
import base64, hashlib, json, os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PHONE = os.environ.get('A8PHONE') or (HERE if os.path.exists(os.path.join(HERE, 'serial.log'))
                                      else os.path.expanduser('~/mnt/platform-tools/phone'))
LOG = os.path.join(PHONE, 'serial.log')
INBOX = os.path.join(PHONE, 'to-phone.txt')
SCORES = os.path.join(PHONE, 'scores.jsonl')
WINDOW = 1 << 20        # raw bytes sent ahead of the phone's last "got" line


def send(lines):
    with open(INBOX, 'ab') as f:
        for line in lines:
            f.write(line.encode() + b'\r\n')


def log_size():
    return os.path.getsize(LOG)


def read_from(pos):
    with open(LOG, 'rb') as f:
        f.seek(pos)
        return f.read().decode('latin-1').replace('\r', '')


def wait_for(pos, done, timeout, step=0.2):
    end = time.time() + timeout
    text = ''
    while time.time() < end:
        text = read_from(pos)
        if done(text):
            return text
        time.sleep(step)
    return text


def lines_with(text, *keys):
    return [l for l in text.split('\n') if any(k in l for k in keys)]


def status():
    pos = log_size()
    send(['slots'])
    text = wait_for(pos, lambda t: len(lines_with(t, 'A8 SLOTS: slot 1', 'A8 SLOTS: BOOT',
                                                  'unknown command')) > 0, 15)
    print('\n'.join(lines_with(text, 'A8 SLOTS', 'A8SH: unknown')))


def put(path):
    data = open(path, 'rb').read()
    sha = hashlib.sha256(data).hexdigest()
    pos = log_size()
    send(['bput %d %s' % (len(data), sha)])
    text = wait_for(pos, lambda t: 'A8B PUT ready' in t or 'A8B PUT error' in t or
                    'unknown command' in t, 30)
    if 'A8B PUT ready' not in text:
        print('\n'.join(lines_with(text, 'A8B', 'A8SH'))[-2000:] or 'no answer to bput')
        return False
    b64 = base64.b64encode(data).decode()
    step = 160                          # base64 chars a line = 120 bytes
    lines = ['bd ' + b64[i:i + step] for i in range(0, len(b64), step)]
    sent = acked = 0
    t0 = time.time()
    i = 0
    while i < len(lines):
        batch = lines[i:i + 256]
        send(batch)
        i += len(batch)
        sent = min(len(data), i * step * 3 // 4)
        while sent - acked > WINDOW:
            text = read_from(pos)
            got = re.findall(r'A8B PUT got (\d+) of', text)
            if 'A8B PUT error' in text or 'lines were dropped' in text:
                print('\n'.join(lines_with(text, 'A8B PUT error', 'dropped'))[-1000:])
                return False
            if got and int(got[-1]) > acked:
                acked = int(got[-1])
                continue
            if time.time() - t0 > 30 + len(data) / 20000:
                print('the phone stopped answering at %d of %d bytes' % (acked, len(data)))
                return False
            time.sleep(0.1)
        print('\r%d of %d bytes sent, %d confirmed, %.0f s' % (sent, len(data), acked,
              time.time() - t0), end='', flush=True)
    send(['bend'])
    text = wait_for(pos, lambda t: 'A8B PUT ok' in t or 'A8B PUT error' in t, 300)
    print()
    res = lines_with(text, 'A8B PUT', 'dropped')
    print('\n'.join(l for l in res if 'PUT got' not in l)[-3000:])
    # The phone prints the sha256 of the compressed payload (after the record).
    want = hashlib.sha256(data[128:]).hexdigest()
    ok = any(l.startswith('A8B PUT ok') and want in l for l in res)
    print('put %s: %s in %.0f s' % (path, 'OK' if ok else 'FAILED', time.time() - t0))
    return ok


def last_boot_text():
    """serial.log text since the last 'connected' header."""
    with open(LOG, 'rb') as f:
        size = f.seek(0, 2)
        back = min(size, 8 << 20)
        f.seek(size - back)
        text = f.read().decode('latin-1').replace('\r', '')
    j = text.rfind(' connected on ')
    if j < 0:
        return text
    return text[text.rfind('\n', 0, j) + 1:]


def score(text, want_slot=None):
    s = {'time': time.strftime('%Y-%m-%d %H:%M:%S')}
    m = re.findall(r'A8 SLOTS: booted slot (\d+) \(([^)]*)\)', text)
    s['slot'] = int(m[-1][0]) if m else None
    s['why'] = m[-1][1] if m else None
    ups = [int(x) for x in re.findall(r'A8 STAT: up (\d+) s', text)]
    s['up'] = max(ups) if ups else 0
    procs = [int(x) for x in re.findall(r'procs (\d+);', text)]
    s['procs'] = procs[-1] if procs else 0
    s['flips'] = len(re.findall(r'^A8 FLIP:', text, re.M))
    # A panic of this boot shows as panic(cpu ...) if the console got it out;
    # "A8 PANIC:" lines are the previous boot's panic, reported by this one.
    s['panic'] = lines_with(text, 'SWEET EARLY PANIC', 'A5 XNU PANIC', 'panic(cpu')[:3]
    s['prev_panic'] = lines_with(text, 'A8 PANIC:')[:3]
    s['failed_try'] = lines_with(text, 'FAILED:')[:2]
    s['reset'] = (re.findall(r'reset cause ([0-9a-f]{8})', text) or [None])[-1]
    s['wdt'] = lines_with(text, 'A8 WDT:')[:1]
    checks = {
        'booted the wanted slot': want_slot is None or s['slot'] == want_slot,
        'up 120 s or more': s['up'] >= 120,
        '150+ processes (iOS up)': s['procs'] >= 150,
        'screen flipped': s['flips'] >= 1,
        'no panic text': not s['panic'],
    }
    s['checks'] = checks
    s['pass'] = all(checks.values())
    return s


def record(s):
    with open(SCORES, 'a') as f:
        f.write(json.dumps(s) + '\n')
    for k, v in s['checks'].items():
        print('  %-26s %s' % (k, 'yes' if v else 'NO'))
    print('score: %s (slot %s, %s, up %s s, %s procs, %s flips)%s' % (
        'PASS' if s['pass'] else 'FAIL', s['slot'], s['why'], s['up'], s['procs'], s['flips'],
        ('; panic: ' + s['panic'][0]) if s['panic'] else ''))


def try_slot(seconds):
    pos = log_size()
    send(['date %d' % int(time.time()), 'slots'])
    text = wait_for(pos, lambda t: 'A8 SLOTS: slot 1' in t, 15)
    rec = re.findall(r'A8 SLOTS: slot (\d): .*\(default\)', text)
    if not rec:
        print('\n'.join(lines_with(text, 'A8 SLOTS', 'A8SH'))[-1500:] or 'no slots report')
        return False
    target = 1 - int(rec[-1])
    pos = log_size()
    send(['tryb'])
    text = wait_for(pos, lambda t: 'A8B TRY ' in t and ('rebooting' in t or 'refused' in t or
                                                       'error' in t), 20)
    print('\n'.join(lines_with(text, 'A8B TRY')))
    if 'rebooting' not in text:
        return False
    t0 = time.time()
    text = wait_for(pos, lambda t: ' connected on ' in t, 300, 1)
    if ' connected on ' not in text:
        print('the phone did not come back within 300 s; look at the screen')
        return False
    print('back after %.0f s' % (time.time() - t0))
    time.sleep(3)
    send(['date %d' % int(time.time())])
    end = time.time() + seconds
    ended = False
    while time.time() < end:
        text = last_boot_text()
        if 'disconnected' in text.split(' connected on ', 1)[-1]:
            ended = True
            break
        time.sleep(5)
    s = score(text if ended else last_boot_text(), target)
    s['target'] = target
    if ended:
        # The try boot went away: wait for the fallback boot and keep what
        # it says about the try (FAILED line, the try's panic message).
        print('the try boot ended after %.0f s; waiting for the fallback' % (time.time() - t0))
        pos = log_size()
        back = wait_for(pos, lambda t: ' connected on ' in t, 420, 1)
        if ' connected on ' in back:
            time.sleep(20)
            send(['date %d' % int(time.time())])
            fb = score(last_boot_text())
            s['fallback'] = {k: fb[k] for k in ('slot', 'why', 'reset', 'failed_try', 'prev_panic')}
        else:
            s['fallback'] = 'did not come back within 420 s'
    record(s)
    if 'fallback' in s:
        print('fallback:', s['fallback'])
    return s['pass']


def judge(word):
    pos = log_size()
    send([word])
    text = wait_for(pos, lambda t: 'A8B TRYOK' in t or 'A8B PROMOTE' in t or 'unknown' in t, 20)
    print('\n'.join(lines_with(text, 'A8B', 'A8SH: unknown')))


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['status']:
        status()
    elif a[:1] == ['put'] and len(a) == 2:
        sys.exit(0 if put(a[1]) else 1)
    elif a[:1] == ['try']:
        sys.exit(0 if try_slot(int(a[1]) if len(a) > 1 else 300) else 1)
    elif a[:1] == ['score']:
        record(score(last_boot_text()))
    elif a[:1] in (['tryok'], ['promote']):
        judge(a[0])
    else:
        print(__doc__)
