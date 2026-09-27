# SunOS 4.1.1/sun3 install on QEMU `-M sun3-60` -- 2026-09-27

Result: `/tmp/sunos3-install/sun3-installed.img` (300 MiB sparse, 49 MiB
used) boots alone from the 3/60 v3.0.1 PROM to a multi-user `login:` in
20 s, root logs in, `cc` works, `sync; reboot` comes back to `login:` in
about 40 s.  Transcript: `sun3-tools/logs/sunos-4.1.1-installed-sun3-60.log`.

## Media / disks

`mk-sun3-disks.py` (same file as `/tmp/sunos3-install/mk-sun3-disks.py`)
builds, from `/tmp/sunos-media`:

* `miniroot-sun3.img` -- miniroot_sun3 at LBA 0, label `a` = 14000 sectors,
  **`b` = 16 MiB swap** (the MINIROOT kernel panics "can't create kernel
  process" without a swap partition), 23 MiB total.
* `target-sun3.img` -- empty labelled 300 MiB disk, `a` root 127 cyl =
  65024 sectors (ufsboot truncates a partition's sector count to 16 bits,
  so the root it opens must stay below 65536), `b` swap 16 MiB, `g` usr
  252 MiB (only the running kernel opens it; fine).
* `sets-sun3.img` -- the five tar.Z sets, one per partition (a root, b usr,
  d kvm, e install, f networking), verified by sha1 readback.

## Emulator unit numbering and boot strings

SunOS names the disks by SCSI target: qemu `unit=0` -> sd0, `unit=1` ->
sd2 (slave 8), `unit=3` -> sd6 (slave 24).  The PROM boot string takes
the unit field in hex: `b sd(0,18,0)` boots target 3 partition a.

The PROM always tries sd(0,0,0) first.  With the blank target there it
runs a zero boot block (`Exception 0x2C at 0x00006000`) and drops to its
`>` prompt, where the driver types `b sd(0,18,0)`.  Once `installboot`
has run, sd(0,0,0) boots the installed system, so the later stages put
the miniroot at unit 0 (auto-booted) and the target at unit 1 = sd2.

## Driver

`install-drive.py --cmds-file cmds_stageN.py [--boot 'b sd(0,18,0)'|auto] -- <qemu argv>`
Serial console on a unix socket; each command is followed by
`echo __DO''NE_n` and the driver waits for `__DONE_n`.  Commands must stay
under 256 characters: the 4.1.1 tty silently discards a longer canonical
input line (the 3/80's long awk patch line was lost this way).

## Stages (wall clock on a 12-core host; the 3/80 needed hours)

| stage | what | time |
|---|---|---|
| 1 (`cmds_stage1.py`, miniroot unit 3, target unit 0, sets unit 1) | device check, `newfs -v /dev/rsd0a` (2 s), `newfs -v /dev/rsd0g` (14 s), fsck, mount at /a, root set (`zcat < /dev/rsd2a \| tar xfBp -`, 2 s) | 51 s |
| 2 (`cmds_stage2.py`) | usr set into /a/usr (35 s), kvm set into /a/usr/kvm, install + networking into /a/usr, `cp /a/usr/kvm/stand/vmunix /a/vmunix`; fsck: root 177 files, usr 3041 files / 30.8 MB | 75 s |
| 3 (`cmds_stage3.py`) | `/etc/fstab` (sd0a /, sd0g /usr, sd0b swap), `/etc/hosts`, `MAKEDEV std sd0 sd1 sd2 st0 pty0`, `/sbin/{init,mount,ifconfig,intr}` from `/usr/kvm/boot` and `/sbin/sh` from `/usr/kvm/stand/sh`, `ln -s ../usr/etc/getty /etc/getty`, `cp /usr/kvm/stand/boot.sun3 /boot`, `installboot /a/boot /a/usr/kvm/mdec/bootsd /dev/rsd0a` | 45 s |
| 4 (`cmds_stage4_verify.py`, miniroot unit 0 auto-boot, target unit 1 = sd2) | fsck both, check config, umount | 29 s |

Not done, deliberately: no `/etc/hostname.le0` (the 3/60 model has no
LANCE; hostname stays empty, the banner reads "Amnesiac login:"); ttya's
own ttytab line stays `off` -- the EEPROM console is ttya, so the
`console` getty already owns that port and a second getty on /dev/ttya
would fight it for input; root has no password (proto_root default);
`/etc/rc` untouched (see below).

## What differed from the 3/80

* `/etc/rc.boot` runs: the first boot printed `checking filesystems`,
  fsck'd both partitions and mounted /usr, so the `/etc/rc` mount patch
  the 3/80 needed is not applied here.
* The login banner arrives with bit 7 set on some bytes: getty std.9600
  selects even parity and the 4.1.1 tty layer computes the parity bit in
  software (`partab`); the drivers strip bit 7.

## Boot / login check

`sun3-tools/boot-sunos-installed.py LOG -- qemu-system-m68k -M sun3-60 -bios 3.60_v3.0.1_rom -display none -drive file=sun3-installed.img,format=raw,if=scsi,bus=0,unit=0`
waits for `login:`, logs in as root, runs uname/hostname/date/df/mount/
ps -ax/who/cc hello-world/ls/sync, then `sync; reboot`, waits for
`login:` again and logs in again.  Cold boot to login 20 s; reboot to
login 38 s.

## Stage 5 (2026-09-27, session 38): window-system sets

`mk-sun3-disks.py gfx` -> sets disk (a sunview_users, b sunview_programmers,
d openwindows_users, e openwindows_fonts, f openwindows_programmers), then
`boot-sunos-installed.py --halt --cmds-file cmds_stage5_gfx.py` on the
installed system (unit 0) with the sets disk at unit 1 = sd2: 92 s in all
(login 24 s, tar 60 s), /usr 31 % used, `MAKEDEV win`, `/dev/fb -> bwtwo0`,
hosts entry `sun3-60`; `cmds_fsck_sd2.py` from the MINIROOT afterwards.
csh reports `$status` 32781 (QUOTE|SIGPIPE: zcat killed when tar closes the
pipe) for every set; the trees are complete.  Result:
`/tmp/sunos3-install/sun3-installed-gfx.img`; hw/m68k/sun3-NOTES.md.
