#!/usr/bin/env python3
"""
mk-bootdisk.py -- make a disk the Sun-3 PROM boots Linux/sun3 from.

    mk-bootdisk.py [--bootblk bootblk.bin] [--initrd FILE] [--append "..."]
                   [--out disk.img] vmlinux

Layout of the image (512-byte sectors, Sun label geometry 16 x 32):

  sector 0        Sun disklabel (partitions a and c = the whole disk),
                  built with sun3-tools/install/mk-sun3-disks.py's code
  sectors 1..16   the boot block (bootblk.bin, built from boot.c/entry.S;
                  the PROM's b_boot reads 8 KiB from block 1 to 0x4000)
  sector 17       the image header (below)
  sector 32..     the segments: the kernel image, then the initrd

Image header (sector 17, big-endian, mirrors struct bootimg_hdr in boot.c):

  char magic[8]  "SUN3LNX1"
  u32  entry     physical address to jump to (ELF e_entry - KERNBASE)
  u32  nseg
  then nseg x { u32 lba (partition-relative sector), u32 size (bytes),
                u32 dest (physical address), u32 flags = 0 }

The kernel segment is what sun3-tools/boot-linux.py injects: every
PT_LOAD at p_vaddr - KERNBASE (0x0E000000), the BSS tail zeroed, and a
bootinfo block at _end (BI_MACHTYPE MACH_SUN3, CPU/FPU/MMU types,
optional BI_COMMAND_LINE, optional BI_RAMDISK, BI_LAST).  The optional
initrd is placed two pages above the end of that segment; everything must
lie below 0x780000, where the boot block runs (the PROM identity-maps only
the low 8 MB).  The vmlinux itself is not copied: the ELF is flattened.

No binaries are committed: build the boot block with `make` in this
directory (m68k-linux-gnu- toolchain, CROSS=... to override).
"""
import argparse
import os
import struct
import sys

KERNBASE = 0x0E000000
PAGE = 8192
SECTOR = 512
NTRKS, NSECT = 16, 32
CYL_SECTORS = NTRKS * NSECT
BOOT_SECTORS = 16            # sectors 1..16
HDR_SECTOR = 17
DATA_SECTOR = 32
RELOC = 0x780000
MAX_SEGS = 30

# bootinfo tags/values (include/uapi/asm/bootinfo.h, bootinfo-*.h)
BI_LAST, BI_MACHTYPE, BI_CPUTYPE, BI_FPUTYPE, BI_MMUTYPE = 0, 1, 2, 3, 4
BI_RAMDISK, BI_COMMAND_LINE = 6, 7
MACH_SUN3 = 5
CPU_68020, FPU_68881, MMU_SUN3 = 1 << 0, 1 << 0, 1 << 4


def load_label_builder():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "install", "mk-sun3-disks.py")
    from importlib.machinery import SourceFileLoader
    return SourceFileLoader("mk_sun3_disks", path).load_module()


def flatten_elf(path):
    """-> (entry_pa, start_pa, bytes) with BSS zeroed, exactly like
    boot-linux.py; the blob covers [start_pa, end_pa)."""
    elf = open(path, "rb").read()
    if elf[:4] != b"\x7fELF" or elf[4] != 1 or elf[5] != 2:
        sys.exit(f"{path}: not a big-endian ELF32")
    e_entry, e_phoff = struct.unpack_from(">II", elf, 24)
    e_phentsize, e_phnum = struct.unpack_from(">HH", elf, 42)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz = \
            struct.unpack_from(">IIIIII", elf, e_phoff + i * e_phentsize)
        if p_type == 1 and p_memsz:
            segs.append((p_vaddr - KERNBASE, elf[p_offset:p_offset + p_filesz],
                         p_memsz))
    if not segs:
        sys.exit(f"{path}: no PT_LOAD")
    start = min(pa for pa, _, _ in segs)
    end = max(pa + memsz for pa, _, memsz in segs)
    blob = bytearray(end - start)
    for pa, data, memsz in segs:
        blob[pa - start:pa - start + len(data)] = data
    return e_entry - KERNBASE, start, bytes(blob)


def bi_record(tag, payload=b""):
    size = (4 + len(payload) + 3) & ~3
    return struct.pack(">HH", tag, size) + payload.ljust(size - 4, b"\0")


def build_bootinfo(cmdline, initrd):
    bi = bi_record(BI_MACHTYPE, struct.pack(">I", MACH_SUN3))
    bi += bi_record(BI_CPUTYPE, struct.pack(">I", CPU_68020))
    bi += bi_record(BI_FPUTYPE, struct.pack(">I", FPU_68881))
    bi += bi_record(BI_MMUTYPE, struct.pack(">I", MMU_SUN3))
    if cmdline:
        bi += bi_record(BI_COMMAND_LINE, cmdline.encode() + b"\0")
    if initrd:
        bi += bi_record(BI_RAMDISK, struct.pack(">II", *initrd))
    bi += bi_record(BI_LAST)
    return bi


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("vmlinux")
    ap.add_argument("--bootblk", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "bootblk.bin"))
    ap.add_argument("--initrd")
    ap.add_argument("--append", default="", help="kernel command line")
    ap.add_argument("--out", default="sun3-linux-boot.img")
    args = ap.parse_args()

    bootblk = open(args.bootblk, "rb").read()
    if len(bootblk) > BOOT_SECTORS * SECTOR:
        sys.exit(f"{args.bootblk}: {len(bootblk)} bytes, the PROM loads 8192")

    entry, start, kernel = flatten_elf(args.vmlinux)
    end = start + len(kernel)
    end_aligned = (end + 3) & ~3
    kernel += b"\0" * (end_aligned - end)
    initrd_data = initrd_loc = None
    if args.initrd:
        initrd_data = open(args.initrd, "rb").read()
        initrd_pa = ((end_aligned + 64 + PAGE - 1) & ~(PAGE - 1)) + 2 * PAGE
        initrd_loc = (initrd_pa, len(initrd_data))
    kernel += build_bootinfo(args.append, initrd_loc)

    segs = [(DATA_SECTOR, start, kernel)]
    if initrd_data:
        lba = DATA_SECTOR + (len(kernel) + SECTOR - 1) // SECTOR
        segs.append((lba, initrd_loc[0], initrd_data))
    for lba, dest, data in segs:
        if dest + len(data) > RELOC:
            sys.exit(f"segment at {dest:#x}+{len(data):#x} reaches the boot "
                     f"block at {RELOC:#x}: the image must fit below it")

    hdr = struct.pack(">8sII", b"SUN3LNX1", entry, len(segs))
    for lba, dest, data in segs:
        hdr += struct.pack(">IIII", lba, len(data), dest, 0)
    assert len(hdr) <= SECTOR

    last = segs[-1]
    total_sectors = last[0] + (len(last[2]) + SECTOR - 1) // SECTOR
    ncyl = (total_sectors + CYL_SECTORS - 1) // CYL_SECTORS
    total_sectors = ncyl * CYL_SECTORS

    mk = load_label_builder()
    label = mk.build_label_sectors(ncyl, {"a": (0, total_sectors),
                                          "c": (0, total_sectors)},
                                   volname=b"QEMU sun3 Linux boot disk")

    with open(args.out, "wb") as f:
        f.write(label)
        f.write(bootblk)
        f.seek(HDR_SECTOR * SECTOR)
        f.write(hdr)
        for lba, dest, data in segs:
            f.seek(lba * SECTOR)
            f.write(data)
        f.truncate(total_sectors * SECTOR)

    print(f"{args.out}: {ncyl} cyl ({total_sectors * SECTOR / 1e6:.1f} MB), "
          f"boot block {len(bootblk)} bytes, entry {entry:#x}")
    for lba, dest, data in segs:
        print(f"  sector {lba:6d}: {len(data):8d} bytes -> {dest:#x}")


if __name__ == "__main__":
    main()
