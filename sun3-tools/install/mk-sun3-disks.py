#!/usr/bin/env python3
"""
mk-sun3-disks.py -- build the SCSI disk images used to prepare a SunOS 4.1.1
sun3 (non-x, 68020 + Sun-3 MMU) install for the QEMU `sun3-60` machine.

Adapted from two existing tools written for the sun3x (3/80) case:
  * sun3x-tools/install/sun3x-SUNOS-mkdisk.py  -- lays a miniroot at LBA 0 + Sun disklabel
  * sun3x-tools/install/mk-install-disks.py -- target + sets disk builder

Differences from the sun3x versions, all deliberate:
  * Media are the sun3 (non-x) sets: miniroot_sun3 (not _sun3x), and
    sun3_kvm.tar.Z (not sun3x_kvm.tar.Z) -- confirmed by extracting
    stand/vmunix from sun3_kvm.tar.Z: a.out mc68020, a_entry=0x00e04000,
    "SunOS Release 4.1.1 (GENERIC)", not stripped.  sun3_proto_root,
    sun3_usr, sun3_install, sun3_networking are BYTE-IDENTICAL between the
    Sun3 and Sun3x archive directories (same sha1, confirmed by HEAD
    content-length + local sha1 compare) -- only the kernel/kvm sets are
    arch-specific, matching maketape1's tar recipe split.
  * miniroot-sun3.img is sized to the actual miniroot_sun3 (7,168,000 B =
    14000 512B blocks), not a fixed 256 MiB disk like sun3x-SUNOS-mkdisk.py
    -- there is no reason to carry ~249 MiB of unused sparse space here,
    and the task asked for "size the disk to the miniroot".
  * miniroot-sun3.img carries a 16 MiB swap partition 'b' after the
    miniroot (added 2026-09-27: the MINIROOT kernel panics "can't create
    kernel process" without one).
  * target-sun3.img partitions: a=root 31.75 MiB (127 cyl = 65024
    sectors, under the ufsboot 16-bit bound), b=swap 16 MiB, g=usr=rest of
    300 MiB, c=whole.  The first (2026-09-27 media session) build used a
    40 MiB root = 160 cyl x 512 sectors/cyl =
    81920 sectors, i.e. ABOVE the 65535-sector (16-bit) truncation bound
    documented in mk-install-disks.py / hw/m68k/sun3x-NOTES.md ("16-bit
    partition-size bug" in the standalone secondary boot program ufsboot,
    which computes a partition's byte size as `(num_sectors & 0xffff) <<
    9`).  That bug was found and fixed for other subsystems in this
    session's later work but was never specifically re-verified against
    ufsboot's own partition-open path on the `sun3` branch.  Built as
    specified because that is what this task asked for, but flagged here
    and in NOTES.md: if `b sd(0,0,0)` (PROM boot of the *installed*
    system's root, after install) fails to open partition 'a' with a
    zero-size-looking error, shrink 'a' to <= 65535 sectors (<=32 MiB) and
    re-test, exactly as the sun3x install had to.  This constraint does
    NOT apply to sets-sun3.img (only ever read from an already-running
    kernel's /dev/rsdX, never by ufsboot) or to miniroot-sun3.img's root
    partition (14000 sectors, well under the limit).

Sun disklabel layout (Linux kernel block/partitions/sun.c), unchanged from
both source scripts:
  info[128]@0 ; rspeed@420 pcyl@422 sparecyl@424 ilfact@430 ncyl@432
  nacyl@434 ntrks@436 nsect@438 ; partitions[8]@444 (be32 start_cyl, be32
  num_sectors) ; magic@508 (0xDABE) ; csum@510 (xor of all 256 be16 == 0).

No binaries are committed; this only reads files the caller names and
writes image files under /tmp/sunos3-install.
"""
import argparse
import hashlib
import os
import struct
import sys

NTRKS, NSECT = 16, 32                 # geometry shared with sun3x-SUNOS-mkdisk.py
BPC = NTRKS * NSECT                   # 512 x 512-byte blocks per cylinder = 256 KiB/cyl
CYL_BYTES = BPC * 512

LETTERS = "abcdefgh"


# --------------------------------------------------------------------------
# disklabel build/parse (identical logic to mk-install-disks.py)
# --------------------------------------------------------------------------

def build_label(ncyl, parts, volname=b"QEMU sun3 install disk"):
    """parts: dict {letter: (start_cyl, num_cyl)} for any subset of a-h.
    Returns the 512-byte disklabel block."""
    b = bytearray(512)
    label = volname[:31] + b"\x00"
    b[0:len(label)] = label
    struct.pack_into(">H", b, 420, 3600)   # rspeed
    struct.pack_into(">H", b, 422, ncyl)   # pcyl
    struct.pack_into(">H", b, 424, 0)      # sparecyl
    struct.pack_into(">H", b, 430, 1)      # interleave
    struct.pack_into(">H", b, 432, ncyl)   # ncyl
    struct.pack_into(">H", b, 434, 0)      # nacyl
    struct.pack_into(">H", b, 436, NTRKS)  # ntrks
    struct.pack_into(">H", b, 438, NSECT)  # nsect
    for i in range(8):
        struct.pack_into(">II", b, 444 + i * 8, 0, 0)  # default: unused, size 0
    for letter, (start_cyl, num_cyl) in parts.items():
        i = LETTERS.index(letter)
        struct.pack_into(">II", b, 444 + i * 8, start_cyl, num_cyl * BPC)
    struct.pack_into(">H", b, 508, 0xDABE)  # magic
    cs = 0
    for off in range(0, 510, 2):
        cs ^= struct.unpack_from(">H", b, off)[0]
    struct.pack_into(">H", b, 510, cs)
    return bytes(b)


def build_label_sectors(ncyl, parts_sectors, volname=b"QEMU sun3 disk"):
    """Like build_label but partition sizes given directly in sectors
    (used for miniroot-sun3.img, whose 'a' partition is not a whole
    number of cylinders)."""
    b = bytearray(512)
    label = volname[:31] + b"\x00"
    b[0:len(label)] = label
    struct.pack_into(">H", b, 420, 3600)
    struct.pack_into(">H", b, 422, ncyl)
    struct.pack_into(">H", b, 424, 0)
    struct.pack_into(">H", b, 430, 1)
    struct.pack_into(">H", b, 432, ncyl)
    struct.pack_into(">H", b, 434, 0)
    struct.pack_into(">H", b, 436, NTRKS)
    struct.pack_into(">H", b, 438, NSECT)
    for i in range(8):
        struct.pack_into(">II", b, 444 + i * 8, 0, 0)
    for letter, (start_cyl, num_sectors) in parts_sectors.items():
        i = LETTERS.index(letter)
        struct.pack_into(">II", b, 444 + i * 8, start_cyl, num_sectors)
    struct.pack_into(">H", b, 508, 0xDABE)
    cs = 0
    for off in range(0, 510, 2):
        cs ^= struct.unpack_from(">H", b, off)[0]
    struct.pack_into(">H", b, 510, cs)
    return bytes(b)


def parse_label(block):
    """Inverse of build_label; returns dict for offline verification."""
    magic = struct.unpack_from(">H", block, 508)[0]
    cs = 0
    for off in range(0, 512, 2):
        cs ^= struct.unpack_from(">H", block, off)[0]
    ncyl = struct.unpack_from(">H", block, 432)[0]
    ntrks = struct.unpack_from(">H", block, 436)[0]
    nsect = struct.unpack_from(">H", block, 438)[0]
    parts = {}
    for i, letter in enumerate(LETTERS):
        start_cyl, num_sectors = struct.unpack_from(">II", block, 444 + i * 8)
        if num_sectors:
            parts[letter] = (start_cyl, num_sectors)
    return {
        "magic_ok": magic == 0xDABE,
        "checksum_ok": cs == 0,
        "ncyl": ncyl, "ntrks": ntrks, "nsect": nsect,
        "parts": parts,
    }


def verify_label(path):
    with open(path, "rb") as f:
        block = f.read(512)
    info = parse_label(block)
    print(f"{path}: magic_ok={info['magic_ok']} checksum_ok={info['checksum_ok']} "
          f"ncyl={info['ncyl']} ntrks={info['ntrks']} nsect={info['nsect']}")
    for letter, (start_cyl, num_sectors) in sorted(info["parts"].items()):
        print(f"  {letter}: start_cyl={start_cyl:5d} num_sectors={num_sectors:8d} "
              f"({num_sectors * 512 / 1048576:8.2f} MiB) "
              f"trunc16={'YES -- BUG RISK' if (num_sectors & 0xffff) != num_sectors else 'ok'}")
    return info


# --------------------------------------------------------------------------
# (a) miniroot-sun3.img -- miniroot at LBA 0 + disklabel over block 0
# --------------------------------------------------------------------------

def build_miniroot_disk(out, miniroot_path, swap_mib=16):
    """The MINIROOT kernel needs a swap partition: without a 'b' it panics
    "can't create kernel process" right after "dump on" (QEMU sun3-60,
    session 35).  16 MiB (64 cyl = 32768 sectors) is plenty and stays
    under the 65535-sector ufsboot bound."""
    mr = open(miniroot_path, "rb").read()
    assert len(mr) % 512 == 0, "miniroot size not a multiple of 512"
    root_blocks = len(mr) // 512   # 14000 for miniroot_sun3
    root_cyl = (root_blocks + BPC - 1) // BPC   # round up to whole cylinders
    swap_cyl = round(swap_mib * 1048576 / CYL_BYTES)
    total_cyl = root_cyl + swap_cyl
    total_bytes = total_cyl * CYL_BYTES

    label = build_label_sectors(total_cyl, {"a": (0, root_blocks),
                                             "b": (root_cyl, swap_cyl * BPC),
                                             "c": (0, total_cyl * BPC)},
                                 volname=b"QEMU sun3 SunOS 4.1.1 miniroot+swap")

    with open(out, "wb") as f:
        f.truncate(total_bytes)
        f.seek(0)
        f.write(mr)      # miniroot at LBA 0 (bootblk in blocks 1..15, superblock @8192)
        f.seek(0)
        f.write(label)   # disklabel stamped over block 0 (miniroot's own block 0
                          # is a boot-block placeholder, not the UFS superblock,
                          # so overwriting it is exactly what sun3x-SUNOS-mkdisk.py
                          # does and is safe -- confirmed: UFS superblock is at
                          # byte 8192, well past this 512-byte label slot)

    print(f"wrote {out} ({total_bytes >> 20} MiB = {total_cyl} cyl: root {root_cyl} cyl, "
          f"swap {swap_cyl} cyl; miniroot {len(mr)} bytes = {root_blocks} blocks)")
    return total_cyl, root_blocks


def verify_miniroot_disk(out, miniroot_path):
    info = verify_label(out)
    mr = open(miniroot_path, "rb").read()
    with open(out, "rb") as f:
        data = f.read(len(mr))
    # the label write clobbers the first 512 bytes, so compare from byte 512 on,
    # and separately confirm the label's declared 'a' size matches the miniroot.
    ok_body = data[512:] == mr[512:]
    ok_size = info["parts"].get("a", (None, 0))[1] * 512 >= len(mr)
    print(f"  body (byte 512..EOF) matches miniroot: {ok_body}")
    print(f"  partition 'a' size ({info['parts']['a'][1]*512} B) "
          f">= miniroot ({len(mr)} B): {ok_size}")
    return ok_body and ok_size


# --------------------------------------------------------------------------
# (b) target-sun3.img -- empty labelled disk for the install destination
# --------------------------------------------------------------------------

def build_target_disk(out, total_mib=300, root_mib=31.75, swap_mib=16):
    """Root defaults to 127 cyl = 65024 sectors: ufsboot (the secondary boot
    the PROM loads from the installed disk) truncates a partition's sector
    count to 16 bits, so the root it opens must stay under 65536 sectors.
    Swap and usr are only ever opened by the running kernel (32-bit safe);
    the 3/80 install ran with a 228 MiB usr."""
    total_bytes = total_mib << 20
    assert total_bytes % CYL_BYTES == 0, "pick a total_mib that is a whole number of cylinders"
    ncyl = total_bytes // CYL_BYTES

    mib_per_cyl = CYL_BYTES / 1048576   # 0.25 MiB/cyl at this geometry
    root_cyl = round(root_mib / mib_per_cyl)     # 31.75 MiB -> 127 cyl (65024 sectors)
    swap_cyl = round(swap_mib / mib_per_cyl)     # 16 MiB -> 64 cyl
    usr_cyl = ncyl - root_cyl - swap_cyl
    assert usr_cyl > 0

    parts = {
        "a": (0, root_cyl),                       # root
        "b": (root_cyl, swap_cyl),                 # swap
        "g": (root_cyl + swap_cyl, usr_cyl),       # usr (rest)
        "c": (0, ncyl),                            # whole disk
    }
    label = build_label(ncyl, parts, volname=b"SunOS 4.1.1 sun3 target disk")

    with open(out, "wb") as f:
        f.truncate(total_bytes)   # sparse; no data written except the label
        f.seek(0)
        f.write(label)

    print(f"wrote {out} ({total_mib} MiB, {ncyl}c/{NTRKS}h/{NSECT}s)")
    print(f"  a (root) : cyl {0:5d}-{root_cyl:<5d} "
          f"({root_cyl * BPC} sectors = {root_cyl * BPC * 512 / 1048576:.1f} MiB) "
          f"{'*** >65535 sectors, ufsboot 16-bit risk ***' if root_cyl*BPC > 65535 else ''}")
    print(f"  b (swap) : cyl {root_cyl:5d}-{root_cyl + swap_cyl:<5d} "
          f"({swap_cyl * BPC} sectors = {swap_cyl * BPC * 512 / 1048576:.1f} MiB)")
    print(f"  g (usr)  : cyl {root_cyl + swap_cyl:5d}-{ncyl:<5d} "
          f"({usr_cyl * BPC} sectors = {usr_cyl * BPC * 512 / 1048576:.1f} MiB)")
    print(f"  c (whole): cyl {0:5d}-{ncyl:<5d}")
    return parts, ncyl


# --------------------------------------------------------------------------
# (c) sets-sun3.img -- one raw tar.Z per partition
# --------------------------------------------------------------------------

# sun3 (non-x) manifest.  Order matches maketape1 (Sun3 dir)'s tar recipe
# order: munix/munixfs/miniroot go on their own boot tape, then
# root, usr, kvm, install, networking, system_v, sys, ... (extras omitted).
# Only the Kvm set differs by filename from the sun3x manifest
# (sun3_kvm.tar.Z here, not sun3x_kvm.tar.Z) -- root/usr/install/networking
# are the identical shared files (sha1-verified against the Sun3x copies).
SETS_MANIFEST = [
    ("a", "sun3_proto_root.sunos_4_1_1.tar.Z", "root"),
    ("b", "sun3_usr.tar.Z", "usr"),
    ("d", "sun3_kvm.tar.Z", "kvm (sun3, GENERIC kernel under stand/vmunix)"),
    ("e", "sun3_install.tar.Z", "install"),
    ("f", "sun3_networking.tar.Z", "networking"),
]


# Window-system sets (session 38): the same architecture-shared "sun3"
# files the 3/80 used (GFX-LOG.md section 1 sha1s), extracted into /usr of
# the *installed* system (`cd /usr; zcat < /dev/rsd2X | tar xfBp -`).
# Decompressed ~42 MB in total; the 3/60 target's usr has ~186 MB free.
GFX_MANIFEST = [
    ("a", "sun3_sunview_users.tar.Z", "SunView users (/usr/bin/sunview1, suntools)"),
    ("b", "sun3_sunview_programmers.tar.Z", "SunView programmers (/usr/include, libs)"),
    ("d", "sun3_openwindows_users.tar.Z", "OpenWindows 2.0 users (/usr/openwin)"),
    ("e", "sun3_openwindows_fonts.tar.Z", "OpenWindows fonts (/usr/openwin/lib/fonts)"),
    ("f", "sun3_openwindows_programmers.tar.Z", "OpenWindows programmers (libX*.a, include)"),
]


def build_sets_disk(out, media_dir, margin_cyl=1, manifest=SETS_MANIFEST,
                    volname=b"SunOS 4.1.1 sun3 sets disk"):
    files = []
    for letter, fname, label in manifest:
        path = os.path.join(media_dir, fname)
        size = os.path.getsize(path)
        need_cyl = (size + CYL_BYTES - 1) // CYL_BYTES + margin_cyl
        files.append((letter, path, fname, label, size, need_cyl))

    # Reserve cylinder 0 for the disklabel only (these are raw verbatim
    # tar.Z blobs, unlike a UFS partition, so partition 'a' must not start
    # at cylinder 0 or the label write would clobber its first 512 bytes).
    RESERVED_CYL = 1
    ncyl = RESERVED_CYL + sum(f[5] for f in files)
    total_bytes = ncyl * CYL_BYTES

    parts = {}
    cur = RESERVED_CYL
    for letter, path, fname, label, size, need_cyl in files:
        parts[letter] = (cur, need_cyl)
        cur += need_cyl
    parts["c"] = (0, ncyl)   # whole-disk convention

    disklabel = build_label(ncyl, parts, volname=volname)

    with open(out, "wb") as f:
        f.truncate(total_bytes)
        f.seek(0)
        f.write(disklabel)
        for letter, path, fname, label, size, need_cyl in files:
            start_cyl, _ = parts[letter]
            off = start_cyl * CYL_BYTES
            f.seek(off)
            with open(path, "rb") as src:
                f.write(src.read())

    print(f"wrote {out} ({total_bytes / 1048576:.2f} MiB, {ncyl}c/{NTRKS}h/{NSECT}s)")
    print(f"{'part':4s} {'set':32s} {'bytes':>10s} {'cyl range':>14s} {'partition MiB':>14s}")
    for letter, path, fname, label, size, need_cyl in files:
        start_cyl, _ = parts[letter]
        print(f"{letter:4s} {fname:32s} {size:10d} "
              f"{start_cyl:6d}-{start_cyl + need_cyl:<6d} "
              f"{need_cyl * CYL_BYTES / 1048576:14.2f}")
    return parts, ncyl, files


def verify_sets_readback(path, media_dir, manifest=SETS_MANIFEST):
    """Re-read each sets.img partition at its label-declared offset and
    confirm the bytes are byte-identical to the source tar.Z (sha1)."""
    info = verify_label(path)
    ok = True
    with open(path, "rb") as f:
        for letter, fname, label in manifest:
            start_cyl, num_sectors = info["parts"][letter]
            off = start_cyl * CYL_BYTES
            srcpath = os.path.join(media_dir, fname)
            size = os.path.getsize(srcpath)
            f.seek(off)
            data = f.read(size)
            got = hashlib.sha1(data).hexdigest()
            want = hashlib.sha1(open(srcpath, "rb").read()).hexdigest()
            status = "OK" if got == want else "MISMATCH"
            if got != want:
                ok = False
            print(f"  readback {letter} ({fname}): {status} "
                  f"(sha1 {got[:12]}.. vs {want[:12]}..)")
    return ok


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--media-dir", default="/tmp/sunos-media")
    ap.add_argument("--out-dir", default="/tmp/sunos3-install")
    ap.add_argument("--target-mib", type=int, default=300)
    ap.add_argument("--gfx-out", default=None,
                    help="window-system sets disk path (default OUT_DIR/gfx-sets-sun3.img)")
    ap.add_argument("action", choices=["build", "verify", "gfx", "gfx-verify"],
                    nargs="?", default="build")
    args = ap.parse_args()

    if args.action in ("gfx", "gfx-verify"):
        gfx_path = args.gfx_out or os.path.join(args.out_dir, "gfx-sets-sun3.img")
        if args.action == "gfx":
            build_sets_disk(gfx_path, args.media_dir, manifest=GFX_MANIFEST,
                            volname=b"SunOS 4.1.1 sun3 window sets")
            print()
        print("=== offline verification ===")
        ok = verify_sets_readback(gfx_path, args.media_dir, manifest=GFX_MANIFEST)
        print("gfx sets disk byte-for-byte readback:", "ALL OK" if ok else "FAILED")
        sys.exit(0 if ok else 1)

    miniroot_path = os.path.join(args.media_dir, "miniroot_sun3")
    miniroot_out = os.path.join(args.out_dir, "miniroot-sun3.img")
    target_path = os.path.join(args.out_dir, "target-sun3.img")
    sets_path = os.path.join(args.out_dir, "sets-sun3.img")

    if args.action == "build":
        build_miniroot_disk(miniroot_out, miniroot_path)
        print()
        build_target_disk(target_path, total_mib=args.target_mib)
        print()
        build_sets_disk(sets_path, args.media_dir)
        print()

    print("=== offline verification ===")
    ok1 = verify_miniroot_disk(miniroot_out, miniroot_path)
    print()
    verify_label(target_path)
    print()
    ok2 = verify_sets_readback(sets_path, args.media_dir)
    print()
    print("miniroot-sun3.img byte-for-byte + label:", "OK" if ok1 else "FAILED")
    print("sets-sun3.img byte-for-byte readback:", "ALL OK" if ok2 else "FAILED")


if __name__ == "__main__":
    main()
