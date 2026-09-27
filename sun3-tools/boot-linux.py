#!/usr/bin/env python3
"""Boot the Linux/sun3 (CONFIG_SUN3) kernel on QEMU -M sun3-60.

Same mechanism as the NetBSD RAMDISK boot: let the resident 3/60 PROM
run its power-on init (so the romvec at 0x0FEF0000 is fully live), then
at the monitor '>' prompt inject the kernel into low RAM through the
gdbstub (the monitor's context 0 identity-maps low VA==PA) and hand
control over with the monitor 'g' command.

The Linux sun3 kernel is linked at KERNBASE=0x0E000000 but is loaded at
physical 0; sun3-head.S expects to start executing at physical
(entry - KERNBASE) with the PROM's low identity map live, then copies
the bootloader's segmaps up to KERNBASE.

Env: SUN3_QEMU, SUN3_ROM, SUN3_KERNEL (vmlinux ELF), SUN3_GDBPORT, SUN3_PROMPT,
     SUN3_TIMEOUT, SUN3_EXTRA (extra qemu args).
Usage: boot-linux.py [logfile]
"""
import os, socket, struct, subprocess, sys, tempfile, time

QEMU=os.environ.get('SUN3_QEMU','/tmp/sun3-build/qemu-system-m68k')
ROM=os.environ.get('SUN3_ROM','/workspace/src/qemu-amiga/_sun3_assets/3.60_v3.0.1_rom')
KERNEL=os.environ.get('SUN3_KERNEL','/tmp/sun3-kbuild/vmlinux')
GDBPORT=os.environ.get('SUN3_GDBPORT','1242')
TIMEOUT=float(os.environ.get('SUN3_TIMEOUT','300'))
KERNBASE=0x0E000000
SOCK=tempfile.mktemp(prefix='sun3-ttya-',suffix='.sock')
LOG=open(sys.argv[1] if len(sys.argv)>1 else '/tmp/sun3-linux-boot.log','wb')

# Parse ELF program headers -> list of (paddr, bytes)
with open(KERNEL,'rb') as f:
    elf=f.read()
assert elf[:4]==b'\x7fELF'
is64=elf[4]==2
be=elf[5]==2
en='>' if be else '<'
e_entry=struct.unpack_from(en+'I',elf,24)[0]
e_phoff=struct.unpack_from(en+'I',elf,28)[0]
e_phentsize=struct.unpack_from(en+'H',elf,42)[0]
e_phnum=struct.unpack_from(en+'H',elf,44)[0]
segs=[]
end_pa=0
for i in range(e_phnum):
    off=e_phoff+i*e_phentsize
    p_type,p_offset,p_vaddr,p_paddr,p_filesz,p_memsz=struct.unpack_from(en+'IIIIII',elf,off)
    if p_type==1 and p_memsz>0:  # PT_LOAD
        pa=p_vaddr-KERNBASE
        if p_filesz>0:
            segs.append((pa,elf[p_offset:p_offset+p_filesz]))
        if p_memsz>p_filesz:  # zero the BSS tail (sun3-head.S doesn't)
            segs.append((pa+p_filesz, b'\x00'*(p_memsz-p_filesz)))
        end_pa=max(end_pa,pa+p_memsz)
# Minimal bootinfo at _end, mirroring a Sun booter: BI_MACHTYPE(MACH_SUN3)
# then BI_LAST.  This tree's setup_arch NULL-derefs without a BI_MACHTYPE.
end_pa=(end_pa+3)&~3
bootinfo=struct.pack(en+'HHI',1,8,5)+struct.pack(en+'HH',0,0)
segs.append((end_pa,bootinfo))
entry_pa=e_entry-KERNBASE
tot=sum(len(b) for _,b in segs)
print(f'kernel {KERNEL}: entry {e_entry:#x} -> pa {entry_pa:#x}; {len(segs)} segs, {tot/1e6:.2f} MB')
for pa,b in segs:
    print(f'  seg pa {pa:#x} size {len(b):#x}')

q=subprocess.Popen([QEMU,'-M','sun3-60','-bios',ROM,'-display','none',
  '-serial',f'unix:{SOCK},server=on,wait=off',
  '-gdb',f'tcp:127.0.0.1:{GDBPORT}',
  '-monitor','unix:/tmp/sun3-mon.sock,server,nowait']+os.environ.get('SUN3_EXTRA','').split())
time.sleep(1)
s=socket.socket(socket.AF_UNIX); s.connect(SOCK); s.settimeout(1)
buf=b''
def pump(t,upto=None):
    global buf; end=time.time()+t
    while time.time()<end:
        if upto and upto in buf: return True
        try: d=s.recv(4096)
        except socket.timeout: continue
        if d:
            buf+=d; LOG.write(d); LOG.flush()
            sys.stdout.write(d.decode('latin1')); sys.stdout.flush()
    return upto is not None and upto in buf

if not pump(120,b'>'):
    print('\n*** no monitor prompt'); q.kill(); sys.exit(1)
print('\n*** monitor prompt reached; injecting kernel via gdbstub')

# write each segment via gdb restore
gdb_ex=['-ex','set architecture m68k','-ex',f'target remote 127.0.0.1:{GDBPORT}',
        '-ex','set endian big']
tmps=[]
for pa,b in segs:
    tf=tempfile.NamedTemporaryFile(prefix='sun3seg-',delete=False); tf.write(b); tf.close()
    tmps.append(tf.name)
    gdb_ex+=['-ex',f'restore {tf.name} binary {pa:#x}']
gdb_ex+=['-ex','detach']
r=subprocess.run(['gdb-multiarch','-batch','-nx']+gdb_ex,capture_output=True,text=True)
sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
for t in tmps: os.unlink(t)

print(f'\n*** starting kernel: g {entry_pa:x}')
s.sendall(b'g %x\r' % entry_pa)

# Optionally drive the interactive shell: SUN3_CMDS is a ';'-separated
# list of shell commands sent once the '# ' prompt appears.
cmds=os.environ.get('SUN3_CMDS','')
if cmds:
    if pump(TIMEOUT, os.environ.get('SUN3_PROMPT', '# ').encode()):
        print('\n*** shell prompt reached; sending commands')
        for c in cmds.split(';'):
            time.sleep(0.5); s.sendall(c.encode()+b'\n'); pump(8)
        time.sleep(1); pump(3)
    else:
        print('\n*** shell prompt not seen')
else:
    pump(TIMEOUT)
q.kill()
