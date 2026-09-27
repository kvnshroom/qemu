#!/usr/bin/env python3
"""
install-drive.py -- drive the SunOS 4.1.1/sun3 MINIROOT shell on QEMU -M sun3-60
for the manual disk install (the 3/60 counterpart of
sun3x-tools/install/install-drive.py).

PROM handshake: the 3/60 v3.0.1 PROM auto-boots sd(0,0,0); with the blank
target disk at unit 0 that boot block is zeros and the PROM drops to its '>'
monitor ("Exception 0x2C at 0x00006000"), where we type `b sd(0,18,0)` (unit
number is hex: target 3 = 0x18) to boot the miniroot+swap disk at qemu
unit=3, which SunOS names sd6.  Then wait for "dump on", the miniroot's own
"# " prompt, and send the caller's commands one at a time.

Command completion: after every command a `echo __DO''NE_<n>` is sent and
the driver waits for "__DONE_<n>" (the echoed command text carries the
quotes, so it cannot match; the shell's next prompt precedes the marker
because both lines are typed at once), with a long per-command
timeout (SUN3_CMD_TIMEOUT, default 1800 s: newfs/tar/zcat on the emulated
si run at a few hundred KB/s).  Each command's wall-clock time is logged.

usage: install-drive.py --cmds-file CMDS.py [--boot 'b sd(0,18,0)'|auto] [--log LOG]
(--boot auto: the miniroot disk is at unit 0 and the PROM auto-boots it; there is no
'>' prompt to wait for -- once the target carries a boot block the PROM boots it
instead, so later stages put the miniroot at unit 0 and the target at unit 1 = sd2)
                        -- <qemu argv (no -serial/-monitor)>
The CMDS file defines CMDS = [str, ...]; a string containing '\\n' is sent as
one block (each line CR-terminated) for here-documents.
"""
import argparse, importlib.util, os, socket, subprocess, sys, tempfile, time, threading

p = argparse.ArgumentParser()
p.add_argument('--cmds-file', required=True)
p.add_argument('--boot', default='b sd(0,18,0)')
p.add_argument('--log', default=os.environ.get('SUN3_LOG', '/tmp/sun3-install-drive.log'))
p.add_argument('--prom-timeout', type=float, default=180)
p.add_argument('--boot-timeout', type=float, default=600)
p.add_argument('--keep', action='store_true', help='leave qemu running after the last command (until --keep-seconds)')
p.add_argument('--keep-seconds', type=float, default=0)
p.add_argument('qemu_argv', nargs=argparse.REMAINDER)
args = p.parse_args()
qemu_argv = args.qemu_argv
if qemu_argv and qemu_argv[0] == '--':
    qemu_argv = qemu_argv[1:]

spec = importlib.util.spec_from_file_location('cmds_mod', args.cmds_file)
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
CMDS = mod.CMDS
CMD_TIMEOUT = float(os.environ.get('SUN3_CMD_TIMEOUT', '1800'))
MAXLOG = int(os.environ.get('SUN3_MAXLOG', str(40 << 20)))

LOG = open(args.log, 'wb'); logbytes = 0
SOCK = tempfile.mktemp(prefix='sun3-ttya-', suffix='.sock')
MON = tempfile.mktemp(prefix='sun3-mon-', suffix='.sock')
q = subprocess.Popen(qemu_argv + ['-serial', f'unix:{SOCK},server=on,wait=off',
                                  '-monitor', f'unix:{MON},server=on,wait=off'])
time.sleep(1)
s = socket.socket(socket.AF_UNIX); s.connect(SOCK); s.settimeout(0.5)
buf = b''; lock = threading.Lock(); stop = False; t0 = time.time()

def note(msg):
    line = ('\n*** [%s +%5.0fs] %s\n' % (time.strftime('%H:%M:%S'), time.time() - t0, msg)).encode()
    sys.stdout.write(line.decode()); sys.stdout.flush(); LOG.write(line); LOG.flush()

def reader():
    global buf, logbytes
    while not stop:
        try: d = s.recv(4096)
        except socket.timeout: continue
        except OSError: break
        if d:
            with lock:
                buf += d
                if len(buf) > (1 << 20): buf = buf[-(1 << 19):]
            if logbytes < MAXLOG:
                LOG.write(d); LOG.flush(); logbytes += len(d)
            sys.stdout.write(d.decode('latin1')); sys.stdout.flush()
threading.Thread(target=reader, daemon=True).start()

def wait(pat, t):
    global buf
    end = time.time() + t
    while time.time() < end:
        with lock:
            i = buf.find(pat)
            if i >= 0:
                buf = buf[i + len(pat):]; return True
        time.sleep(0.2)
    return False

def send(b): s.sendall(b)

def finish(rc):
    global stop
    stop = True; time.sleep(0.5)
    try: q.kill()
    except Exception: pass
    sys.exit(rc)

if args.boot == 'auto':
    note('letting the PROM auto-boot sd(0,0,0)')
else:
    if not wait(b'\n>', args.prom_timeout):
        note('no PROM monitor prompt'); finish(1)
    note('PROM monitor prompt; ' + args.boot)
    time.sleep(0.3); send((args.boot + '\r').encode())
if not wait(b'dump on', args.boot_timeout):
    note('kernel never reached "dump on"'); finish(1)
if not wait(b'# ', 120):
    note('no miniroot shell prompt'); finish(1)
note('miniroot shell prompt')
time.sleep(1)

failed = 0
for n, cmd in enumerate(CMDS):
    marker = ('__DONE_%d' % n).encode()
    lines = cmd.split('\n')
    block = b''.join(l.encode() + b'\r' for l in lines)
    block += b"echo __DO''NE_%d\r" % n
    with lock: buf = b''
    tc = time.time()
    note('cmd %d/%d: %s' % (n + 1, len(CMDS), cmd if len(cmd) < 200 else cmd[:200] + '...'))
    send(block)
    ok = wait(marker, CMD_TIMEOUT)
    note('cmd %d %s in %.0fs' % (n + 1, 'done' if ok else 'TIMED OUT', time.time() - tc))
    if not ok:
        failed += 1
        if os.environ.get('SUN3_STOP_ON_TIMEOUT', '1') == '1':
            break
    time.sleep(0.3)
note('all commands sent; %d timed out; total %.0fs' % (failed, time.time() - t0))
if args.keep:
    wait(b'\x00NEVER', args.keep_seconds)
finish(1 if failed else 0)
