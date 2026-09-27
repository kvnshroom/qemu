Sun-3 workstations (``sun3-60``, ``sun3x``)
===========================================

QEMU models two of Sun's 68k workstations: ``sun3-60`` is the Sun-3/60
"Ferrari" (a 68020 with the discrete Sun-3 MMU) and ``sun3x`` is the Sun 3/80
(a 68030 with its on-chip PMMU).  Both run their genuine boot PROMs, boot
SunOS 4.1.1 from an installed SCSI disk to a multi-user ``login:``, run
SunView and OpenWindows 2.0 on their own bwtwo screen and boot Linux; the
3/60 also boots NetBSD/sun3 1.5.2.  Boot transcripts and screenshots are kept
in ``sun3-tools/logs/`` and ``sun3x-tools/logs/``.

Sun-3/60 (``sun3-60``)
----------------------

The ``sun3-60`` machine models the Sun-3/60: a 68020 workstation built around
the discrete Sun-3 MMU, with on-board SCSI, Ethernet, a monochrome frame
buffer and the Sun Type-4 keyboard and mouse.  The 3/60 boot PROM runs its
full power-up self-test, boots a disk that carries a boot block, and otherwise
drops to the interactive ``>`` monitor prompt.

Emulated hardware
^^^^^^^^^^^^^^^^^

* 68020 CPU (``-m`` up to 24MB, default 16MB).
* The Sun-3 MMU: 8 contexts, per-context segment map (2048 segments of 128KB)
  selecting one of 256 PMEGs of 16 8KB page table entries, with
  valid/write/system protection, the four PTE type spaces (on-board memory,
  on-board I/O, VME16, VME32) and hardware accessed/modified bits.  The MMU
  control space (IDPROM, page and segment maps, context register, system
  enable register, bus error and diagnostic registers) is reached through
  ``MOVES`` with SFC/DFC=3, using the CPU's board function-code hooks.
* The boot state: while the system enable register's NOTBOOT bit is clear,
  every supervisor program fetch is redirected to the boot EPROM regardless
  of address (this also supplies the reset vectors).
* Bus timeouts: accesses that decode to nothing latch the TIMEOUT bit in the
  bus error register and raise a 68020 bus error, which is how the PROM sizes
  memory and probes for missing devices.
* Two Z8530 zs SCCs: zs0 is ttya/ttyb (``-serial``); zs1 carries the Sun
  Type-4 keyboard (channel A, US layout) and the mouse (channel B).
* Intersil ICM7170 RTC, the 100Hz system clock on interrupt level 5/7.
* The interrupt register with its soft interrupt, clock and video gates.
* Memory error (parity) registers, with enough forced-parity behaviour to
  satisfy the PROM's level-7 NMI self-test.
* 2KB configuration EEPROM; byte 0x1F selects the console.
* The on-board bwtwo frame buffer: 128KB of video RAM at obmem 0xFF000000,
  1152x900 monochrome, enabled by the VIDEO bit of the system enable
  register, with the resolution ID byte at 0xFF1C0000 and the level-4
  vertical-retrace interrupt.
* The "si" SCSI controller at obio 0x140000: an NCR5380 with the Am9516 UDC
  DMA engine and its FIFO, interrupt level 2.  DMA goes through Sun-3 DVMA:
  the 24-bit device address is translated by the CPU's own MMU as virtual
  ``0x0F000000 | addr`` in the current context.
* The "le" Am7990 LANCE Ethernet at obio 0x120000, interrupt level 3, with
  the same DVMA translation (only with a ``lance`` NIC, see `Networking`_).

Firmware
^^^^^^^^

``-bios`` (default name ``3.60_v3.0.1_rom``) takes the 64KB Sun-3/60 boot
PROM v3.0.1 (27C512 image, md5 ``28597f8a7f59b44395a398e152203505``, from the
``sunshack_bootroms`` collection on archive.org).  The machine does not start
without it; there is no ``-kernel`` path on the 3/60.

Machine options
^^^^^^^^^^^^^^^

``sunos-console=ttya|fb``
   Sets EEPROM byte 0x1F.  ``ttya`` (the default) puts the PROM, the kernels
   and SunOS's ``consconfig`` on the first serial port, so serial-driven runs
   are unchanged.  ``fb`` selects the on-board bwtwo screen and the Type-4
   keyboard/mouse: the PROM draws its banner, the Sun logo and the ``>``
   prompt on the screen, and SunOS links ``/dev/kbd`` and ``/dev/mouse``.

``-m``
   16MB by default, 24MB at most (larger values are rejected).

Running
^^^^^^^

.. code-block:: shell

   qemu-system-m68k -M sun3-60 -bios 3.60_v3.0.1_rom -display none -serial mon:stdio

After the self-test (the full memory test takes a while) the PROM prints its
banner on ttya, probes for a bootable disk and, finding none, drops to the
``>`` monitor prompt.  With ``sunos-console=fb`` the same happens on the
screen.  Without a GTK/SDL display the screen can be driven through VNC and
the HMP monitor:

.. code-block:: shell

   qemu-system-m68k -M sun3-60,sunos-console=fb -bios 3.60_v3.0.1_rom \
     -vnc 127.0.0.1:12 -monitor unix:/tmp/mon.sock,server,nowait

With the screen console the PROM writes nothing to ttya and SunOS runs no
getty there, so input goes in with HMP ``sendkey`` (``sun3-tools/hmp.py
/tmp/mon.sock 'type:root\n'`` types a string), ``mouse_move`` and
``mouse_button`` (a bitmask: 1 = left, 2 = right, 4 = middle), and pictures
come out with ``screendump`` (``sun3-tools/screendump-png.py`` makes a PNG).

Disks
^^^^^

.. code-block:: shell

   -drive file=disk.img,format=raw,if=scsi,bus=0,unit=N

``unit=N`` is the SCSI target.  SunOS names disks by target times two, so
qemu unit 0/1/3 are ``sd0``/``sd2``/``sd6``.  The PROM boot string
``b sd(controller,unit,partition)`` takes the unit field in hex as target
times 8: ``b sd(0,18,0)`` boots partition ``a`` of the disk at unit 3
(0x18 = 24 = 3 * 8).

The PROM always tries ``sd(0,0,0)`` first.  A disk at unit 0 with a boot
block is auto-booted; a blank disk there runs a zero boot block
(``Exception 0x2C at 0x00006000``) and the PROM drops to ``>``; with no disk
the ``le`` probe bus-errors (or RARPs, if a LANCE is configured) and the
monitor is entered.

Networking
^^^^^^^^^^

The LANCE exists only when a NIC of model ``lance`` is configured:

.. code-block:: shell

   -nic user,id=n0,model=lance[,hostfwd=tcp:127.0.0.1:2323-:23]

QEMU's implicit default NIC has no model and a plain ``-nic user`` cannot be
told apart from it, so both silently leave the 3/60 without a LANCE.  This is
deliberate: with the chip present a diskless auto-boot RARPs forever
(``Requesting Internet address for 8:0:20:36:0:60``) instead of bus-erroring
into the ``>`` monitor, which the kernel-injection tools below rely on.  The
MAC address is always the IDPROM's ``08:00:20:36:00:60``.

SunOS 4.1.1 autoconfigures ``le0 at obio 0x120000 pri 3``; with slirp:

.. code-block:: console

   # hostname sun3-60
   # ifconfig le0 10.0.2.15 netmask 255.255.255.0 up
   # route add default 10.0.2.2 1
   # ping 10.0.2.2 5

To make this permanent, populate ``/etc/hostname.le0``, ``/etc/hosts``,
``/etc/netmasks`` (``10 255.255.255.0``, keyed by the class-A network number)
and ``/etc/defaultrouter`` with a ``route add default`` line in
``/etc/rc.local`` (4.1.1's ``rc`` does not read ``defaultrouter``), and copy
a static ``hostname`` into ``/sbin`` for ``rc.boot``.  Beware ``guestfwd``
to a plain ``cat`` echo: ``telnet``'s option negotiation is echoed back and
the two loop at wire speed; use a one-shot responder.

Booting NetBSD
^^^^^^^^^^^^^^

NetBSD/sun3 1.5.2 boots from its miniroot on disk.  Decompress the miniroot
to a raw image and attach a copy at unit 0 (the INSTALL kernel writes to it):

.. code-block:: shell

   zcat miniroot.gz > nbmini.img
   qemu-system-m68k -M sun3-60 -bios 3.60_v3.0.1_rom -display none \
     -serial mon:stdio -drive file=nbmini.img,format=raw,if=scsi,bus=0,unit=0

The PROM auto-boots the disk, ``ufsboot`` loads ``netbsd.sun3``, the INSTALL
kernel attaches ``le0`` (if configured) and ``sd0``, mounts ``root on sd0a``
and reaches an interactive ``#`` shell, on ttya or (with
``sunos-console=fb``) on the screen.

The diskless path still works: ``sun3-tools/boot-netbsd-ramdisk.py`` waits
for the ``>`` prompt, injects the RAMDISK kernel (a.out header stripped) at
physical 0x4000 through the gdbstub and types ``g 4000``.  It reads
``SUN3_QEMU``, ``SUN3_ROM``, ``SUN3_KERNEL``, ``SUN3_GDBPORT``,
``SUN3_TIMEOUT`` and ``SUN3_EXTRA`` from the environment.  That kernel
reaches its welcome banner, but the installer shell then loops (see `Known
limitations`_).

Booting Linux
^^^^^^^^^^^^^

The Linux/sun3 kernel (``CONFIG_SUN3``, 68020, console ``hvc0`` tunnelled
through the PROM's romvec) is built from ``sun3-tools/sun3-linux.config`` on a
tree carrying ``sun3-tools/linux-sun3.patch`` (the ``hvc_sun3`` console driver
and an ``ELF_ET_DYN_BASE`` inside the 3/60's 28-bit user address space), with
the built-in initramfs described in ``sun3-tools/initramfs/README.md`` (a
static, non-LTO smolutils userland linked at 0x04000000 with 8KB page
alignment).  As the 3/60 has no ``-kernel`` path,
``sun3-tools/boot-linux.py`` injects the ELF segments through the gdbstub at
the monitor prompt and starts the kernel with ``g``:

.. code-block:: shell

   SUN3_QEMU=./qemu-system-m68k SUN3_ROM=3.60_v3.0.1_rom SUN3_KERNEL=vmlinux \
     SUN3_GDBPORT=11354 SUN3_EXTRA="-m 24M" SUN3_PROMPT="smolsh / > " \
     SUN3_CMDS="uname -a; cat /proc/cpuinfo" \
     python3 sun3-tools/boot-linux.py /tmp/sun3-linux.log

``SUN3_PROMPT``, ``SUN3_CMDS`` and ``SUN3_TIMEOUT`` drive the session once
``smolsh`` is up; ``/proc/cpuinfo`` reports a 68020 with the Sun-3 MMU.

Booting SunOS 4.1.1/sun3
^^^^^^^^^^^^^^^^^^^^^^^^

The SunOS 4.1.1 MINIROOT for the sun3 architecture boots from disk; since the
si bus has no tape or CD-ROM, the system is installed by hand from the
MINIROOT shell using extra disks.  The media (``miniroot_sun3`` and the
``sun3_proto_root``, ``sun3_usr``, ``sun3_kvm``, ``sun3_install`` and
``sun3_networking`` tar.Z sets) come from the Sun3 Archive (sun3arc.org).
Two constraints: the MINIROOT needs a swap partition (without a ``b``
partition the kernel panics ``can't create kernel process`` right after
``dump on``), and ``ufsboot`` truncates a partition's sector count to 16
bits, so a bootable root must stay below 65536 sectors (32MB).

``sun3-tools/install/mk-sun3-disks.py`` builds the images from the media: the
miniroot disk (with a 16MB swap), a labelled empty 300MB target and a sets
disk with one tar.Z per partition.  ``sun3-tools/install/install-drive.py``
boots the MINIROOT (``b sd(0,18,0)`` at the ``>`` prompt) and runs the
``cmds_stage1.py`` .. ``cmds_stage4_verify.py`` command lists: newfs/mount,
``zcat < /dev/rsd2X | tar xfBp -`` per set, the GENERIC kernel to
``/vmunix``, fstab, MAKEDEV, ``installboot``, fsck.  ``INSTALL-LOG.md`` there
records the stages (about 3.5 minutes in all).  The result boots unattended:

.. code-block:: shell

   sun3-tools/boot-sunos-installed.py /tmp/sunos.log -- \
     qemu-system-m68k -M sun3-60 -bios 3.60_v3.0.1_rom -display none \
     -drive file=sun3-installed.img,format=raw,if=scsi,bus=0,unit=0 \
     -nic user,model=lance

The PROM boots ``sd(0,0,0)`` to a multi-user ``login:`` in about 20 s; the
driver logs in as root, runs a command list (including ``cc``), then
``sync; reboot`` and logs in again.  4.1.1's getty selects even parity, so
some banner bytes arrive with bit 7 set; the drivers strip it.

For the window systems, ``mk-sun3-disks.py gfx`` builds a sets disk with the
SunView and OpenWindows sets, which ``boot-sunos-installed.py --halt
--cmds-file cmds_stage5_gfx.py`` extracts.  Then:

.. code-block:: shell

   qemu-system-m68k -M sun3-60,sunos-console=fb -bios 3.60_v3.0.1_rom \
     -vnc 127.0.0.1:12 -monitor unix:/tmp/mon.sock,server,nowait \
     -drive file=sun3-installed-gfx.img,format=raw,if=scsi,bus=0,unit=0 \
     -nic user,model=lance

The GENERIC kernel boots with its console on the screen to ``login:`` in
under 45 s.  Log in with ``hmp.py``, type ``suntools`` for SunView (stipple
desktop, CONSOLE, shelltool, clock; root menu on the right button) or
``openwin`` for OpenWindows 2.0 (OPEN LOOK cmdtool, File Manager,
Wastebasket; Workspace menu on the right button), and take
``screendump-png.py`` pictures.

Sun 3/80 (``sun3x``)
--------------------

The ``sun3x`` machine models the Sun 3/80, the "sun3x" kernel architecture: a
68030 whose on-chip PMMU replaces the Sun-3 MMU, with the same on-board
peripheral set as the 3/60.  There is no 68882, so the Linux kernel is built
with FPU emulation.

Emulated hardware
^^^^^^^^^^^^^^^^^

* 68030 CPU with the on-chip PMMU (``-m`` up to 64MB, default 16MB).
* Two Z8530 zs SCCs: the console chip at 0x62000000 (ttya/ttyb, ``-serial``)
  and the keyboard/mouse chip at 0x62002000 (Type-4 keyboard on channel A,
  mouse on channel B).
* The interrupt/enable register at 0x61001400 and the 100Hz level-5 tick.
* The MK48T02 NVRAM at 0x64000000 with the IDPROM at offset 0x7d8 and a
  real, ticking time-of-day clock in its last 8 bytes (host time, ``-rtc``
  honoured, WRITE/READ freeze bits and the ST bit).
* Memory-error/parity registers, enough for the PROM's parity self-tests and
  the kernel's memory-error handler.
* The printer/diagnostic port latch at 0x6f00003c.
* The "si" NCR5380 SCSI at 0x66000000 with its DMA/CSR block at 0x66001000,
  and the DVMA I/O mapper (IOMMU) at 0x60000000 whose page table the PROM
  builds (16MB of DVMA space).
* The "le" Am7990 LANCE Ethernet at 0x65002000 (``le0``), DMA through the
  same IOMMU; always present, MAC ``08:00:20:11:22:33``.
* The bwtwo frame buffer as a genuine P4-bus device: P4 register at
  0x50300000 (ID byte 0x01 = bwtwo, 1152x900), 128KB of video RAM aliased at
  +0x2000 for the kernel and +0x100000 for the PROM.
* A minimal Intel 82072 floppy controller at 0x6e000000 that answers the
  PROM's and the kernel's probes with "no drive".

Two modes
^^^^^^^^^

The machine starts in one of two ways, chosen by ``-bios``:

Linux mode (no ``-bios``)
   ``-kernel vmlinux`` is required; ``-initrd`` is passed as ``BI_RAMDISK``
   and ``-append`` as ``BI_COMMAND_LINE``.  The machine synthesises a boot
   PROM window (romvec, page-table root and the ``putc`` stubs ``head.S``
   needs) and enters the kernel directly.  ``sunos-console`` is ignored.

SunOS mode (``-bios sun3_80_v3.0.3.bin``)
   The genuine 128KB Sun 3/80 boot PROM is mapped at 0xfefe0000 with its
   private scratch/stack RAM below it, unmapped space bus-errors as on
   hardware, and the CPU starts at the PROM's reset vector.

Without either option the machine refuses to start (``sun3x needs -kernel
(Linux) or -bios (SunOS boot PROM)``).

Getting the PROM
^^^^^^^^^^^^^^^^

The Sun3 Archive keeps the four 3/80 PROM revisions at
``https://www.sun3arc.org/ROMs/3_80/``.  Use 3.0.3, the only revision that
boots SCSI with a dead NVRAM.  The ``3.0.x`` files are Intel-HEX and must be
decoded to a raw 131072-byte binary; ``2.9.2`` is already raw.

=====  =====================  ========  ============================================
Rev    file (.../ROMs/3_80/)  raw size  raw SHA1
=====  =====================  ========  ============================================
3.0.3  sun3_80_v3.0.3.gz      131072    ``e4be2dcbb29fc5c60ed9d838ab241c634fdd24e5``
3.0.2  sun3_80_v3.0.2.gz      131072    ``830187dfe58e65289533717a797d2c42da86ac4e``
3.0    sun3_80_v3.0.gz        131072    ``1e045b6f542aaf7808d6567c28a9e734a8c5d815``
2.9.2  sun3_80_v2.9.2.gz      131072    ``7ecd4a0d0988c1d1d53fd79ac16c8456ed73ace1``
=====  =====================  ========  ============================================

.. code-block:: shell

   curl -sLO https://www.sun3arc.org/ROMs/3_80/sun3_80_v3.0.3.gz
   gunzip sun3_80_v3.0.3.gz          # -> Intel-HEX
   # decode Intel-HEX -> raw 131072-byte binary:
   python3 - <<'PY'
   d=bytearray(); base=0
   for l in open("sun3_80_v3.0.3"):
       l=l.strip()
       if not l.startswith(':'): continue
       b=bytes.fromhex(l[1:]); n=b[0]; a=(b[1]<<8)|b[2]; t=b[3]
       if t==0:
           p=base+a
           if p+n>len(d): d.extend(b'\xff'*(p+n-len(d)))
           d[p:p+n]=b[4:4+n]
       elif t==2: base=((b[4]<<8)|b[5])<<4
       elif t==4: base=((b[4]<<8)|b[5])<<16
       elif t==1: break
   open("sun3_80_v3.0.3.bin","wb").write(d)
   PY
   sha1sum sun3_80_v3.0.3.bin   # -> e4be2dcbb29fc5c60ed9d838ab241c634fdd24e5

``-bios`` insists on exactly 131072 bytes; an undecoded HEX file is rejected.

Machine options
^^^^^^^^^^^^^^^

``sunos-console=ttya|fb``
   SunOS mode only: sets NVRAM byte 0x1F.  ``ttya`` (default) puts the PROM
   and SunOS console on the first serial port; ``fb`` on the bwtwo screen
   with the Sun keyboard/mouse.

``-m``
   16MB by default, 64MB at most.

Running the PROM
^^^^^^^^^^^^^^^^

.. code-block:: shell

   qemu-system-m68k -M sun3x -m 16M -bios sun3_80_v3.0.3.bin \
     -display none -serial mon:stdio

The PROM runs its power-on self-test on ttya:

.. code-block:: console

   Sun-3/80 Boot PROM Selftest (Rev. 3.0.3)
    (Press <Esc> to abort tests or <Cntrl-L> for the Loop Menu)
   System Enable Register Read Test {pass}      PROM Checksum Test {pass}
   TOD [Clock/Calender] Test {pass}
   ...
   Parity Memory Test {pass}   Parity Memory Forced Error Test {pass}

Press ``Esc`` on ttya (also with ``sunos-console=fb``: the PROM reads its
abort from the serial port, not from the keyboard).  Left to run unattended,
the self-test passes the TOD and both parity subtests and then wedges in its
phase-2 "ESP SCSI Controller Check", which the NCR5380 stand-in cannot
satisfy and which the ROM retries forever.  After ``Esc`` the PROM prints
``<Selftest aborted, Initializing the Monitor>``, initialises memory and
either auto-boots a disk or shows the ``>`` monitor prompt.

Booting SunOS 4.1.1
^^^^^^^^^^^^^^^^^^^

The sun3x MINIROOT (``miniroot_sun3x`` from
``https://www.sun3arc.org/BootTapes/Sun3x/``) is a ready UFS partition image
with the ``ufsboot`` boot block in blocks 1-15.
``sun3x-tools/install/sun3x-SUNOS-mkdisk.py`` lays it at LBA 0 of a 256MB raw
image and stamps a Sun disklabel over block 0:

.. code-block:: shell

   python3 sun3x-tools/install/sun3x-SUNOS-mkdisk.py miniroot_sun3x sunos-boot.img
   SUN3_LOG=/tmp/sunos.log python3 sun3x-tools/boot-sunos.py \
     qemu-system-m68k -M sun3x -m 16M -bios sun3_80_v3.0.3.bin \
     -drive file=sunos-boot.img,format=raw,if=scsi,bus=0,unit=3 -display none

``boot-sunos.py`` adds ``-serial stdio -monitor none``, sends ``Esc`` at the
self-test banner, types ``b sd(0,30,3)`` if a ``>`` prompt appears (the
PROM's own default, ``Boot: sd(0,18,0)``, boots the same unit-3 disk and
usually wins the race), waits for ``dump on`` and then for the MINIROOT's
``#`` shell, and runs a command list.  Its environment variables are
``SUN3_LOG``, ``SUN3_CAPTURE``, ``SUN3_CMDS`` (``;``-separated),
``SUN3_CMD_TIMEOUT``, ``SUN3_MAXLOG`` (console-flood guard), ``SUN3_PIDFILE``,
``SUN3_NO_ESC`` and ``SUN3_NOESC_TIMEOUT``.  SunOS names disks by target
times two, so unit 3 is ``sd6`` (``root on sd6a``), unit 0 ``sd0``, unit 1
``sd2``.

The disk-only install lives in ``sun3x-tools/install/``:
``mk-install-disks.py`` builds the empty 300MB target and the sets disk from
the five tar.Z sets, ``install-drive.py --cmds-file`` runs command lists in
the MINIROOT shell with long per-command timeouts, and ``INSTALL-LOG.md`` and
``NOTES.md`` record the attempts, the set layout and the ``ufsboot`` 16-bit
partition-size limit.  The installed system boots to multi-user with the
disk at unit 0:

.. code-block:: shell

   SUN3_LOG=/tmp/reboot.log python3 sun3x-tools/boot-sunos-reboot.py -- \
     qemu-system-m68k -M sun3x -m 16M -bios sun3_80_v3.0.3.bin \
     -drive file=installed.img,format=raw,if=scsi,bus=0,unit=0 \
     -nic user,model=lance -display none

The driver handles the first boot's ``Esc``, logs in as root, runs
``sync; reboot`` and verifies that the warm reboot returns to ``login:`` (a
warm reboot never prints the self-test banner: ``reboot(8)`` re-enters the
monitor, which boots its default device at once).

For graphics, boot the image that carries the window-system sets with the
screen console and a VNC display:

.. code-block:: shell

   qemu-system-m68k -M sun3x,sunos-console=fb -m 16M -bios sun3_80_v3.0.3.bin \
     -vnc 127.0.0.1:11 -serial unix:/tmp/ttya.sock,server,nowait \
     -monitor unix:/tmp/mon.sock,server,nowait \
     -drive file=installed-gfx.img,format=raw,if=scsi,bus=0,unit=0

Send ``Esc`` over the ttya socket, then log in and type with HMP ``sendkey``
(``sun3-tools/hmp.py`` serves both machines).  SunView (``suntools``) and
OpenWindows 2.0 (``/usr/openwin/bin/openwin``) both run with mouse focus and
keyboard input; OpenWindows 2.0 only accepts Type-4 keyboard layouts, which
is what the model answers.

Booting NetBSD
^^^^^^^^^^^^^^

NetBSD was only exercised on the 3/60; NetBSD/sun3x has not been tried.

Booting Linux
^^^^^^^^^^^^^

The Linux/sun3x kernel (``CONFIG_SUN3X``, ``CONFIG_M68030``,
``CONFIG_M68KFPU_EMU``) is built from ``sun3x-tools/sun3x-linux.config`` with
the same built-in initramfs as the 3/60.  ``sun3x-tools/`` carries two
patches: ``zs-console.patch`` (the native Z8530 ``ttyS0`` console driver
``drivers/tty/serial/sun3x_scc.c`` with its earlyprintk hook) and
``linux-sun3x.patch`` (the older ``hvc_sun3`` PROM-tunnelled console, kept as
a fallback).  The console is pinned by
``CONFIG_BOOTPARAM_STRING="console=ttyS0 earlyprintk"``, so ``-append`` has
no effect on that kernel.

.. code-block:: shell

   qemu-system-m68k -M sun3x -m 16M -kernel vmlinux -display none -serial mon:stdio

The log starts with ``bootconsole [debug0] enabled``, ``ttyS0`` takes over
(``sun3x-scc: ttyS0 MMIO:0x62000000 ... is a Z8530``) and an interactive
``smolsh`` comes up; ``/proc/cpuinfo`` shows a 68030 with a 68030 MMU.

Tools and evidence
------------------

``sun3-tools/`` (Sun-3/60):

===========================  ===================================================
File                         Purpose
===========================  ===================================================
``boot-netbsd-ramdisk.py``   Inject the NetBSD 1.5.2 RAMDISK kernel via gdbstub.
``boot-linux.py``            Inject a Linux/sun3 ELF kernel, drive the shell.
``boot-sunos-installed.py``  Boot an installed SunOS disk to ``login:``, run
                             commands, ``sync; reboot`` or ``--halt``.
``hmp.py``                   HMP helper: ``sendkey`` typing, mouse, screendump.
``screendump-png.py``        HMP screendump to PNG, with ink statistics.
``sun3-linux.config``        Kernel configuration for Linux/sun3.
``linux-sun3.patch``         ``hvc_sun3`` console and ``ELF_ET_DYN_BASE`` patch.
``initramfs/``               gen_init_cpio manifest, ``init.c``, smolutils recipe.
``install/``                 ``mk-sun3-disks.py``, ``install-drive.py``,
                             ``cmds_stage*.py``, ``INSTALL-LOG.md``.
``logs/``                    One transcript per OS (Linux, NetBSD miniroot,
                             SunOS installed and networked) and the
                             ``sun3-60-*.png`` screenshots.
===========================  ===================================================

``sun3x-tools/`` (Sun 3/80):

===========================  ===================================================
File                         Purpose
===========================  ===================================================
``boot-sunos.py``            Genuine PROM plus MINIROOT disk to ``#``, commands.
``boot-sunos-reboot.py``     Installed disk to ``login:``, ``sync; reboot``,
                             verify the warm reboot.
``sunos-symbols.py``         UFS walker and a.out ``nlist`` parser for symbols.
``sunos-read-verify.py``     Byte-exact verifier for guest disk-to-disk copies.
``sunos-write-verify.py``    Byte-exact verifier for guest-driven SCSI writes.
``sun3x-linux.config``       Kernel configuration for Linux/sun3x.
``zs-console.patch``         Native Z8530 ``ttyS0`` console driver.
``linux-sun3x.patch``        The older ``hvc_sun3`` console (fallback).
``install/``                 ``sun3x-SUNOS-mkdisk.py``, ``mk-install-disks.py``,
                             ``install-drive.py``, ``NOTES.md``, ``INSTALL-LOG.md``.
``logs/``                    Linux and SunOS installed multi-user transcripts
                             and the ``sunos-*.png`` screenshots.
===========================  ===================================================

Known limitations
-----------------

Both machines:

* Slirp answers neither RARP nor TFTP, so the PROM's ``b le()`` network boot
  only requests; a real netboot was not attempted.
* The si bus carries disks only: no tape or CD-ROM is modelled, which is why
  SunOS is installed from disk images.
* On the 3/80 the si command-complete interrupt bit is presented for exactly
  the first four CSR reads after the driver arms a command.  This
  access-count contract matches SunOS's driver exactly but is not how the
  hardware signals completion.

Sun-3/60:

* The NetBSD RAMDISK kernel injected through the gdbstub loops on its
  welcome banner: once per shell cycle an ``open(2)`` fails with ``EFAULT``
  on the pathname buffer.  The INSTALL kernel booted from the miniroot disk
  does not show this.
* An LTO-built ``smolsh`` dies with SIGSEGV on the 3/60 (not on the 3/80) as
  soon as it does a PATH search; build the initramfs without LTO.
* ``-nic user`` without ``model=lance`` silently leaves the machine without
  a LANCE (by design).  Linux/sun3's ``sun3lance`` driver was not exercised.

Sun 3/80:

* The PROM's unattended self-test never finishes (it wedges in the ESP SCSI
  subtest and the ROM retries a failing subtest forever); send ``Esc`` on
  ttya.
* The TOD clock ticks and carries the host date, but SunOS 4.1.1 reads the
  two-digit year as 1968 + yy, so a host clock in 2026 prints 1994.  This is
  SunOS's own limitation; ``-rtc base=`` is honoured.
* With the supplied Linux configuration ``-append`` is overridden by the
  kernel's ``CONFIG_BOOTPARAM_STRING``.
* NetBSD/sun3x has not been tried.
