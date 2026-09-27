#!/usr/bin/env python3
"""
boot-sunos-installed.py -- boot the installed SunOS 4.1.1/sun3 disk on QEMU
-M sun3-60 (target at unit 0, PROM auto-boots sd(0,0,0)), wait for the
multi-user "login:" prompt on ttya, log in as root, run a command list, then
`sync; reboot` and log in again to prove the warm reboot comes back.

getty std.9600 sets even parity, and the 4.1.1 tty layer computes the parity
bit in software (partab), so the login banner arrives with bit 7 set on some
bytes ("Amnesiac login:" reads as "Aíîeóiác loçiî:"); matching and the
transcript strip bit 7.

usage: boot-sunos-installed.py LOG [--no-reboot] [--halt] [--cmds-file F.py]
                               -- <qemu argv (no -serial/-monitor)>
  --cmds-file F.py  run that file's CMDS list instead of the built-in smoke test
                    (root's shell is csh: use $status, keep lines under 256 chars)
  --halt            after the commands `sync; sync; halt` and wait for the PROM
                    (used to shut the disk down cleanly before an fsck); implies
                    --no-reboot
env: SUN3_LOGIN_TIMEOUT (600), SUN3_CMD_TIMEOUT (300)
"""
import importlib.util, os, socket, subprocess, sys, tempfile, time, threading

argv = sys.argv[1:]
logname = argv.pop(0)
do_reboot = True; do_halt = False; cmds_file = None
while argv and argv[0].startswith('--') and argv[0] != '--':
    opt = argv.pop(0)
    if opt == '--no-reboot':
        do_reboot = False
    elif opt == '--halt':
        do_halt = True; do_reboot = False
    elif opt == '--cmds-file':
        cmds_file = argv.pop(0)
    else:
        sys.exit('unknown option ' + opt)
if argv and argv[0] == '--':
    argv.pop(0)
LOGIN_TIMEOUT = float(os.environ.get('SUN3_LOGIN_TIMEOUT', '600'))
CMD_TIMEOUT = float(os.environ.get('SUN3_CMD_TIMEOUT', '300'))
CMDS = ['uname -a', 'hostname', 'date', 'df', 'mount', 'ps -ax', 'who',
        "echo 'main(){printf(\"hello from sunos on the sun3\\n\");}' > /tmp/h.c",
        'cc -o /tmp/h /tmp/h.c', '/tmp/h', 'ls -l /vmunix /boot /sbin', 'sync']
if cmds_file:
    spec = importlib.util.spec_from_file_location('cmds_mod', cmds_file)
    mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
    CMDS = mod.CMDS

LOG = open(logname, 'wb')
SOCK = tempfile.mktemp(prefix='sun3-ttya-', suffix='.sock')
q = subprocess.Popen(argv + ['-serial', f'unix:{SOCK},server=on,wait=off', '-monitor', 'none'])
time.sleep(1)
s = socket.socket(socket.AF_UNIX); s.connect(SOCK); s.settimeout(0.5)
buf = b''; lock = threading.Lock(); stop = False; t0 = time.time()

def note(msg):
    line = ('\n*** [%s +%4.0fs] %s\n' % (time.strftime('%H:%M:%S'), time.time() - t0, msg)).encode()
    sys.stdout.write(line.decode()); sys.stdout.flush(); LOG.write(line); LOG.flush()

def reader():
    global buf
    while not stop:
        try: d = s.recv(4096)
        except socket.timeout: continue
        except OSError: break
        if d:
            d = bytes(c & 0x7f for c in d)
            with lock:
                buf += d
                if len(buf) > (1 << 20): buf = buf[-(1 << 19):]
            LOG.write(d); LOG.flush()
            sys.stdout.write(d.decode('latin1')); sys.stdout.flush()
threading.Thread(target=reader, daemon=True).start()

def wait(pat, t, nudge=None):
    global buf
    end = time.time() + t; last = time.time()
    while time.time() < end:
        with lock:
            i = buf.find(pat)
            if i >= 0:
                buf = buf[i + len(pat):]; return True
        if nudge and time.time() - last > 15:
            s.sendall(nudge); last = time.time()
        time.sleep(0.2)
    return False

def send(b): s.sendall(b)

def finish(rc):
    global stop
    stop = True; time.sleep(0.5)
    try: q.kill()
    except Exception: pass
    sys.exit(rc)

def login_and_run(cmds, tag):
    if not wait(b'login:', LOGIN_TIMEOUT, nudge=b'\r'):
        note(tag + ': no login: prompt'); finish(1)
    note(tag + ': login: prompt; logging in as root')
    time.sleep(0.5); send(b'root\r')
    if not wait(b'# ', 120):
        note(tag + ': no root shell prompt'); finish(1)
    note(tag + ': root shell')
    for n, cmd in enumerate(cmds):
        marker = ('__DONE_%s_%d' % (tag, n)).encode()
        with lock: buf = b''
        tc = time.time()
        send(cmd.encode() + b'\r' + ("echo __DO''NE_%s_%d\r" % (tag, n)).encode())
        ok = wait(marker, CMD_TIMEOUT)
        note('%s cmd %d (%s) %s in %.0fs' % (tag, n + 1, cmd[:40], 'done' if ok else 'TIMED OUT', time.time() - tc))
        if not ok: finish(1)

login_and_run(CMDS, 'boot1')
if do_reboot:
    note('sync; reboot')
    send(b'sync; reboot\r')
    if not wait(b'rebooted by root', 120):
        note('no "rebooted by root"')
    login_and_run(['uname -a', 'uptime', 'df', 'sync'], 'boot2')
if do_halt:
    note('sync; sync; halt')
    send(b'sync; sync; halt\r')
    # "halt: halted by root" is syslog, before the kernel's own sync: wait for
    # the kernel's "syncing file systems... done" and then the PROM.
    # (the kernel prints the dirty-buffer countdown in between: "syncing
    # file systems... 4 1 1 1 1 1 done")
    if not wait(b'syncing file systems...', 120) or not wait(b'done', 60):
        note('no "syncing file systems... done" from the kernel'); finish(1)
    if not wait(b'Halted', 60):
        note('no "Halted" from the kernel'); finish(1)
    wait(b'>', 30)
    time.sleep(2)
note('ALL OK')
finish(0)
