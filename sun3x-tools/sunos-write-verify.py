#!/usr/bin/env python3
"""sunos-write-verify.py -- host-side, byte-exact verifier for a guest-driven
write test onto a raw Sun disklabel image (built for the sun3x/SunOS 4.1.1
NCR5380 "sm" write-integrity investigation, hw/m68k/sun3x-NOTES.md (full journal: branch sun3-history)).

The test this supports: boot a SunOS MINIROOT against a target disk image
and run a sequence of `dd if=<source> of=/dev/rsd0<partition> bs=<bs>
seek=<n>` commands (no `count=`, so each dd copies the WHOLE source file
starting at partition-relative byte offset `n * bs`). This tool reads the
target disklabel directly (no dependency on any other script), computes
each write's expected placement, and compares every 8 KiB block of the
named partition against the expected content: bytes covered by a write must
match the source file exactly; every other byte (gaps between writes, and
everything beyond the last write) must be zero, since target.img.pristine's
partitions are built empty/all-zero.

Usage:
    python3 sunos-write-verify.py <target.img> <source-file> \\
        --partition g --write bs=8k,seek=0 --write bs=8k,seek=160 ... \\
        [--dump-mismatches N]

Exit status: 0 if every block matches (including the "beyond written range
must be zero" check), 1 otherwise. Prints one line per mismatching 8 KiB
block (capped by --dump-mismatches, default 40) plus a summary.

No binaries are written; this only reads the image and source file named on
the command line.
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


def parse_write_spec(spec):
    """'bs=8k,seek=160[,count=85]' -> (bs_bytes, seek_blocks, count_or_None)

    count mirrors dd's own count=: caps the write at count*bs bytes (used to
    avoid a trailing sub-sector partial write when the source file's length
    is not a multiple of bs -- real raw/character disk devices reject a
    write() that isn't a multiple of the sector size, so any test that hits
    that is a userland dd limitation, not something this tool should treat
    as a placement bug)."""
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
            raise SystemExit("error: unknown write-spec key %r in %r" % (k, spec))
    if bs is None:
        raise SystemExit("error: write-spec %r missing bs=" % spec)
    return bs, seek, count


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image')
    ap.add_argument('source')
    ap.add_argument('--partition', default='g')
    ap.add_argument('--write', action='append', default=[], metavar='bs=N,seek=N',
                     help='one dd-equivalent write; repeatable, in the order the '
                          'guest issued them (later writes in an overlapping '
                          'region win, matching real dd semantics)')
    ap.add_argument('--dump-mismatches', type=int, default=40)
    ap.add_argument('--full-scan', action='store_true', default=True,
                     help='verify the ENTIRE partition, not just up to the last '
                          'write plus a margin (default: on)')
    ap.add_argument('--no-full-scan', dest='full_scan', action='store_false')
    args = ap.parse_args()

    parts = parse_label(args.image)
    if args.partition not in parts:
        raise SystemExit("error: partition %r not found in %s's disklabel (have: %s)"
                          % (args.partition, args.image, sorted(parts)))
    part_off, part_len = parts[args.partition]
    print("partition %s: byte offset %d, length %d (%.1f MiB)"
          % (args.partition, part_off, part_len, part_len / 1048576.0))

    with open(args.source, 'rb') as f:
        source = f.read()
    print("source %s: %d bytes" % (args.source, len(source)))

    if not args.write:
        raise SystemExit("error: no --write given")

    # Build the expected-content map as a list of (start, end, source) segments,
    # later writes overriding earlier ones on overlap (real dd semantics).
    segments = []
    highest_end = 0
    for spec in args.write:
        bs, seek, count = parse_write_spec(spec)
        wlen = len(source) if count is None else min(len(source), count * bs)
        start = seek * bs
        end = start + wlen
        if end > part_len:
            raise SystemExit("error: write bs=%d,seek=%d,count=%s would run off the "
                              "end of partition %s (end=%d > part_len=%d)"
                              % (bs, seek, count, args.partition, end, part_len))
        segments.append((start, end))
        highest_end = max(highest_end, end)
        print("  write: bs=%d seek=%d count=%s -> partition offset [%d, %d)"
              % (bs, seek, count, start, end))

    scan_end = part_len if args.full_scan else min(part_len, highest_end + (4 << 20))

    # Compare block by block (8 KiB), building each block's expected content
    # on the fly from the segment list (segments listed in guest issue order;
    # later ones win on overlap).
    mismatches = []
    zero_block = b'\x00' * BLOCK
    with open(args.image, 'rb') as f:
        nblocks = (scan_end + BLOCK - 1) // BLOCK
        for blk in range(nblocks):
            blk_start = blk * BLOCK
            blk_end = min(blk_start + BLOCK, part_len)
            blk_len = blk_end - blk_start

            expected = bytearray(BLOCK)
            any_write = False
            for seg_start, seg_end in segments:
                lo = max(seg_start, blk_start)
                hi = min(seg_end, blk_end)
                if lo < hi:
                    any_write = True
                    expected[lo - blk_start: hi - blk_start] = \
                        source[lo - seg_start: hi - seg_start]
            expected = bytes(expected[:blk_len])

            f.seek(part_off + blk_start)
            actual = f.read(blk_len)
            if actual != expected:
                mismatches.append((blk, blk_start, any_write, actual, expected))

    print()
    if not mismatches:
        print("RESULT: PASS -- all %d blocks (0..%d, %d bytes) of partition %s match "
              "expected content exactly" % (nblocks, scan_end, scan_end, args.partition))
        return 0

    print("RESULT: FAIL -- %d of %d blocks mismatch" % (len(mismatches), nblocks))
    for blk, blk_start, any_write, actual, expected in mismatches[:args.dump_mismatches]:
        lba = (part_off + blk_start) // 512
        kind = "within a write's range" if any_write else "should be ZERO (untouched)"
        print("  block %6d  partition-offset %10d  abs-LBA %8d  (%s)"
              % (blk, blk_start, lba, kind))
        print("    actual[:32]   = %s" % actual[:32].hex())
        print("    expected[:32] = %s" % expected[:32].hex())
    if len(mismatches) > args.dump_mismatches:
        print("  ... (%d more, capped by --dump-mismatches)"
              % (len(mismatches) - args.dump_mismatches))
    return 1


if __name__ == '__main__':
    sys.exit(main())
