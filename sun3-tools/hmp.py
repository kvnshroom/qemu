#!/usr/bin/env python3
"""hmp.py SOCK cmd [cmd ...] -- send HMP commands over the monitor unix socket, print replies.
Special forms: 'type:<text>' types text via sendkey (chars -> keynames), 'sleep:<s>'."""
import socket, sys, time
sock = sys.argv[1]
KEYS = {' ': 'spc', '-': 'minus', '=': 'equal', '/': 'slash', '.': 'dot', ',': 'comma',
        ';': 'semicolon', "'": 'apostrophe', '[': 'bracket_left', ']': 'bracket_right',
        '\\': 'backslash', '`': 'grave_accent', '\n': 'ret', '\t': 'tab'}
SHIFT = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
         '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right',
         '|': 'backslash', ':': 'semicolon', '"': 'apostrophe', '<': 'comma', '>': 'dot',
         '?': 'slash', '~': 'grave_accent'}
def keyname(c):
    if c in KEYS: return KEYS[c]
    if c in SHIFT: return 'shift-' + SHIFT[c]
    if c.isupper(): return 'shift-' + c.lower()
    return c
s = socket.socket(socket.AF_UNIX); s.connect(sock); s.settimeout(0.3)
def rd():
    out = b''
    while True:
        try: d = s.recv(65536)
        except socket.timeout: break
        if not d: break
        out += d
    return out.decode('latin1')
rd()
def cmd(c, quiet=False):
    s.sendall(c.encode() + b'\n'); time.sleep(0.15)
    r = rd()
    if not quiet:
        r = '\n'.join(l for l in r.splitlines() if l.strip() and not l.startswith('(qemu)') and c not in l)
        if r: print(r)
for a in sys.argv[2:]:
    if a.startswith('type:'):
        for ch in a[5:]:
            cmd('sendkey ' + keyname(ch), quiet=True); time.sleep(0.12)
    elif a.startswith('sleep:'):
        time.sleep(float(a[6:]))
    else:
        cmd(a)
