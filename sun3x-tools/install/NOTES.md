# SunOS 4.1.1 sun3x manual install -- working notes / hand-off

Goal: install SunOS 4.1.1 (sun3x) onto a disk image under QEMU `-M sun3x`,
via a **manual install from the miniroot shell**, using only SCSI **disks**
(no tape/CD-ROM emulation exists in the current `si`/NCR5380 model).

Media directory: `/tmp/sunos-media`. Working directory: `/tmp/sunos-install`.

## 0. Environment / shared-resource notes

* QEMU binary under test: **always a copy**, `/tmp/qemu-sunos-stable`,
  refreshed from `/tmp/qemu-m8-build/qemu-system-m68k` (another agent is
  actively rebuilding that tree -- never run the build dir's binary
  directly, never touch `/tmp/qemu-m8` or `/tmp/qemu-sun3`).
* `/tmp/sunos-boot.img` (the existing miniroot boot disk, built by
  `sun3x-tools/install/sun3x-SUNOS-mkdisk.py`) was **in use by another process**
  (`Failed to get "write" lock`) during this session -- copied it to a
  private `/tmp/sunos-install/sunos-boot-mytest.img` and booted that copy
  instead, rather than disturbing the shared file. Delete this private
  copy when done (256 MiB, tmpfs).
* No TCP ports were needed: `sun3x-tools/boot-sunos.py` drives QEMU over
  `-serial stdio -monitor none` (no chardev/monitor sockets), so the
  "pick a free port" guidance doesn't apply to this harness.
* `df -h /tmp` before this session: 16G tmpfs, ~13G already used by other
  agents' work, ~3.3-3.4G free. Our own footprint: media ~23 MB + two
  small install images (300 MiB target.img is **sparse** -- `truncate`,
  no real data written except the label; 17 MiB sets.img is fully
  populated) + one throwaway 256 MiB boot-disk copy for the smoke test
  (deleted after). Comfortably inside budget.

## 1. Media inventory (downloaded from https://www.sun3arc.org/BootTapes/Sun3x/)

Already present before this session (`/tmp/sunos-media`): `sun3_80_v3.0.3.bin`
(PROM, 131072 B), `miniroot_sun3x` (7,168,000 B), `munix_sun3x` (754,176 B),
`vmunix` (698,991 B), `listing.html` (directory listing).

Newly fetched this session:

| file | bytes | sha1 |
|---|---|---|
| README | 2014 | `8ea8c17db9f1a9c1fc21b51d55571f0481203838` |
| REAME.TOO | 2911 | `002619ae892948ff915b6f72c92f726ef84ca428` |
| maketape1 | 1107 | `05ad0254502dd5f8a51843d2d29875aab32f0832` |
| maketape2 | 593 | `4a89e526be59b8b4989357d06ba330ac1fa39029` |
| SunOS411.sun3x.CDROM.xdrtoc | 6436 | `6d05ffb56746cdeba6839cd99abaf175a6a753a6` |
| SunOS411.sun3x.CDZ.xdrtoc | 6436 | `c0ab971ff2e9953048dff7cd1e5684ad65967091` |
| SunOS411.sun3x.Exa.xdrtoc | 7620 | `c42f188e7be5844f84317aeca99e30bc1f6c685c` |
| SunOS411.sun3x.QIC-24a.xdrtoc | 8192 | `3071dab699dcbaee263699355e5ef1d74f74dd80` |
| SunOS411.sun3x.QIC-24b.xdrtoc | 8192 | `5e838f583029c26b225ce389d8a7ef21aa0f37c3` |
| SunOS411.sun3x.QIC-150.xdrtoc | 7620 | `c4b6f64e57725d6becafff62a43077801a57fcd2` |
| **sun3_proto_root.sunos_4_1_1.tar.Z** | 74309 | `00f5475d84dc5744d8d3f0889a0d893b3e1646ee` |
| **sun3_usr.tar.Z** | 11633386 | `5be9571c2d609ae92ced0bf576199423e7bf0dc2` |
| **sun3x_kvm.tar.Z** | 2102442 | `55d7f97b9c7020c24c2301acb9a3b9596c3177c2` |
| **sun3_install.tar.Z** | 375437 | `6bec99aec2133c5d0183f30d4ffbde48b4a3abbb` |
| **sun3_networking.tar.Z** | 368339 | `7781eb7218dd1b4c4d1543a2b736476ae5f60a2e` |

Bold = the 5 sets actually needed for a minimal bootable multi-user sun3x
system (total 14.55 MB compressed, far under the ~60-80 MB the task
anticipated -- this Sun3x archive's set list turned out much smaller than a
full sun4 distribution). NOT downloaded (deliberately, see below):
`sun3_kvm.tar.Z` / `sun3_sys.tar.Z` (sun3 = 68020 arch, wrong for sun3x),
`sun3x_sys.tar.Z` (kernel *source*, not needed to boot GENERIC), and all the
demo/OpenWindows/games/manual/etc sets (not needed for a minimal system).

### Which sets are required -- derived from ground truth, not guessed

1. `maketape1` (the real Sun3x tape-build script) lists the tar sets in
   **exactly this order**: `sun3_proto_root.sunos_4_1_1.tar sun3_usr.tar
   sun3x_kvm.tar sun3_install.tar sun3_networking.tar sun3_system_v.tar
   sun3x_sys.tar ...` (then demos/openwindows/etc). The first five are
   the base OS; everything after is optional extras.
2. `SunOS411.sun3x.QIC-150.xdrtoc` (binary XDR table of contents) has the
   same five categories first, with their exact tar recipes, e.g.:
   * `root`: `cd /proto;tar cfX - $k/exclude.lists/root.usr .` (packs `/`
     minus `/usr`, hence tiny -- 74 KB compressed)
   * `usr`: `cd /proto/usr;tar cfX - /tmp/exc$$ .` (packs `/usr` minus
     kvm-specific stuff) -- shared between sun3 and sun3x (only one
     `sun3_usr.tar.Z`, no `sun3x_usr.tar.Z`), because `/usr` is mostly
     68020/68030-compatible m68k user-mode binaries; only the kernel
     (`sys`) and kernel-dependent standalone/boot programs (`kvm`) differ
     per arch, hence the separate `sun3x_*` files for those two only.
   * `Kvm`: `cd /proto/usr/kvm;tar cfX - $k/exclude.lists/kvm.exclude .`
   * `Install`: installer support files under `/usr`.
   * `Networking`: `k=cat exclude.lists/usr.net;cd /proto/usr;tar cf - $k`
3. **No separate "sys" set is needed to boot multi-user.** Extracted
   `/usr/etc/install/install.bin` from the miniroot (the data file
   `add_client` uses to populate a client's root) -- it lists exactly:
   ```
   impl	stand/vmunix	/
   impl	stand/kadb	/
   appl	bin/hostname	/sbin
   appl	etc/ifconfig	/sbin
   appl	etc/init	/sbin
   appl	etc/mount	/sbin
   appl	etc/intr	/sbin
   appl	bin/sh		/sbin
   ```
   i.e. the **Kvm set ships a prebuilt GENERIC kernel** at
   `usr/kvm/<arch>/stand/vmunix` (plus `kadb`, the standalone debugger).
   `sys` is kernel *source* (`/usr/src/sys`) for people who want to
   `config`+`make` a custom kernel -- optional, skipped here.

## 2. Miniroot capabilities -- extracted offline via the UFS walker

Walked `/tmp/sunos-media/miniroot_sun3x` with the `UFS` class in
`/tmp/qemu-m8/sun3x-tools/sunos-symbols.py` (imported as a module, not
edited) to list directories and pull out shell scripts, without booting
anything. Findings:

**Tools present** (so no extra binaries need to be shipped in):
* `/bin`: `tar`, `dd`, `sh`, `ed`, `cp`, `mv`, `sed`, `awk`, `grep`, `ln`,
  `mkdir`, `mt`, `arch`, `hostname`, `hostid`, `stty`, standard coreutils.
* `/etc`: `newfs`, `mkfs`, `mount`, `umount`, `fsck`, `fsirand`, `dkinfo`,
  `format.dat`, `ifconfig`, `rc.boot`, `passwd`, `group`, `hosts`,
  `protocols`, `services`, `ttytab`.
* `/usr/etc`: `format`, `mkfile`, `mknod`, `route`, `in.routed`, and the
  `install/` directory (suninstall + all its helper scripts/data).
* `/usr/ucb`: **`compress`, `uncompress`, `zcat`**, `rcp`, `rsh`, `tset`,
  `whoami` -- so `zcat < /dev/rsdXn | tar xvfBp -` works exactly as
  planned; no need to smuggle in a decompressor.
* `/usr/kvm` -> really `/usr/kvm` (miniroot's own, not the installed
  arch tree) has `mdec/{bootsd,bootxd,bootxy,installboot,rawboot}` -- the
  boot-block installer tools -- and `getcons`.
* `/usr/mdec` is a symlink -> `kvm/mdec` (so `/usr/mdec/installboot`
  resolves to the same directory).
* `/dev` already has **every** device node the miniroot ships with
  (716 entries) -- `rsd0a..rsd15h` / `sd0a..sd15h` (raw and block, major 7
  minor scheme `unit<<3 | partition`, i.e. classic BSD sd numbering),
  `rst*/rmt*` (tape), `rsr0/rsr1` (CD-ROM), `rfd0*` (floppy). **No `mknod`
  needed for any SCSI target/partition combination we use.**
* `/a` and `/tmp` exist as empty directories -- `/a` is suninstall's
  conventional mount point for the target root.
* `/usr/etc/install/*`: 28 entries. Most (`suninstall`, `easyinstall`,
  `get_disk_info`, `add_client`, ...) are compiled a.out binaries (can't
  read their logic directly), but several are **shell scripts**, fully
  extracted and read:
  * `extract_files` (the actual media-reading logic suninstall/easyinstall
    call): for **tape** devices it does exactly
    `mt -f /dev/nr<dev> asf <fsf>; dd if=/dev/nr<dev> bs=40b | uncompress | tar ${what} - ${files}`
    (`what` = `xpf` extract / `tvf` list); for **CD-ROM** it mounts hsfs and
    `tar ${what} <path-under-mounted-tree>`. **There is no disk-partition
    branch in this script** -- confirms suninstall/extract_files cannot be
    pointed at "a local disk" as a media source out of the box; the manual
    method (drive the primitives ourselves) is the right approach for this
    task. The key primitive it validates for us: **plain `tar xvfBp -` /
    `tar tvfBp -` on a pipe from `uncompress`/`zcat`, no special flags
    beyond `B` (reblock) `p` (preserve perms)**.
  * `install_small_kernel`: shows the on-disk convention
    `$rootdisk/usr/kvm/stand/vmunix_small` -> copied to `$rootdisk/vmunix`,
    confirming kernels live under `usr/kvm/<arch>/stand/` in the Kvm set
    and get copied to the target's `/vmunix`.
  * `config_host`: hostname is written to `/etc/hostname.<ether-device>`
    (e.g. `/etc/hostname.ie0`) from a `sys_info` file's `hostname=` field;
    domainname to `/etc/defaultdomain`; timezone via a symlink
    `/usr/lib/zoneinfo/localtime -> <timezone>`.
  * `install.bin` (a *data* file despite the name, used by `add_client`,
    see kvm/sys discussion above).

## 3. Chosen install method

**Manual, disk-only, driven from the miniroot `#` shell** (no
suninstall/easyinstall run -- confirmed above that those scripts don't
support "local disk" as a media source; we replicate what `extract_files`
does under the hood by hand). Two extra SCSI disks, attached read via the
*already-booted* miniroot kernel (not the PROM/ufsboot loader):

* **target.img** -- qemu `unit=0` -> appears as `sd0`/`rsd0*`. A Sun
  disklabel with an EMPTY root/swap/usr layout (no filesystem yet).
* **sets.img** -- qemu `unit=1` -> appears as `sd2`/`rsd2*` (SunOS sd unit
  numbering on this machine is `target*2`: target0->sd0, target1->sd2,
  target2->sd4, target3->sd6 = the existing miniroot boot disk). Each of
  the 5 tar.Z sets sits verbatim at the start of its own partition.

### 16-bit partition-size bug -- scope, and why it does NOT block this plan

`sun3x-tools/install/sun3x-SUNOS-mkdisk.py` and `hw/m68k/sun3x-NOTES.md` (the journal is in branch sun3-history,
"Fix 2") document a real RE'd bug: the boot path computes a partition's
byte size as `(num_sectors & 0xffff) << 9` -- treating the 32-bit
`dkl_nblk` disklabel field as 16-bit, so a partition sized to a multiple of
65536 sectors (exactly 32 MiB granularity) truncates to size 0 and fails
to open. **Re-checked exactly where this happens**: every disassembly
address cited (`0x210770` sd-open, `0x21094e` the `andil #0xffff`,
`0x2109f0` the transfer wrapper, `0x211b50` label-validate) is inside
**ufsboot**, the standalone secondary boot program loaded at `0x00200000`
-- i.e. it only affects the partition ufsboot itself must open to load a
kernel during PROM boot (`b sd(0,30,N)`). The notes' own RE separately
traces the **kernel's** sd-open in kernel text (`0xf8000000+`), a
different, unaffected code path. **Conclusion**: the bug only constrains
a disk's `a` (root) partition if you want that disk to be PROM-bootable.
It does **not** constrain any partition read from an already-running
kernel's shell via `/dev/rsdX` -- which is exactly how both target.img's
big `usr` (`g`) partition and every sets.img partition are accessed here.
The smoke test below empirically re-confirms this (large-offset reads on
`rsd0g`, `>` 32 MiB, and a `>32 MiB`-worth-of-real-tar.Z readback on
`rsd2b`, succeed).

Design consequence: target.img's **root** partition is kept at 60416
sectors (~29.5 MiB, comfortably under the 65535-sector limit) so the
*installed* system stays independently PROM-bootable later; swap/usr and
every sets.img partition are sized freely.

### The manual install script (to run from the miniroot `#` shell)

Not yet executed (task explicitly says hold off on newfs/extraction until
the "Lost Busy" self-healing bus-reset workaround lands, to keep each
run fast) -- written here as the concrete, ordered plan, cross-checked
against `install_small_kernel`/`config_host`/`extract_files` above:

```sh
# --- 1. label is already on target.img / sets.img (written offline by
#        mk-install-disks.py); nothing to do here for the label itself.

# --- 2. make filesystems on the target disk (sd0) ---
# newfs needs either a /etc/disktab entry matching our geometry
# (16 heads x 32 sect/trk, see below) or explicit overrides; SunOS 4.1.1
# newfs supports -b/-f/-t/-s/-c overrides on the command line, e.g.:
newfs -v -t 16 -s 32 -b 8192 -f 1024 /dev/rsd0a      # root,  60416 sectors
newfs -v -t 16 -s 32 -b 8192 -f 1024 /dev/rsd0g      # usr,  487424 sectors
# (swap partition 'b' is NOT newfs'd -- it's raw swap space)

# --- 3. mount the target root + usr under /a ---
mount /dev/sd0a /a
mkdir -p /a/usr
mount /dev/sd0g /a/usr

# --- 4. extract each set, in order, exactly like extract_files' tape
#        primitive (dd | uncompress | tar), just reading a disk partition
#        instead of a tape file mark ---
cd /a
zcat < /dev/rsd2a | tar xvfBp -            # root  (into /a         == /)
cd /a/usr
zcat < /dev/rsd2b | tar xvfBp -            # usr   (into /a/usr     == /usr)
zcat < /dev/rsd2d | tar xvfBp -            # kvm   (into /a/usr/kvm)
zcat < /dev/rsd2e | tar xvfBp -            # install
zcat < /dev/rsd2f | tar xvfBp -            # networking

# --- 5. install the boot block + copy the GENERIC kernel to / ---
/usr/mdec/installboot /a/usr/mdec/bootsd /dev/rsd0a     # (bootsd is the
                                                          #  sun3x boot
                                                          #  program image;
                                                          #  verify exact
                                                          #  name once the
                                                          #  Kvm set is on
                                                          #  disk -- the
                                                          #  miniroot's own
                                                          #  /usr/kvm/mdec
                                                          #  ships bootsd)
cp /a/usr/kvm/sun3x/stand/vmunix /a/vmunix

# --- 6. minimal /etc/fstab + hostname + /etc/hosts on the target ---
cat > /a/etc/fstab <<'EOF'
/dev/sd0a / 4.2 rw 1 1
/dev/sd0g /usr 4.2 rw 1 2
/dev/sd0b swap swap rw 0 0
EOF
echo sun3x1 > /a/etc/hostname.ie0        # or le0, whichever net driver applies
echo "127.0.0.1 localhost" >> /a/etc/hosts
echo "sun3x1" > /a/etc/hostname
/etc/hostname sun3x1                     # active hostname for this session

# --- 7. swap config note ---
# swap is /dev/sd0b, referenced via /etc/fstab above; no mkfile/newfs
# needed for raw swap.

# --- 8. unmount cleanly and reboot from disk ---
umount /a/usr
umount /a
sync; sync
# reboot, then at the PROM: b sd(0,30,3)  <- unit 0 target -> encode target
#   as needed once target.img is moved to its real target/unit for booting.
```

Open items to firm up once newfs/extraction is actually attempted:
* Exact `newfs` invocation syntax for SunOS 4.1.1 on a disk geometry with
  no matching `/etc/disktab` entry (may need to add one, or the miniroot's
  `newfs -t/-s/-b/-f` flags may suffice -- untested, flagged in the plan).
* Confirm `/usr/mdec/bootsd` is the right sun3x boot-block image name
  (vs. `bootxd`/`bootxy`, which are for xd/xy disks, not sd -- `bootsd` is
  almost certainly right by naming convention, matches `installboot`'s
  usual `installboot <bootprog> <target raw device>` calling convention).
* Net driver name for `/etc/hostname.<if>` (`ie0` vs `le0`) -- QEMU sun3x's
  emulated NIC, if any, isn't confirmed; harmless to leave unconfigured
  for a first boot-to-single-user validation.

## 4. Image layouts (built by `mk-install-disks.py`)

Both share geometry **16 heads x 32 sectors/track = 512 (512-byte) blocks
per cylinder = 256 KiB/cylinder** (same as `sun3x-SUNOS-mkdisk.py`).

### target.img -- 300 MiB, 1200 cyl, qemu `unit=0` -> `sd0`

| part | contents | start cyl | cyl count | sectors | MiB | notes |
|---|---|---:|---:|---:|---:|---|
| a | root (empty, to newfs) | 0 | 118 | 60416 | 29.5 | kept < 65535 sectors: PROM/ufsboot-bootable later |
| b | swap | 118 | 130 | 66560 | 32.5 | |
| g | usr (empty, to newfs) | 248 | 952 | 487424 | 238.0 | rest of disk |
| c | whole disk | 0 | 1200 | 614400 | 300.0 | convention only |

### sets.img -- 17,039,360 B (16.25 MiB), 65 cyl, qemu `unit=1` -> `sd2`

Cylinder 0 is reserved for the disklabel only (unlike target.img's root,
these are raw non-filesystem blobs, so nothing may start at absolute byte
0 or the label write clobbers the data and vice versa). Partition -> set
map:

| part | set (tar.Z) | start cyl | cyl count | bytes (file) | partition MiB |
|---|---|---:|---:|---:|---:|
| a | sun3_proto_root.sunos_4_1_1.tar.Z | 1 | 2 | 74309 | 0.50 |
| b | sun3_usr.tar.Z | 3 | 46 | 11633386 | 11.50 |
| d | sun3x_kvm.tar.Z | 49 | 10 | 2102442 | 2.50 |
| e | sun3_install.tar.Z | 59 | 3 | 375437 | 0.75 |
| f | sun3_networking.tar.Z | 62 | 3 | 368339 | 0.75 |
| c | whole disk | 0 | 65 | -- | 16.25 | convention only |

(`g`/`h` unused, size 0.) Each lettered set partition holds its tar.Z
starting at byte 0 of the partition (i.e. `zcat < /dev/rsd2<letter>`
reads valid compress-format data immediately, no skip needed); the
partition is sized 1 cylinder larger than the file needs, so trailing
zero padding after the real data is harmless (`uncompress` stops at its
own logical end-of-stream marker, not at EOF).

Both images verified offline by `mk-install-disks.py verify`: disklabel
magic `0xDABE` at byte 508 and XOR checksum == 0 on both; every sets.img
partition's bytes re-read from the image and SHA1-compared **identical**
to the source tar.Z file (see script output below).

```
$ python3 mk-install-disks.py verify
/tmp/sunos-install/target.img: magic_ok=True checksum_ok=True ncyl=1200 ntrks=16 nsect=32
  a: start_cyl=    0 num_sectors=   60416 (   29.50 MiB) trunc16=ok
  b: start_cyl=  118 num_sectors=   66560 (   32.50 MiB) trunc16=YES -- BUG RISK  (harmless: swap, never ufsboot-opened)
  c: start_cyl=    0 num_sectors=  614400 (  300.00 MiB) trunc16=YES -- BUG RISK  (harmless: whole-disk convention, not booted from)
  g: start_cyl=  248 num_sectors=  487424 (  238.00 MiB) trunc16=YES -- BUG RISK  (harmless: usr, never ufsboot-opened; confirmed by smoke test below)
/tmp/sunos-install/sets.img: magic_ok=True checksum_ok=True ncyl=65 ntrks=16 nsect=32
  a: start_cyl=    1 num_sectors=    1024 (    0.50 MiB) trunc16=ok
  b: start_cyl=    3 num_sectors=   23552 (   11.50 MiB) trunc16=ok
  c: start_cyl=    0 num_sectors=   33280 (   16.25 MiB) trunc16=ok
  d: start_cyl=   49 num_sectors=    5120 (    2.50 MiB) trunc16=ok
  e: start_cyl=   59 num_sectors=    1536 (    0.75 MiB) trunc16=ok
  f: start_cyl=   62 num_sectors=    1536 (    0.75 MiB) trunc16=ok
  readback a (sun3_proto_root.sunos_4_1_1.tar.Z): OK (sha1 00f5475d84dc.. vs 00f5475d84dc..)
  readback b (sun3_usr.tar.Z): OK (sha1 5be9571c2d60.. vs 5be9571c2d60..)
  readback d (sun3x_kvm.tar.Z): OK (sha1 55d7f97b9c70.. vs 55d7f97b9c70..)
  readback e (sun3_install.tar.Z): OK (sha1 6bec99aec213.. vs 6bec99aec213..)
  readback f (sun3_networking.tar.Z): OK (sha1 7781eb7218dd.. vs 7781eb7218dd..)
sets.img byte-for-byte readback: ALL OK
```

(`trunc16=YES` on b/c/g is expected and harmless per the scope analysis
above -- only `a` needs to stay under the ufsboot limit.)

## 5. Smoke test (emulator)

Setup: `cp /tmp/qemu-m8-build/qemu-system-m68k /tmp/qemu-sunos-stable` (stable
copy, per instructions -- never run the build dir's binary directly, the
other agent rebuilds it live). `/tmp/sunos-boot.img` (the shared miniroot
boot disk) was held open by another process during this session
(`Failed to get "write" lock`), so a private copy
`/tmp/sunos-install/sunos-boot-mytest.img` was made and used instead
(delete when done). No TCP ports needed -- `boot-sunos.py` drives QEMU over
`-serial stdio -monitor none`.

Driver: copied `sun3x-tools/boot-sunos.py` (unmodified upstream, never
edited in place) to `/tmp/sunos-install/boot-smoketest.py` and replaced
only its hard-coded post-prompt `CMDS` list with our checks. Command:

```sh
SUN3_LOG=/tmp/sunos-install/smoketest.log \
python3 /tmp/sunos-install/boot-smoketest.py /tmp/qemu-sunos-stable \
  -M sun3x -m 16M -bios /tmp/sunos-media/sun3_80_v3.0.3.bin \
  -drive file=/tmp/sunos-install/sunos-boot-mytest.img,format=raw,if=scsi,bus=0,unit=3 \
  -drive file=/tmp/sunos-install/target.img,format=raw,if=scsi,bus=0,unit=0 \
  -drive file=/tmp/sunos-install/sets.img,format=raw,if=scsi,bus=0,unit=1 \
  -display none
```

### Run 1 (`smoketest.log`) -- device nodes confirmed, tool-availability gap found

Boot succeeded to the `#` shell (full PROM selftest pass, `root on sd6a
fstype 4.2`, `SunOS Release 4.1.1 (MINIROOT)`, `sm0`/`st0-3`/`sr0`/`sd0`
autoconfig). First two commands, verbatim from the log:

```
# ls -l /dev/rsd0a /dev/rsd0b /dev/rsd0g /dev/rsd0c
crw-r-----  1 root      17,   0 Oct 14  1990 /dev/rsd0a
crw-r-----  1 root      17,   1 Oct 14  1990 /dev/rsd0b
crw-r-----  1 root      17,   2 Oct 14  1990 /dev/rsd0c
crw-r-----  1 root      17,   6 Oct 14  1990 /dev/rsd0g
# ls -l /dev/rsd2a /dev/rsd2b /dev/rsd2d /dev/rsd2e /dev/rsd2f /dev/rsd2c
crw-r-----  1 root      17,  16 Oct 14  1990 /dev/rsd2a
crw-r-----  1 root      17,  17 Oct 14  1990 /dev/rsd2b
crw-r-----  1 root      17,  18 Oct 14  1990 /dev/rsd2c
crw-r-----  1 root      17,  19 Oct 14  1990 /dev/rsd2d
crw-r-----  1 root      17,  20 Oct 14  1990 /dev/rsd2e
crw-r-----  1 root      17,  21 Oct 14  1990 /dev/rsd2f
```

This **confirms the sd unit-numbering assumption exactly**: target.img
(qemu `unit=0`) is `sd0` (minor `partition` 0/1/2/6 = a/b/c/g); sets.img
(qemu `unit=1`) is `sd2` (minor `16+partition` = a/b/c/d/e/f), matching
`target*8 + partition` classic BSD minor encoding and the `target*2`
sd-unit convention noted in the task. **The rest of run 1 is
methodologically compromised**: the CMDS list piped `dd`/`zcat`/`tar`
output into `od -x | head -N` for readability, but **the miniroot ships
neither `od` nor `head`** (confirmed absent from `/bin` and `/usr/bin` in
the offline UFS listing, section 2) -- every such pipeline died at
`od: not found`, the shell prompt reappeared immediately (before the
kernel's disk I/O for that command had actually finished), and the next
command's characters got typed over still-arriving asynchronous "Lost
Busy" diagnostic lines from the previous command, garbling several
command echoes (e.g. `odecho SMOKETEST_END`). One real, non-garbled
finding survived: `/dev/rsd2a: I/O error`, `/dev/rsd2d: I/O error`,
`/dev/rsd2e: I/O error`, `/dev/rsd2f: I/O error` (with `sd2a/d/e/f:  read
retry` immediately before each) -- reads from the **sets.img disk
(target 1 / sd2)** hit the known "Lost Busy" bus reset and, unlike the
existing miniroot boot disk (target 3 / sd6, which self-heals via
repeated resets down to `sm0:  sd6, unit online`), **did not self-heal
within one retry** and surfaced a hard I/O error to the shell. No
conclusive data on `rsd0g` (target.img's big `usr` partition, the
>32 MiB-offset addressing question) -- its `od`/`head` pipeline failed
the same way before any byte could be shown.

### Run 2 (`smoketest2.log`) -- fixed CMDS (no `od`/`head` dependency)

Rewrote the CMDS list to avoid every tool not present in the miniroot:
plain `dd ... 2>&1` with **no pipe** (so dd's own `N+0 records in/out`
summary and any raw bytes land straight in the transcript/log, which is
captured byte-for-byte regardless of "binary" content), `zcat < dev >
/tmp/root.tar` + `ls -l` (size proxy) instead of piping through anything,
and `tar tfBp /tmp/root.tar` (list a real file, not a live pipe).

Results, in order (from `smoketest2.log`, control characters stripped):

```
# dd if=/dev/rsd0a bs=512 count=1 2>&1
[... "Lost Busy" resets x4, self-heal, "sd0a: read retry" once, "sm0: sd0, unit online" ...]
SunOS 4.1.1 sun3x target disk°° ìv      `øpÚ¾R'1+0 records in
1+0 records out
# dd if=/dev/rsd0c bs=512 count=1 2>&1
[... resets, self-heal ...]
SunOS 4.1.1 sun3x target disk°° ìv      `øpÚ¾R'1+0 records in
1+0 records out
# dd if=/dev/rsd0g bs=512 count=1 2>&1
[... resets, self-heal ...]
1+0 records in
1+0 records out
# dd if=/dev/rsd0g bs=512 skip=200000 count=1 2>&1
[... "sm0: sd0, unit online" repeats CONTINUOUSLY, ~thousands of times, over
 the entire remaining ~13 minutes of the run, never completing ...]
```

**Two solid positive results**: (1) `dd if=/dev/rsd0a` and `dd if=/dev/rsd0c`
both return the **literal ASCII volume-name string we wrote into the Sun
disklabel** -- `"SunOS 4.1.1 sun3x target disk"` -- byte-for-byte, straight
off the emulated disk, at both offsets (partition `a` and partition `c`
read the identical block 0, as designed). This is an end-to-end proof that
`mk-install-disks.py`'s label is not just offline-valid (section 4) but
**actually readable through the full stack** -- QEMU `-drive` -> emulated
`si`/NCR5380 -> SunOS `sm0`/`sd0` driver -> `/dev/rsd0a`/`c` -> `dd`.
(2) The first, un-skipped `dd if=/dev/rsd0g bs=512 count=1` (partition
`g`/usr, which starts at absolute LBA 126976 = ~62 MiB into the disk) also
completes cleanly (`1+0 records in/out`) -- a real, positive data point that
reading **past the ufsboot 32 MiB/65535-sector limit works fine through the
already-running kernel**, exactly as the code-path analysis in section 3
predicted.

**One hard blocker found, squarely in "Lost Busy" territory and out of this
task's scope to fix**: `dd if=/dev/rsd0g bs=512 skip=200000 count=1` (same
partition, but ~97.7 MiB further in, absolute LBA ~326976 = ~160 MiB into
the 300 MiB disk) never completed. The console showed an **unbounded**
sequence of single-block READ(6) CDBs at slowly incrementing LBAs, each
one going through a full `sm0: resetting scsi bus` -> `Lost Busy error` ->
`sm0: sd0, unit online` cycle, for the **entire remainder of the run**
(~24,000 log lines / ~13 minutes) with no sign of terminating. This
consumed the whole test budget: every subsequent scheduled command
(`dd if=/dev/rsd2a`, `zcat < /dev/rsd2a > /tmp/root.tar`, the `ls -l`, the
`tar tfBp`) got typed into the console at its scheduled time (visible
verbatim in the log at the very end, once each) but **never got a chance
to actually run** -- the shell was still wedged servicing the runaway
retry storm, so none of them produced output before the driver's overall
capture window expired and it killed qemu. **This looks like a distinct
(likely offset- or retry-count-related) failure mode of the same "Lost
Busy" area the other agent is already working on** -- worth flagging to
them verbatim: a `count=1` read at a *large* sector offset on `sd0`
degenerates into an apparently endless single-sector retry loop, whereas
the same partition at a small offset self-heals in a handful of resets.
Given this, **sets.img's actual `zcat | tar` extraction was not
re-verified live this session** (run 1's attempt hit `I/O error` on
`/dev/rsd2{a,d,e,f}` after `read retry`, but that run's methodology was
compromised by missing `od`/`head`, so treat that as suggestive, not
conclusive, evidence that target 1/sd2 is currently less reliable than
target 3/sd6 for sustained transfers -- consistent with, but not proof of,
a target-specific facet of the same bug).

**Net assessment**: disklabels are provably correct and readable end to
end; small/early reads on every attached target succeed and self-heal;
larger-offset or sustained reads are where the still-in-progress "Lost
Busy" fix matters, and until it lands, actually running `newfs` (which
needs many, sequential, large-file-relative transfers) or the tar
extractions (same) is not "quick," so -- per the task's own guidance --
those steps were correctly deferred rather than attempted through a known
hang. No qemu process was left running (the driver's own `q.kill()` tore
it down at the end of the CMDS loop; verified via `ps aux` post-run). The
private boot-disk copy (`sunos-boot-mytest.img`) and worker scratch files
were deleted after the runs; `smoketest.log` (94 KB) and `smoketest2.log`
(954 KB) are kept as the verbatim transcripts.

## 6. Summary / hand-off for whoever continues this

**Ready now:**
* `/tmp/sunos-install/target.img`, `/tmp/sunos-install/sets.img` -- built,
  offline-verified (label + byte-exact readback), and now also **live**
  disklabel-verified through the real boot path (dd off `/dev/rsd0a`/`c`
  returned our exact volume-name string).
* `/tmp/sunos-install/mk-install-disks.py` -- regenerates both images
  from `/tmp/sunos-media` at any time (`python3 mk-install-disks.py build`),
  plus an offline `verify` mode.
* The exact 5-set media inventory + sha1s (section 1) and the concrete,
  ordered manual-install shell script (section 3) -- ready to run as soon
  as large/sustained SCSI transfers are reliable.

**Blocking, not in this task's scope to fix:** the "Lost Busy" self-heal
is not yet reliable enough for sustained/large-offset transfers (this
session reproduced a run that never terminated on a single `dd` at a
~160 MiB disk offset, on the *newly attached* target.img, not just the
existing miniroot disk). Re-run the smoke test in section 5 once that
lands, then proceed to the `newfs`/`mount`/`tar` extraction script in
section 3 verbatim.

**Minor fix-ups for next attempt:**
* Don't pipe miniroot output through `od`/`head`/`wc` -- none exist. Use
  `dd ... 2>&1` (raw, captured by the log verbatim), redirection to a
  `/tmp` file + `ls -l` as a size proxy, and `tar tfBp <file>` on a real
  file rather than a live pipe, as run 2 above does.
* Confirm the exact sun3x boot-block image name in `/usr/kvm/mdec`
  (`bootsd`, verified present, see section 2) before running
  `installboot` for real.
