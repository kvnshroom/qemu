#!/usr/bin/env python3
"""sunos-read-verify.py -- host-side, byte-exact verifier for a guest-driven
READ (disk-to-disk copy) test, built for the sun3x/SunOS 4.1.1 NCR5380 "sm"
cross-target READ-integrity investigation (hw/m68k/sun3x-NOTES.md (full journal: branch sun3-history),
session 26).

Companion to sunos-write-verify.py, but instead of comparing a target
partition against a *host file* the guest DMA'd in, this compares a target
partition against ANOTHER disklabeled image's partition -- the source of a
guest-driven `dd if=/dev/rsd2<letter> of=/dev/rsd0<letter> bs=N` copy (i.e.
the actual shape of the panic-triggering `zcat < /dev/rsd2X | tar xfBp -`
workload: reads off one SCSI target, written to another).

Both images' disklabels are parsed directly (no dependency on any other
tool). The source partition's ENTIRE content is read from the source image
and treated as the "file" being copied, exactly mirroring sunos-write-
verify.py's segment/overlap logic for placing it into the destination
partition according to a list of `bs=N,seek=N[,count=N]` copy specs (one
per `dd` the guest actually ran). Every 8 KiB block of the destination
partition is compared: bytes covered by a copy must match the corresponding
source bytes exactly; every other byte (gaps, and everything beyond the
last copy) must be zero, since target.img.pristine's partitions are built
empty/all-zero.

Usage:
    python3 sunos-read-verify.py <dest-image> <dest-partition> \\
        <source-image> <source-partition> \\
        --copy bs=8k,seek=0 [--copy bs=8k,seek=1472,count=100] ... \\
        [--dump-mismatches N]

Exit status: 0 if every block matches, 1 otherwise.
"""
import argparse
import struct
import sys

BLOCK = 8192
LETTERS = "abcdefgh"


def parse_label(path):
    with open(path, 'rb') as f:
        b = f.read(512)
    magic = struct.unpack_from(">H", b, 508)[0]
    if magic != 0xDABE:
        raise SystemExit("error: %s: bad disklabel magic %#x (expected 0xdabe)"
                          % (path, magic))
    csum = 0
    for i in range(0, 512, 2):
        csum ^= struct.unpack_from(">H", b, i)[0]
    if csum != 0:
        print("warning: %s: disklabel checksum does not cancel to 0 (xor=%#x)"
              % (path, csum), file=sys.stderr)
    ntrks = struct.unpack_from(">H", b, 436)[0]
    nsect = struct.unpack_from(">H", b, 438)[0]
    cyl_bytes = ntrks * nsect * 512
    parts = {}
    for i, letter in enumerate(LETTERS):
        start_cyl, num_sectors = struct.unpack_from(">II", b, 444 + i * 8)
        if num_sectors:
            parts[letter] = (start_cyl * cyl_bytes, num_sectors * 512)
    return parts


def parse_size(s):
    s = s.strip().lower()
    mult = 1
    if s.endswith('k'):
        mult, s = 1024, s[:-1]
    elif s.endswith('m'):
        mult, s = 1024 * 1024, s[:-1]
    return int(s) * mult


def parse_copy_spec(spec):
    """'bs=8k,seek=0[,count=N]' -> (bs_bytes, seek_blocks(dest), count_or_None)"""
    bs = None
    seek = 0
    count = None
    for kv in spec.split(','):
        k, _, v = kv.partition('=')
        k = k.strip()
        if k == 'bs':
            bs = parse_size(v)
        elif k == 'seek':
            seek = int(v)
        elif k == 'count':
            count = int(v)
        else:
            raise SystemExit("error: unknown copy-spec key %r in %r" % (k, spec))
    if bs is None:
        raise SystemExit("error: copy-spec %r missing bs=" % spec)
    return bs, seek, count


def read_partition(image, letter, label_image=None):
    """Read partition `letter`'s bytes from `image`, but parse the disklabel
    itself from `label_image` if given (default: `image`). Needed when a test
    write has landed on the label sector itself (e.g. a raw write to the very
    start of partition 'a', which overlaps the label on a real Sun disk
    layout) -- the partition geometry is unchanged (the kernel parses the
    label once at boot; it does not re-read it per I/O), only the on-disk
    label bytes were clobbered, so geometry must come from an untouched copy."""
    parts = parse_label(label_image or image)
    if letter not in parts:
        raise SystemExit("error: partition %r not found in %s's disklabel (have: %s)"
                          % (letter, label_image or image, sorted(parts)))
    off, length = parts[letter]
    with open(image, 'rb') as f:
        f.seek(off)
        data = f.read(length)
    return off, length, data


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dest_image')
    ap.add_argument('dest_partition')
    ap.add_argument('source_image')
    ap.add_argument('source_partition')
    ap.add_argument('--copy', action='append', default=[], metavar='bs=N,seek=N',
                     help='one dd-equivalent copy (if=<source part> of=<dest '
                          'part>); repeatable, in guest issue order (later '
                          'copies win on overlap, matching real dd semantics)')
    ap.add_argument('--dump-mismatches', type=int, default=40)
    ap.add_argument('--no-full-scan', dest='full_scan', action='store_false',
                     default=True)
    ap.add_argument('--dest-label-image', default=None,
                     help='parse the dest disklabel from this file instead of '
                          'dest_image (use if dest_image\'s own label sector '
                          'was overwritten by an unrelated test write)')
    ap.add_argument('--source-label-image', default=None,
                     help='parse the source disklabel from this file instead '
                          'of source_image')
    args = ap.parse_args()

    dest_off, dest_len, _ = read_partition(args.dest_image, args.dest_partition,
                                            args.dest_label_image)
    print("dest partition %s (%s): byte offset %d, length %d (%.1f MiB)"
          % (args.dest_partition, args.dest_image, dest_off, dest_len,
             dest_len / 1048576.0))

    src_off, src_len, source = read_partition(args.source_image, args.source_partition,
                                               args.source_label_image)
    print("source partition %s (%s): byte offset %d, length %d (%.1f MiB)"
          % (args.source_partition, args.source_image, src_off, src_len,
             src_len / 1048576.0))

    if not args.copy:
        raise SystemExit("error: no --copy given")

    segments = []
    highest_end = 0
    for spec in args.copy:
        bs, seek, count = parse_copy_spec(spec)
        clen = len(source) if count is None else min(len(source), count * bs)
        start = seek * bs
        end = start + clen
        if end > dest_len:
            raise SystemExit("error: copy bs=%d,seek=%d,count=%s would run off "
                              "the end of dest partition %s (end=%d > len=%d)"
                              % (bs, seek, count, args.dest_partition, end, dest_len))
        segments.append((start, end))
        highest_end = max(highest_end, end)
        print("  copy: bs=%d seek=%d count=%s -> dest offset [%d, %d)"
              % (bs, seek, count, start, end))

    scan_end = dest_len if args.full_scan else min(dest_len, highest_end + (4 << 20))

    mismatches = []
    with open(args.dest_image, 'rb') as f:
        nblocks = (scan_end + BLOCK - 1) // BLOCK
        for blk in range(nblocks):
            blk_start = blk * BLOCK
            blk_end = min(blk_start + BLOCK, dest_len)
            blk_len = blk_end - blk_start

            expected = bytearray(BLOCK)
            any_copy = False
            for seg_start, seg_end in segments:
                lo = max(seg_start, blk_start)
                hi = min(seg_end, blk_end)
                if lo < hi:
                    any_copy = True
                    expected[lo - blk_start: hi - blk_start] = \
                        source[lo - seg_start: hi - seg_start]
            expected = bytes(expected[:blk_len])

            f.seek(dest_off + blk_start)
            actual = f.read(blk_len)
            if actual != expected:
                mismatches.append((blk, blk_start, any_copy, actual, expected))

    print()
    if not mismatches:
        print("RESULT: PASS -- all %d blocks (0..%d, %d bytes) of dest partition "
              "%s match the source partition's exact bytes"
              % (nblocks, scan_end, scan_end, args.dest_partition))
        return 0

    print("RESULT: FAIL -- %d of %d blocks mismatch" % (len(mismatches), nblocks))
    for blk, blk_start, any_copy, actual, expected in mismatches[:args.dump_mismatches]:
        lba = (dest_off + blk_start) // 512
        kind = "within a copy's range" if any_copy else "should be ZERO (untouched)"
        print("  block %6d  dest-offset %10d  abs-LBA %8d  (%s)"
              % (blk, blk_start, lba, kind))
        print("    actual[:32]   = %s" % actual[:32].hex())
        print("    expected[:32] = %s" % expected[:32].hex())
    if len(mismatches) > args.dump_mismatches:
        print("  ... (%d more, capped by --dump-mismatches)"
              % (len(mismatches) - args.dump_mismatches))
    return 1


if __name__ == '__main__':
    sys.exit(main())
