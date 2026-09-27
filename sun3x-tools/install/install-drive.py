#!/usr/bin/env python3
"""Adapted from /tmp/qemu-m8/sun3x-tools/boot-sunos.py (copied, not edited in
place) for the manual SunOS 4.1.1 sun3x disk install: boots the miniroot the
same way (Esc past the self-test, boot sd(0,30,3), wait for 'dump on', nurse
a '# ' shell prompt), then sends a caller-supplied list of commands, each
with a *long* per-command timeout (default 1800s, override SUN3_CMD_TIMEOUT)
since newfs/tar/zcat on this emulated SCSI stack run at ~80-100 KB/s.

Differences from boot-sunos.py:
  * CMDS is read from a Python file (--cmds-file) as a list of strings, so
    each install stage can be edited without touching this driver.
  * A string containing '\\n' is sent as a multi-line block (each line
    '\\r'-terminated in one write()), for heredocs (e.g. writing /a/etc/fstab).
  * Each command's wall-clock start/end time is logged (to stdout AND the
    SUN3_LOG file) so step timings land in the transcript.
  * Per-command wait timeout is SUN3_CMD_TIMEOUT (default 1800s), not 30s.
  * Prompt detection: waits for '\\n# ' (prompt at start of a fresh line) to
    avoid matching '# ' inside command echo/output text as it streams in.
"""
import os, select, subprocess, sys, time, argparse, importlib.util

p = argparse.ArgumentParser()
p.add_argument('--cmds-file', required=True, help='python file defining CMDS = [...]')
p.add_argument('qemu_argv', nargs=argparse.REMAINDER)
args = p.parse_args()

spec = importlib.util.spec_from_file_location('cmds_mod', args.cmds_file)
cmds_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cmds_mod)
CMDS = cmds_mod.CMDS

LOG = open(os.environ.get('SUN3_LOG', '/tmp/sunos-install/install-drive.log'), 'wb')
CAPTURE = float(os.environ.get('SUN3_CAPTURE', '300'))
CMD_TIMEOUT = float(os.environ.get('SUN3_CMD_TIMEOUT', '1800'))
MAXBUF = 1 << 20
MAXLOG = int(os.environ.get('SUN3_MAXLOG', str(40 << 20)))  # 40 MiB default (install has more output than smoketests)
_logbytes = 0

qemu_argv = args.qemu_argv
if qemu_argv and qemu_argv[0] == '--':
    qemu_argv = qemu_argv[1:]

q = subprocess.Popen(qemu_argv + ['-serial', 'stdio', '-monitor', 'none'],
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                      stderr=subprocess.STDOUT)
open(os.environ.get('SUN3_PIDFILE', '/tmp/sunos-install/install-drive.pid'), 'w').write(str(q.pid))
buf = b''


def logline(s):
    msg = ('\n*** [%s] %s' % (time.strftime('%H:%M:%S'), s)).encode()
    print(msg.decode(), flush=True)
    LOG.write(msg)
    LOG.flush()


def pump(t, upto=None):
    global buf, _logbytes
    end = time.time() + t
    last_flush = time.time()
    while time.time() < end:
        if upto and upto in buf:
            return True
        r, _, _ = select.select([q.stdout], [], [], 0.5)
        if r:
            d = os.read(q.stdout.fileno(), 65536)
            if not d:
                break
            buf += d
            if len(buf) > MAXBUF:
                buf = buf[-(MAXBUF // 2):]
            LOG.write(d)
            _logbytes += len(d)
            now = time.time()
            if now - last_flush > 1.0:
                LOG.flush()
                last_flush = now
            if _logbytes > MAXLOG:
                LOG.flush()
                print('\n*** log exceeded %d bytes (probable text storm) -- '
                      'aborting to protect shared /tmp' % MAXLOG, flush=True)
                q.kill()
                sys.exit(3)
    LOG.flush()
    return upto is not None and upto in buf


def send(b):
    q.stdin.write(b)
    q.stdin.flush()


if not pump(60, b'Press <Esc>'):
    logline('no selftest banner'); q.kill(); sys.exit(1)
time.sleep(0.3); send(b'\x1b'); logline('sent Esc')
buf = b''
if pump(60, b'\n>'):
    logline('monitor prompt; booting b sd(0,30,3)')
    time.sleep(0.5); buf = b''; send(b'b sd(0,30,3)\r')
else:
    logline('no literal monitor prompt seen in 60s; assuming autoboot in progress')

if not pump(CAPTURE, b'dump on'):
    logline('never saw "dump on" within %ss -- giving up' % CAPTURE)
    q.kill(); sys.exit(1)
logline('saw "dump on"; waiting up to 240s for a shell prompt')

buf = b''
got_prompt = False
deadline = time.time() + 240
last_nudge = time.time()
while time.time() < deadline:
    if pump(2, b'\n# '):
        got_prompt = True
        break
    if time.time() - last_nudge > 20:
        send(b'\r')
        last_nudge = time.time()
        logline('nudged with a bare CR')

if not got_prompt:
    logline('shell prompt never appeared within 240s')
    logline('qemu pid=%d (left running for inspection)' % q.pid)
    sys.exit(2)

logline('SHELL PROMPT REACHED -- starting %d commands, per-cmd timeout=%ss' % (len(CMDS), CMD_TIMEOUT))

all_ok = True
for i, c in enumerate(CMDS):
    buf = b''
    t0 = time.time()
    logline('CMD[%d/%d] START: %r' % (i + 1, len(CMDS), c))
    if '\n' in c:
        for line in c.split('\n'):
            send((line + '\r').encode('latin1'))
            time.sleep(0.05)
    else:
        send((c + '\r').encode('latin1'))
    ok = pump(CMD_TIMEOUT, b'\n# ')
    dt = time.time() - t0
    logline('CMD[%d/%d] END prompt_seen=%s elapsed=%.1fs' % (i + 1, len(CMDS), ok, dt))
    if not ok:
        all_ok = False
        logline('*** STOPPING: prompt not seen for command %r within %ss' % (c, CMD_TIMEOUT))
        break

logline('all commands processed, ok=%s; leaving qemu running briefly then killing' % all_ok)
pump(5)
q.kill()
sys.exit(0 if all_ok else 4)
