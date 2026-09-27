#!/usr/bin/env python3
"""Boot the NetBSD/sun3 1.5.2 RAMDISK kernel on -M sun3-60.

Waits for the PROM monitor prompt on the serial socket, injects the
kernel image at physical 0x4000 through the gdbstub (the monitor's
context 0 maps low virtual == physical), then issues the monitor's
'g 4000' command - entering the kernel the same way the PROM's own
boot path would, with the PROM vector at 0x0FEF0000 live.

Usage: sun3-tools/boot-netbsd-ramdisk.py [logfile]
Environment: SUN3_QEMU, SUN3_ROM, SUN3_KERNEL, SUN3_TIMEOUT.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

QEMU = os.environ.get('SUN3_QEMU', './build/qemu-system-m68k')
ROM = os.environ.get('SUN3_ROM', '_sun3_assets/3.60_v3.0.1_rom')
KERNEL = os.environ.get('SUN3_KERNEL', '_sun3_assets/netbsd.RAMDISK')
TIMEOUT = float(os.environ.get('SUN3_TIMEOUT', '300'))
GDBPORT = os.environ.get('SUN3_GDBPORT', '11253')
SOCK = tempfile.mktemp(prefix='sun3-ttya-', suffix='.sock')
LOG = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/sun3-boot.log', 'wb')

# a.out (OMAGIC): strip the 32-byte header, keep text+data
with open(KERNEL, 'rb') as f:
    hdr = f.read(32)
    mag, text, data, bss, syms, entry = struct.unpack('>6I', hdr[:24])
    flat = f.read(text + data)
flatfile = tempfile.NamedTemporaryFile(prefix='sun3-kernel-', delete=False)
flatfile.write(flat)
flatfile.close()
load = entry & 0xFFFFFF  # 0xE004000 -> physical 0x4000
print(f'kernel: text+data {text + data:#x} entry {entry:#x} load {load:#x}')

qemu = subprocess.Popen([QEMU, '-M', 'sun3-60', '-bios', ROM,
    '-display', 'none',
    '-serial', f'unix:{SOCK},server=on,wait=off',
    '-gdb', f'tcp:127.0.0.1:{GDBPORT}'] +
    os.environ.get('SUN3_EXTRA', '').split())
time.sleep(1)
s = socket.socket(socket.AF_UNIX)
s.connect(SOCK)
s.settimeout(1)

buf = b''
def pump(t, upto=None):
    global buf
    end = time.time() + t
    while time.time() < end:
        if upto and upto in buf:
            return True
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        if d:
            buf += d
            LOG.write(d)
            LOG.flush()
            sys.stdout.write(d.decode('latin1'))
            sys.stdout.flush()
    return upto is not None and upto in buf

if not pump(120, b'>'):
    print('\n*** no monitor prompt')
    qemu.kill()
    sys.exit(1)

print('\n*** monitor prompt; injecting kernel via gdbstub')
subprocess.run(['gdb-multiarch', '-batch', '-nx',
    '-ex', 'set architecture m68k',
    '-ex', f'target remote 127.0.0.1:{GDBPORT}',
    '-ex', f'restore {flatfile.name} binary {load:#x}',
    '-ex', 'detach'], capture_output=True, check=True)
os.unlink(flatfile.name)

s.sendall(b'g %x\r' % load)
pump(TIMEOUT)
qemu.kill()
