#!/usr/bin/env python3
"""
mk-install-disks.py -- build the two auxiliary SCSI disk images used for a
manual (miniroot-shell) SunOS 4.1.1 sun3x install under QEMU -M sun3x:

  target.img  -- the disk SunOS gets installed ONTO.  Sun disklabel with
                 partitions a (root), b (swap), g (usr), c (whole disk).
                 Left EMPTY (no filesystem) -- newfs is a later, separate
                 step done from the miniroot shell once attached.

  sets.img    -- a "distribution" disk: each required tar.Z set is laid
                 down VERBATIM (byte for byte, still .Z-compressed) at the
                 start of its own disklabel partition, so the miniroot
                 shell can do:
                     zcat < /dev/rsd2<letter> | tar xvfBp -
                 (rsd2 because this disk is attached at qemu unit=1, and
                 SunOS sd unit numbering on this machine is target*2 --
                 target1 -> sd2; see NOTES.md "sd unit numbering").

Sun disklabel format (from sun3x-tools/install/sun3x-SUNOS-mkdisk.py, Linux kernel
block/partitions/sun.c):
  info[128]@0 ; rspeed@420 pcyl@422 sparecyl@424 ilfact@430 ncyl@432
  nacyl@434 ntrks@436 nsect@438 ; partitions[8]@444 (be32 start_CYL,
  be32 num_SECTORS) ; magic@508 (0xDABE) ; csum@510 (xor of all 256 be16
  words, including magic, == 0).

CRITICAL SIZE CONSTRAINT (see NOTES.md "16-bit partition-size bug"):
  the SunOS *standalone secondary boot program* ufsboot (loaded at
  0x00200000, distinct from the real kernel at 0xf8000000+) computes a
  partition's byte size as `(num_sectors & 0xffff) << 9` -- it treats
  num_sectors as 16-bit.  This ONLY matters for a partition ufsboot itself
  must open, i.e. the ROOT partition of a disk you intend to boot with
  `b sd(0,30,N)`.  It does NOT affect partitions read by an
  already-running kernel via /dev/rsdX in a shell (confirmed by RE: the
  bug's disassembly addresses 0x210xxx all fall inside ufsboot's own
  0x200000-0x220000 text, not the kernel's 0xf8000000+ text).  We still
  keep target.img's root partition <= 65535 sectors (~32 MiB) so the
  installed system stays independently PROM-bootable later; swap/usr and
  every sets.img partition are unconstrained.

No binaries are committed; this only reads files the caller names and
writes image files under /tmp/sunos-install.
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


def build_label(ncyl, parts, volname=b"QEMU sun3x install disk"):
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


# --------------------------------------------------------------------------
# target.img -- empty labelled disk for the install destination
# --------------------------------------------------------------------------

def build_target_disk(out, total_mib=300):
    total_bytes = total_mib << 20
    assert total_bytes % CYL_BYTES == 0, "pick a total_mib that is a whole number of cylinders"
    ncyl = total_bytes // CYL_BYTES

    # root ~29.5 MiB: 118 cyl, well under the 65535-sector (~32 MiB) ufsboot
    # limit so this disk stays independently PROM-bootable after install.
    root_cyl = 118
    # swap ~32.5 MiB: 130 cyl.  (Not ufsboot-constrained -- swap is never
    # opened by the boot loader -- but kept modest per the task's ~32 MB ask.)
    swap_cyl = 130
    usr_cyl = ncyl - root_cyl - swap_cyl
    assert usr_cyl > 0

    parts = {
        "a": (0, root_cyl),                                   # root
        "b": (root_cyl, swap_cyl),                             # swap
        "g": (root_cyl + swap_cyl, usr_cyl),                   # usr (rest)
        "c": (0, ncyl),                                        # whole disk
    }
    label = build_label(ncyl, parts, volname=b"SunOS 4.1.1 sun3x target disk")

    with open(out, "wb") as f:
        f.truncate(total_bytes)   # sparse; no data written except the label
        f.seek(0)
        f.write(label)

    print(f"wrote {out} ({total_mib} MiB, {ncyl}c/{NTRKS}h/{NSECT}s)")
    print(f"  a (root) : cyl {0:5d}-{root_cyl:<5d} "
          f"({root_cyl * BPC} sectors = {root_cyl * BPC * 512 / 1048576:.1f} MiB)")
    print(f"  b (swap) : cyl {root_cyl:5d}-{root_cyl + swap_cyl:<5d} "
          f"({swap_cyl * BPC} sectors = {swap_cyl * BPC * 512 / 1048576:.1f} MiB)")
    print(f"  g (usr)  : cyl {root_cyl + swap_cyl:5d}-{ncyl:<5d} "
          f"({usr_cyl * BPC} sectors = {usr_cyl * BPC * 512 / 1048576:.1f} MiB)")
    print(f"  c (whole): cyl {0:5d}-{ncyl:<5d}")
    return parts, ncyl


# --------------------------------------------------------------------------
# sets.img -- one raw tar.Z per partition, at fixed cylinder offsets
# --------------------------------------------------------------------------

# (letter, source file, human label).  Order matches the real Sun3x
# QIC-150 tape's category order (see maketape1 / SunOS411.sun3x.QIC-150.xdrtoc):
# root, usr, Kvm, Install, Networking -- everything a minimal multi-user
# sun3x system needs.  'sys' (kernel *source*) is intentionally omitted:
# install.bin (used by add_client) shows the Kvm set already ships a
# prebuilt GENERIC kernel at usr/kvm/sun3x/stand/vmunix, so no separate
# 'sys' set is required just to boot multi-user.
SETS_MANIFEST = [
    ("a", "sun3_proto_root.sunos_4_1_1.tar.Z", "root"),
    ("b", "sun3_usr.tar.Z", "usr"),
    ("d", "sun3x_kvm.tar.Z", "kvm (sun3x)"),
    ("e", "sun3_install.tar.Z", "install"),
    ("f", "sun3_networking.tar.Z", "networking"),
]


def build_sets_disk(out, media_dir, margin_cyl=1):
    files = []
    for letter, fname, label in SETS_MANIFEST:
        path = os.path.join(media_dir, fname)
        size = os.path.getsize(path)
        need_cyl = (size + CYL_BYTES - 1) // CYL_BYTES + margin_cyl
        files.append((letter, path, fname, label, size, need_cyl))

    # Cylinder 0 (its first 512 bytes) is the disklabel slot.  Unlike
    # target.img's root partition -- a UFS filesystem, which deliberately
    # leaves block 0 alone (superblock is at byte 8192) -- these are raw
    # verbatim tar.Z blobs, so if partition 'a' started at cylinder 0 the
    # label write would clobber the first 512 bytes of the tar.Z data (and
    # vice versa). Reserve cylinder 0 for the label only; all set data
    # partitions start at cylinder 1.
    RESERVED_CYL = 1
    ncyl = RESERVED_CYL + sum(f[5] for f in files)
    total_bytes = ncyl * CYL_BYTES

    parts = {}
    cur = RESERVED_CYL
    for letter, path, fname, label, size, need_cyl in files:
        parts[letter] = (cur, need_cyl)
        cur += need_cyl
    parts["c"] = (0, ncyl)   # whole-disk convention

    disklabel = build_label(ncyl, parts, volname=b"SunOS 4.1.1 sun3x sets disk")

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
    print(f"{'part':4s} {'set':28s} {'bytes':>10s} {'cyl range':>14s} {'partition MiB':>14s}")
    for letter, path, fname, label, size, need_cyl in files:
        start_cyl, _ = parts[letter]
        print(f"{letter:4s} {fname:28s} {size:10d} "
              f"{start_cyl:6d}-{start_cyl + need_cyl:<6d} "
              f"{need_cyl * CYL_BYTES / 1048576:14.2f}")
    return parts, ncyl, files


# --------------------------------------------------------------------------
# offline verification
# --------------------------------------------------------------------------

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


def verify_sets_readback(path, media_dir):
    """Re-read each sets.img partition at its label-declared offset and
    confirm the bytes are byte-identical to the source tar.Z (sha1)."""
    info = verify_label(path)
    ok = True
    with open(path, "rb") as f:
        for letter, fname, label in SETS_MANIFEST:
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--media-dir", default="/tmp/sunos-media")
    ap.add_argument("--out-dir", default="/tmp/sunos-install")
    ap.add_argument("--target-mib", type=int, default=300)
    ap.add_argument("action", choices=["build", "verify"], nargs="?", default="build")
    args = ap.parse_args()

    target_path = os.path.join(args.out_dir, "target.img")
    sets_path = os.path.join(args.out_dir, "sets.img")

    if args.action == "build":
        build_target_disk(target_path, total_mib=args.target_mib)
        print()
        build_sets_disk(sets_path, args.media_dir)
        print()

    print("=== offline verification ===")
    verify_label(target_path)
    ok = verify_sets_readback(sets_path, args.media_dir)
    print("sets.img byte-for-byte readback:", "ALL OK" if ok else "FAILED")


if __name__ == "__main__":
    main()
