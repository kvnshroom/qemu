#!/usr/bin/env python3
"""
sunos-symbols.py -- recover kernel symbols for the SunOS 4.1.1/sun3x bring-up.

Two independent jobs, chained together:

  1. Extract a named file (default "vmunix") from a SunOS 4.1.1 miniroot UFS
     image, by actually walking the filesystem (superblock -> root directory
     ino 2 -> directory entries -> target inode -> direct/indirect block
     list), not by guessing byte offsets.  This is the "old-style" (pre-4.4BSD
     dynamic cylinder groups) BSD Fast File System SunOS 4.1.1 shipped: the
     on-disk `struct fs` here does NOT carry `fs_id[2]` (removing it is what
     makes the field offsets line up -- see WHY THE OFFSETS ARE WHAT THEY ARE
     below), and its `daddr_t` fields (fs_sblkno/cblkno/iblkno/dblkno,
     di_db[]/di_ib[]) are plain fs_fsize-fragment numbers, not 512-byte
     sectors.

  2. Parse the extracted file as a 32-byte Sun/BSD a.out header + (if
     a_syms != 0) a classic `struct nlist` symbol table + string table, and
     write a sorted "address name type" symbol file plus offer a PC -> nearest
     symbol helper.  If the extracted binary is stripped (a_syms == 0, as the
     installed MINIROOT /vmunix on this image is), the script also accepts an
     already-downloaded *unstripped* binary of the same driver codebase (e.g.
     the standalone install kernel, munix_sun3x, entry point 0xf8004000 same
     as /vmunix here) via --sym-source, and can byte-match a handful of known
     PCs from one binary's .text into the other's to recover names for them
     even when the two builds' function layouts have drifted apart (their
     text sizes differ -- more drivers configured into MINIROOT's /vmunix --
     so this is a *search*, not a fixed offset).

WHY THE OFFSETS ARE WHAT THEY ARE (empirically derived from this exact image,
/tmp/sunos-media/miniroot_sun3x, cross-checked against `file`'s independent
UFS parse and internal consistency of fs_spc/fs_fpg/fs_ipg/fs_fsmnt):

  Superblock at byte 8192 (SBOFF), struct starts with two 4-byte slots that
  are always zero on disk (fs_link/fs_rlink, in-core-only fields) --
  confirmed becaue fs_size (the field that must show `number of blocks`) only
  lines up with `file`'s reported 7000 blocks if those two slots are counted;
  the field *after* that pair, walking the classic 4.1BSD struct fs in order,
  reproduces every other value `file` reports (bsize 8192, fsize 1024, ncg 4,
  minfree 0, rotdelay 4, rps 60) exactly.  Continuing the same struct layout
  through fs_id[2] (8 bytes) puts fs_fsmnt 8 bytes *later* than where the
  literal ASCII "/miniroot" actually starts -- so this SunOS 4.1.1 struct
  predates fs_id and those 8 bytes simply are not there; removing them lines
  fs_fsmnt up exactly.  fs_sblkno/fs_cblkno/fs_iblkno/fs_dblkno are then
  fragment numbers directly (proved by fs_dblkno - fs_iblkno == fs_ipg /
  fs_inopb * frag, i.e. exactly the inode table's own size in frags -- the
  sector-number reading off by a factor of 2 fails that check).

Usage:
    python3 sunos-symbols.py extract  <miniroot.img> <outfile> [--path /vmunix]
    python3 sunos-symbols.py symbols  <aout-file> <outfile.syms>
    python3 sunos-symbols.py match    <stripped-aout> <symbol-source-aout> \\
                                       --pcs 0xf8073522 0xf8073586 ...

No binaries are committed by this tool; it only reads/writes files the
caller names on the command line.
"""

import argparse
import bisect
import struct
import sys


# --------------------------------------------------------------------------
# 1. UFS (old-style, static-layout, 4.1BSD-derived) reader
# --------------------------------------------------------------------------

SBOFF = 8192          # fixed on-disk superblock location (BBSIZE)
FS_MAGIC = 0x00011954  # BSD FFS magic, at superblock byte offset 1372


class UFSError(Exception):
    pass


class UFS:
    def __init__(self, data):
        self.data = data
        sb = data[SBOFF:SBOFF + 2048]
        magic = struct.unpack('>I', sb[1372:1376])[0]
        if magic != FS_MAGIC:
            raise UFSError(f"no FS_MAGIC at sb+1372 (got {magic:#x})")

        def u32(off):
            return struct.unpack('>I', sb[off:off + 4])[0]

        def s32(off):
            return struct.unpack('>i', sb[off:off + 4])[0]

        # See module docstring for how these offsets were derived: two
        # 4-byte zero in-core-only slots (fs_link/fs_rlink), then the
        # classic 4.1BSD struct fs field order with NO fs_id[2].
        self.sblkno = u32(8)
        self.cblkno = u32(12)
        self.iblkno = u32(16)
        self.dblkno = u32(20)
        self.cgoffset = u32(24)
        self.cgmask = s32(28)
        self.fs_time = u32(32)
        self.size = u32(36)          # total blocks, in frags
        self.dsize = u32(40)
        self.ncg = u32(44)
        self.bsize = u32(48)
        self.fsize = u32(52)
        self.frag = u32(56)          # frags per block
        self.minfree = u32(60)
        self.rotdelay = u32(64)
        self.rps = u32(68)
        self.inopb = u32(120)        # inodes per block
        self.csaddr = u32(152)
        self.cssize = u32(156)
        self.cgsize = u32(160)
        self.ntrak = u32(164)
        self.nsect = u32(168)
        self.spc = u32(172)
        self.ncyl = u32(176)
        self.cpg = u32(180)
        self.ipg = u32(184)          # inodes per cylinder group
        self.fpg = u32(188)          # frags per cylinder group
        fsmnt = sb[212:212 + 32]
        self.fsmnt = fsmnt.split(b'\x00', 1)[0].decode('ascii', 'replace')

        # sanity checks -- fail loudly instead of silently misparsing
        if self.spc != self.ntrak * self.nsect:
            raise UFSError("fs_spc consistency check failed")
        if self.dblkno - self.iblkno != (self.ipg // self.inopb) * self.frag:
            raise UFSError("inode-table size consistency check failed")

    # -- inode -----------------------------------------------------------

    def read_dinode(self, ino):
        """Classic 128-byte on-disk dinode, absolute (fs-wide) inode number."""
        cg = ino // self.ipg
        idx = ino % self.ipg
        cgbase = self.fpg * cg
        # cgoffset/cgmask: old-style superblock-position rotation between
        # cylinder groups; degenerates to 0 for cg 0, which is all this
        # extraction tool actually needs (root dir + /vmunix both live in
        # cg 0 on this image), but computed properly for completeness.
        cgstart = cgbase + self.cgoffset * (cg & ~self.cgmask)
        itab_off = (cgstart + self.iblkno) * self.fsize
        off = itab_off + idx * 128
        rec = self.data[off:off + 128]
        mode, nlink = struct.unpack('>Hh', rec[0:4])
        size = struct.unpack('>Q', rec[8:16])[0]
        db = struct.unpack('>12I', rec[40:40 + 48])
        ib = struct.unpack('>3I', rec[88:100])
        return {'mode': mode, 'nlink': nlink, 'size': size, 'db': db, 'ib': ib}

    def _frag_bytes(self, fragno):
        return fragno * self.fsize

    def read_file(self, ino):
        inode = self.read_dinode(ino)
        size = inode['size']
        out = bytearray()
        bsize = self.bsize
        for fb in inode['db']:
            if len(out) >= size:
                break
            take = min(bsize, size - len(out))
            if fb == 0:
                out += b'\x00' * take
                continue
            start = self._frag_bytes(fb)
            out += self.data[start:start + take]
        if len(out) < size and inode['ib'][0] != 0:
            ib_off = self._frag_bytes(inode['ib'][0])
            nptrs = bsize // 4
            ptrs = struct.unpack('>%dI' % nptrs,
                                  self.data[ib_off:ib_off + bsize])
            for fb in ptrs:
                if len(out) >= size:
                    break
                take = min(bsize, size - len(out))
                if fb == 0:
                    out += b'\x00' * take
                    continue
                start = self._frag_bytes(fb)
                out += self.data[start:start + take]
        if len(out) < size:
            raise UFSError("file uses double-indirect blocks; not implemented "
                            "(not needed for /vmunix on this image)")
        return bytes(out[:size])

    # -- directory ---------------------------------------------------------

    def read_dir(self, ino):
        """Yield (ino, name) for each entry in directory inode `ino`."""
        data = self.read_file(ino)
        i = 0
        while i < len(data) - 8:
            d_ino, d_reclen, d_namlen = struct.unpack('>IHH', data[i:i + 8])
            if d_reclen == 0:
                break
            if d_ino != 0:
                name = data[i + 8:i + 8 + d_namlen]
                yield d_ino, name.decode('ascii', 'replace')
            i += d_reclen

    def lookup_path(self, path):
        """Resolve a simple absolute path (single component supported, which
        is all /vmunix needs) starting from the root inode (2)."""
        parts = [p for p in path.split('/') if p]
        ino = 2
        for part in parts:
            found = None
            for e_ino, name in self.read_dir(ino):
                if name == part:
                    found = e_ino
                    break
            if found is None:
                raise UFSError(f"path component {part!r} not found")
            ino = found
        return ino


def cmd_extract(args):
    data = open(args.image, 'rb').read()
    fs = UFS(data)
    print(f"superblock: fsmnt={fs.fsmnt!r} bsize={fs.bsize} fsize={fs.fsize} "
          f"ncg={fs.ncg} size={fs.size} frags ({fs.size * fs.fsize} bytes)",
          file=sys.stderr)
    ino = fs.lookup_path(args.path)
    print(f"{args.path} -> inode {ino}", file=sys.stderr)
    content = fs.read_file(ino)
    open(args.out, 'wb').write(content)
    print(f"wrote {len(content)} bytes to {args.out}", file=sys.stderr)
    if len(content) >= 32:
        vals = struct.unpack('>8I', content[:32])
        names = ['a_magic', 'a_text', 'a_data', 'a_bss', 'a_syms', 'a_entry',
                  'a_trsize', 'a_drsize']
        for n, v in zip(names, vals):
            print(f"  {n} = {v:#x}", file=sys.stderr)
        if vals[4] == 0:
            print("  a_syms == 0: this binary is STRIPPED; use `symbols` on "
                  "an unstripped binary of the same codebase instead, and "
                  "`match` to correlate addresses.", file=sys.stderr)


# --------------------------------------------------------------------------
# 2. a.out header + nlist symbol table
# --------------------------------------------------------------------------

class AoutError(Exception):
    pass


def parse_aout_header(data):
    if len(data) < 32:
        raise AoutError("file too short for an a.out header")
    vals = struct.unpack('>8I', data[:32])
    names = ['a_magic', 'a_text', 'a_data', 'a_bss', 'a_syms', 'a_entry',
              'a_trsize', 'a_drsize']
    hdr = dict(zip(names, vals))
    magic_lo = hdr['a_magic'] & 0xffff
    if magic_lo not in (0x0107, 0x0108, 0x010b):
        raise AoutError(f"not a recognised a.out magic ({hdr['a_magic']:#x})")
    return hdr


def _find_symtab_and_strtab(data, hdr):
    """
    The naive `32 + a_text + a_data (+ a_trsize + a_drsize)` symtab offset is
    consistently short by a few hundred bytes on Sun's linker output (a
    reserved/padding block whose exact purpose wasn't chased further, only
    empirically confirmed present); the robust way to find the real start is
    to locate the string table first (a long dense run of printable, NUL
    terminated strings -- kernel symbols are long and plentiful, so this is
    unambiguous) and walk backward by a_syms bytes.
    """
    naive = 32 + hdr['a_text'] + hdr['a_data'] + hdr['a_trsize'] + hdr['a_drsize']
    if hdr['a_syms'] == 0:
        return naive, naive, 0

    import re
    pattern = re.compile(rb'[ -~]{6,}\x00')
    region = data[naive:naive + 200000]
    matches = list(pattern.finditer(region))
    if not matches:
        raise AoutError("could not locate string table")
    # Find the first match that begins a run of >=5 consecutive tightly
    # packed strings (small gaps between them) -- that run's *first* match is
    # the real start of the string table; a stray, isolated printable run
    # earlier in the padding is not.
    strtab_rel = None
    for i in range(len(matches) - 4):
        prev_end = matches[i].start()
        ok = True
        for j in range(i, i + 5):
            gap = matches[j].start() - prev_end
            if gap >= 4:
                ok = False
                break
            prev_end = matches[j].end()
        if ok:
            strtab_rel = matches[i].start()
            break
    if strtab_rel is None:
        strtab_rel = matches[0].start()
    strtab_off = naive + strtab_rel - 4  # back up over the 4-byte size prefix
    symtab_off = strtab_off - hdr['a_syms']
    return symtab_off, strtab_off, naive


def parse_symbols(data):
    """Return a sorted list of (address, name) for non-stab, named symbols."""
    hdr = parse_aout_header(data)
    if hdr['a_syms'] == 0:
        return hdr, []
    symtab_off, strtab_off, _ = _find_symtab_and_strtab(data, hdr)
    symtab = data[symtab_off:symtab_off + hdr['a_syms']]
    strsize = struct.unpack('>I', data[strtab_off:strtab_off + 4])[0]
    strtab = data[strtab_off:strtab_off + strsize]

    syms = []
    n = hdr['a_syms'] // 12
    for i in range(n):
        off = i * 12
        n_strx, n_type, n_other, n_desc, n_value = struct.unpack(
            '>IBBHI', symtab[off:off + 12])
        if n_type & 0xe0:      # STAB debug entry, not a real symbol
            continue
        if n_strx == 0:
            continue
        end = strtab.find(b'\x00', n_strx)
        name = strtab[n_strx:end].decode('ascii', 'replace')
        if not name or name.endswith('.o'):
            continue
        syms.append((n_value, name))
    syms.sort()
    return hdr, syms


def cmd_symbols(args):
    data = open(args.aout, 'rb').read()
    hdr, syms = parse_symbols(data)
    print(f"a_entry={hdr['a_entry']:#x} a_text={hdr['a_text']} "
          f"a_syms={hdr['a_syms']}", file=sys.stderr)
    if not syms:
        print("no symbols (stripped binary)", file=sys.stderr)
        return
    with open(args.out, 'w') as f:
        for addr, name in syms:
            f.write(f"{addr:08x} {name}\n")
    print(f"wrote {len(syms)} symbols to {args.out}", file=sys.stderr)


class SymTable:
    """PC -> nearest-preceding-symbol lookup, built from a `symbols`-style
    address-sorted list; usable directly from Python (import this module) or
    via the `symbols`/`match` subcommands."""

    def __init__(self, syms):
        self.syms = syms
        self.addrs = [a for a, _ in syms]

    @classmethod
    def load(cls, path):
        syms = []
        with open(path) as f:
            for line in f:
                addr_s, name = line.split(None, 1)
                syms.append((int(addr_s, 16), name.strip()))
        return cls(syms)

    def nearest(self, pc):
        i = bisect.bisect_right(self.addrs, pc) - 1
        if i < 0:
            return None
        addr, name = self.syms[i]
        return name, pc - addr

    def annotate(self, pc):
        r = self.nearest(pc)
        if r is None:
            return f"{pc:#x}"
        name, delta = r
        return f"{pc:#x} <{name}+{delta:#x}>" if delta else f"{pc:#x} <{name}>"


# --------------------------------------------------------------------------
# 3. cross-binary PC matching (stripped target text -> symbol-source text)
# --------------------------------------------------------------------------

def cmd_match(args):
    target = open(args.stripped, 'rb').read()
    source = open(args.sym_source, 'rb').read()
    thdr = parse_aout_header(target)
    shdr, ssyms = parse_symbols(source)
    if not ssyms:
        print("symbol source has no symbols either", file=sys.stderr)
        sys.exit(1)
    symtab = SymTable(ssyms)

    t_text = target[32:32 + thdr['a_text']]
    s_text = source[32:32 + shdr['a_text']]
    t_entry = thdr['a_entry']
    s_entry = shdr['a_entry']

    for pc in args.pcs:
        pc = int(pc, 0)
        off = pc - t_entry
        if not (0 <= off < len(t_text)):
            print(f"{pc:#x}: outside target .text", file=sys.stderr)
            continue
        found_at = None
        shift_used = 0
        for shift in (0, -8, -4, 4, 8, -16, 16):
            s = off + shift
            if s < 0 or s + 16 > len(t_text):
                continue
            needle = t_text[s:s + 16]
            idx = s_text.find(needle)
            if idx >= 0:
                found_at = idx
                shift_used = shift
                break
        if found_at is None:
            print(f"{pc:#x}: no match found in symbol source", file=sys.stderr)
            continue
        matched_va = found_at + s_entry
        name = symtab.annotate(matched_va)
        note = f" (needle shifted {shift_used:+d}B)" if shift_used else ""
        print(f"{pc:#x} -> {name}{note}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    p = sub.add_parser('extract', help="extract a file from a miniroot UFS image")
    p.add_argument('image')
    p.add_argument('out')
    p.add_argument('--path', default='/vmunix')
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser('symbols', help="dump an a.out's nlist symbol table")
    p.add_argument('aout')
    p.add_argument('out')
    p.set_defaults(func=cmd_symbols)

    p = sub.add_parser('match', help="name PCs in a stripped binary by "
                                      "byte-matching into a symbol source")
    p.add_argument('stripped')
    p.add_argument('sym_source')
    p.add_argument('--pcs', nargs='+', required=True)
    p.set_defaults(func=cmd_match)

    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
