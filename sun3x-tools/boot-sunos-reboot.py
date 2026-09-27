#!/usr/bin/env python3
"""boot-sunos-reboot.py -- drive an INSTALLED SunOS 4.1.1/sun3x disk to a
multi-user `login:`, log in as root, issue `sync; reboot`, and verify the
machine comes back to `login:` and a root shell again, unattended.

Why a dedicated tool: sun3x-tools/boot-sunos.py drives the MINIROOT to its
single-user `#` shell, which is a different (and much shorter) sequence.
And the one trap this test has, which cost a session's time before it was
understood: a WARM reboot legitimately never prints the PROM self-test
banner.  SunOS's `reboot(8)` re-enters the monitor, which immediately boots
its own default device -- so a driver that waits for "Press <Esc>" (or for a
`>` prompt) after the reboot fails the test even when the reboot worked
perfectly.  Only the FIRST boot gets the Esc/self-test handling here.

The login prompt itself is expected to arrive partially garbled (an
authentic SunOS getty artifact on this console -- see the session-23
section of hw/m68k/sun3x-NOTES.md (full journal: branch sun3-history), reproduced by real hardware), so
the login is driven blind: after rc's own completion markers and a quiet
console, send `root` and look for a shell prompt.

Usage:
    SUN3_LOG=/tmp/x.log python3 boot-sunos-reboot.py -- \\
        qemu-system-m68k -M sun3x -m 16M -bios <prom> \\
        -drive file=<copy-of-installed.img>,format=raw,if=scsi,bus=0,unit=0 \\
        -nic user,model=lance -display none

Environment: SUN3_LOG (transcript, default /tmp/sunos-reboot.log),
SUN3_PIDFILE, SUN3_MAXLOG (console-flood guard, default 60 MiB),
SUN3_BOOT_TIMEOUT (per-boot budget, default 1200 s).

Exit status: 0 = both boots reached a root shell and the reboot passed.
"""
import os
import select
import subprocess
import sys
import time

LOG = open(os.environ.get('SUN3_LOG', '/tmp/sunos-reboot.log'), 'wb')
MAXLOG = int(os.environ.get('SUN3_MAXLOG', str(60 << 20)))
BOOT_TIMEOUT = float(os.environ.get('SUN3_BOOT_TIMEOUT', '1200'))
_logbytes = 0

argv = sys.argv[1:]
if argv and argv[0] == '--':
    argv = argv[1:]
if not argv:
    sys.exit(__doc__)

q = subprocess.Popen(argv + ['-serial', 'stdio', '-monitor', 'none'],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.STDOUT)
open(os.environ.get('SUN3_PIDFILE', '/tmp/sunos-reboot.pid'),
     'w').write(str(q.pid))
buf = b''


def logline(s):
    msg = ('\n*** [%s] %s' % (time.strftime('%H:%M:%S'), s)).encode()
    print(msg.decode(), flush=True)
    LOG.write(msg)
    LOG.flush()


def pump(t, upto=None, upto_any=None, quiet_after=None, quiet_secs=15):
    """Read the console for up to t seconds.  Returns the matched needle (or
    True) for upto/upto_any, b'QUIET' once quiet_after has been seen and the
    console has then been silent for quiet_secs, else None/False."""
    global buf, _logbytes
    end = time.time() + t
    last_data = time.time()
    seen_marker = False
    while time.time() < end:
        if upto and upto in buf:
            return True
        for u in (upto_any or ()):
            if u in buf:
                return u
        if quiet_after and not seen_marker and quiet_after in buf:
            seen_marker = True
            last_data = time.time()
        if seen_marker and (time.time() - last_data) > quiet_secs:
            return b'QUIET'
        r, _, _ = select.select([q.stdout], [], [], 0.5)
        if r:
            d = os.read(q.stdout.fileno(), 65536)
            if not d:
                break
            buf += d
            last_data = time.time()
            if len(buf) > (1 << 20):
                buf = buf[-(1 << 19):]
            LOG.write(d)
            LOG.flush()
            _logbytes += len(d)
            if _logbytes > MAXLOG:
                logline('console flood: over %d bytes -- aborting' % MAXLOG)
                q.kill()
                sys.exit(3)
    if upto_any or quiet_after:
        return None
    return upto is not None and upto in buf


def send(b):
    q.stdin.write(b)
    q.stdin.flush()


def boot_and_login(phase, expect_banner):
    global buf
    if expect_banner:
        if not pump(90, b'Press <Esc>'):
            logline('%s: no self-test banner' % phase)
            return None
        time.sleep(0.3)
        send(b'\x1b')
        logline('%s: sent Esc' % phase)
        buf = b''
        if pump(60, b'\n>'):
            logline('%s: monitor prompt; booting sd(0,0,0)' % phase)
            time.sleep(0.5)
            buf = b''
            send(b'b sd(0,0,0)\r')
        else:
            logline('%s: no > prompt; assuming autoboot' % phase)
    result = pump(BOOT_TIMEOUT, upto_any=[b'panic'],
                  quiet_after=b'starting local daemons', quiet_secs=45)
    if result == b'panic':
        logline('%s: KERNEL PANIC' % phase)
        return None
    if result is None:
        logline('%s: never reached post-rc quiet within %ss'
                % (phase, BOOT_TIMEOUT))
        return None
    logline('%s: console quiet after rc markers -- logging in' % phase)
    for attempt in range(6):
        time.sleep(3)
        buf = b''
        send(b'\r')
        time.sleep(1)
        buf = b''
        send(b'root\r')
        result = pump(20, upto_any=[b'assword:', b'\n# ', b'\n$ '])
        if result == b'assword:':
            time.sleep(0.5)
            buf = b''
            send(b'\r')
            result = pump(20, upto_any=[b'\n# ', b'\n$ '])
        logline('%s: login attempt %d -> %r' % (phase, attempt + 1, result))
        if result in (b'\n# ', b'\n$ '):
            logline('%s: LOGGED IN' % phase)
            return result
    logline('%s: never reached a shell' % phase)
    return None


def run_cmd(cmd, prompt, timeout=60):
    global buf
    buf = b''
    send((cmd + '\r').encode('latin1'))
    ok = pump(timeout, prompt)
    logline('cmd=%r prompt_seen=%s' % (cmd, ok))
    return ok


prompt = boot_and_login('FIRST BOOT', expect_banner=True)
if not prompt:
    logline('first boot failed -- qemu pid=%d left running' % q.pid)
    sys.exit(2)
run_cmd('uname -a', prompt)
logline('issuing sync; reboot')
run_cmd('sync', prompt)
buf = b''
send(b'reboot\r')
# A warm reboot re-enters the monitor, which boots its default device with
# no self-test banner and no '>' prompt: expect_banner=False.
prompt2 = boot_and_login('AFTER REBOOT', expect_banner=False)
if not prompt2:
    logline('reboot did not come back -- qemu pid=%d left running' % q.pid)
    sys.exit(2)
run_cmd('uname -a', prompt2)
run_cmd('who', prompt2)
run_cmd('sync', prompt2)
logline('REBOOT TEST PASSED')
pump(3)
q.kill()
