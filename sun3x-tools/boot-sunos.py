#!/usr/bin/env python3
"""Drive the sun3x SunOS boot all the way to the MINIROOT single-user shell:
abort the PROM self-test with Esc, wait for the '>' monitor prompt (or, if
this PROM wins the Esc-vs-autoboot race and never prints one, just keep
watching -- it boots its own default device, which is the same disk), boot
from the disk, wait for 'dump on' (root/swap/dump configured), then wait up
to ~240s for a '# ' shell prompt, sending a bare \\r every ~20s if nothing
shows. Once the prompt appears, send a fixed list of commands one at a time,
waiting for the prompt to reappear after each. Keeps the full transcript,
bounded in two ways: an in-memory match buffer capped at 1 MiB, and a hard
SUN3_MAXLOG (default 20 MiB) abort if the session floods the console with
real text (e.g. a "stray level N interrupt" storm) -- /tmp is a shared
tmpfs and must not be filled."""
import os, select, subprocess, sys, time

LOG = open(os.environ.get('SUN3_LOG', '/tmp/sunos-drive.log'), 'wb')
CAPTURE = float(os.environ.get('SUN3_CAPTURE', '300'))
MAXBUF = 1 << 20  # never let the in-memory match buffer exceed 1 MiB
# Safety valve: some boot attempts hit a "stray level 7" (or similar) storm
# that floods the console with real text at high rate, unlike the silent
# zs storm. /tmp is a shared tmpfs -- bail out hard rather than fill it.
MAXLOG = int(os.environ.get('SUN3_MAXLOG', str(20 << 20)))  # 20 MiB
_logbytes = 0

q = subprocess.Popen(sys.argv[1:] + ['-serial', 'stdio', '-monitor', 'none'],
                      stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                      stderr=subprocess.STDOUT)
open(os.environ.get('SUN3_PIDFILE', '/tmp/sunos-drive.pid'), 'w').write(str(q.pid))
buf = b''


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
                print('\n*** log exceeded %d bytes (probable text storm, '
                      'e.g. "stray level N interrupt") -- aborting to '
                      'protect shared /tmp' % MAXLOG, flush=True)
                q.kill()
                sys.exit(3)
    LOG.flush()
    return upto is not None and upto in buf


def send(b):
    q.stdin.write(b)
    q.stdin.flush()


NO_ESC = bool(os.environ.get('SUN3_NO_ESC'))

if not pump(60, b'Press <Esc>'):
    print('\n*** no selftest banner'); q.kill(); sys.exit(1)
if NO_ESC:
    # Let the full, unattended self-test run to completion instead of
    # aborting it -- exercises the TOD-clock, forced-parity and phase-2
    # device subtests (see hw/m68k/sun3x-NOTES.md (full journal: branch sun3-history)). As of session 34
    # the TOD and both parity subtests pass and the run gets as far as the
    # phase-2 "ESP SCSI Controller Check", which this machine's NCR5380
    # stand-in cannot satisfy; because the ROM retries any failing subtest
    # forever (the `bclr #7,%d7` contract, see sun3x-NOTES.md), the run then
    # wedges there rather than reaching `>`, so the Esc abort is still the
    # default. SUN3_MAXLOG bounds the console text either way, but a wedge
    # produces very little text, so also honour SUN3_NOESC_TIMEOUT
    # (default 90s) as a belt-and-braces wall-clock abort.
    print('\n*** SUN3_NO_ESC set: letting the self-test run unattended')
    noesc_timeout = float(os.environ.get('SUN3_NOESC_TIMEOUT', '90'))
    if not pump(noesc_timeout, b'\n>'):
        print('\n*** no monitor prompt / autoboot seen within %ss of the '
              'unattended self-test' % noesc_timeout)
else:
    time.sleep(0.3); send(b'\x1b'); print('\n*** sent Esc')
buf = b''
if pump(60, b'\n>'):
    print('\n*** monitor prompt; booting b sd(0,30,3)')
    time.sleep(0.5); buf = b''; send(b'b sd(0,30,3)\r')
else:
    # This PROM sometimes wins the Esc-vs-autoboot race and boots its own
    # default device straight through without ever printing a literal '>'
    # (a known, harmless timing race, not a hang) -- don't give up, just
    # keep watching for the kernel to come up on its own.
    print('\n*** no literal monitor prompt seen in 60s; assuming autoboot '
          'in progress, continuing to watch for the kernel')

if not pump(CAPTURE, b'dump on'):
    print('\n*** never saw "dump on" within %ss -- giving up' % CAPTURE)
    q.kill(); sys.exit(1)
print('\n*** saw "dump on"; waiting up to 240s for a shell prompt')

buf = b''
got_prompt = False
deadline = time.time() + 240
last_nudge = time.time()
while time.time() < deadline:
    if pump(2, b'# '):
        got_prompt = True
        break
    if time.time() - last_nudge > 20:
        send(b'\r')
        last_nudge = time.time()
        print('\n*** nudged with a bare CR')

if not got_prompt:
    print('\n*** shell prompt never appeared within 240s')
    print('*** qemu pid=%d (left running for inspection)' % q.pid)
    sys.exit(2)

print('\n*** SHELL PROMPT REACHED')
# SUN3_CMDS, if set, overrides the default regression transcript with a
# ';'-separated list of commands (same convention as sun3-tools/boot-linux.py
# and serial-drive2.py) -- used by session-17 to drive sustained-I/O repros
# (newfs/dd) through this same, already-verified boot path without touching
# the default regression list below. SUN3_CMD_TIMEOUT overrides the 30s
# per-command wait (newfs/dd on this emulated SCSI stack can take far longer).
CMD_TIMEOUT = float(os.environ.get('SUN3_CMD_TIMEOUT', '30'))
_cmds_env = os.environ.get('SUN3_CMDS')
CMDS = _cmds_env.split(';') if _cmds_env else [
    'uname -a',
    'ls /',
    'df',
    'ps -ax',
    'date',
    'cat /etc/motd',
    'ls /dev | head',
    'dmesg | tail',
    'mount',
    'echo SUNOS_SHELL_OK',
]
for c in CMDS:
    buf = b''
    t0 = time.time()
    send((c + '\r').encode('latin1'))
    ok = pump(CMD_TIMEOUT, b'# ')
    print('\n*** cmd=%r prompt_seen=%s elapsed=%.1fs' % (c, ok, time.time() - t0), flush=True)

print('\n*** all commands sent; leaving qemu running (pid %d)' % q.pid)
pump(5)
q.kill()
