# Test initramfs for the sun3 / sun3x Linux kernels

`initramfs.spec` is the gen_init_cpio manifest referenced by
`CONFIG_INITRAMFS_SOURCE` in `sun3-tools/sun3-linux.config` and
`sun3x-tools/sun3x-linux.config` (as `/tmp/sun3-ir/initramfs.spec`; copy this
directory there or edit the config).  It expects the binaries in `files/`:

* `init` -- `init.c` here, built with the kernel's nolibc: PID 1 mounts
  /proc and /sys, prints a banner and (re)spawns `/bin/smolsh` on
  /dev/console.
* `smolsh`, `uname`, `files` (cat/cp/ln), `ls`, `process` (ps/kill), `dmesg`,
  `df`, `mount` -- from smolutils (`/workspace/src/smolutils`,
  `Makefile.m68kmmu`), built **static, non-PIE, linked at 0x04000000 with
  8 KiB page alignment and without LTO**:

  * non-PIE: the toolchain at hand
    (`/workspace/src/eltec_e17/build/toolchain/bin/m68k-linux-gnu-*`, gcc
    14.3, no libc) fails to assemble smolutils' static-PIE build
    ("Conversion of PC relative displacement to absolute");
  * 0x04000000: the default static link address 0x80000000 is outside the
    Sun-3's 28-bit user address space (the sun3 kernel sits at 0x0E000000,
    so user space ends there); execve fails with ENOMEM;
  * 8 KiB alignment: sun3 uses 8 KiB pages, so ELF segments must be 8 KiB
    congruent (`-z max-page-size=8192 -z common-page-size=8192`);
  * no LTO: the LTO build of smolsh dies with SIGSEGV on sun3-60 (not on
    sun3x) as soon as it does a PATH search -- see the 2026-09-24 entry in
    hw/m68k/sun3-NOTES.md.  Comment out `_COPTS += -flto` in smolutils' common.mk.

Build recipe (paths as used on 2026-09-24):

    export PATH=/workspace/src/eltec_e17/build/toolchain/bin:$PATH
    LINUX=/path/to/linux-sun3            # the linux 'sun3' branch checkout
    make -C $LINUX ARCH=m68k O=/tmp/sun3-khdr-build \
         INSTALL_HDR_PATH=/tmp/sun3-khdr headers_install
    # wrapper adding the link address / page size to every link:
    mkdir -p /tmp/lowcc && cat > /tmp/lowcc/m68k-low-gcc <<'W'
    #!/bin/sh
    exec m68k-linux-gnu-gcc "$@" -Wl,-Ttext-segment=0x04000000 \
         -Wl,-z,max-page-size=8192 -Wl,-z,common-page-size=8192
    W
    for t in strip ld.bfd; do printf '#!/bin/sh\nexec m68k-linux-gnu-%s "$@"\n' $t > /tmp/lowcc/m68k-low-$t; done
    chmod +x /tmp/lowcc/*
    cp -r /workspace/src/smolutils /tmp/smolutils-build && cd /tmp/smolutils-build
    sed -i 's/^_COPTS += -flto$/#&/' common.mk
    make -f Makefile.m68kmmu CPU=68040 NOPIE=1 FEATURE_DISABLE=net \
         CROSS_COMPILE=/tmp/lowcc/m68k-low- TARWAK=/bin/true \
         NOLIBCDIR=$LINUX/tools/include/nolibc \
         NOLIBCEXTDIR=/workspace/src/nolibc-extensions \
         UAPIDIR=/tmp/sun3-khdr/include \
         smolsh.68040.elf uname.68040.elf files.68040.elf ls.68040.elf \
         process.68040.elf dmesg.68040.elf df.68040.elf mount.68040.elf
    mkdir -p /tmp/sun3-ir/files
    for t in smolsh uname files ls process dmesg df mount; do cp $t.68040.elf /tmp/sun3-ir/files/$t; done
    m68k-linux-gnu-gcc -m68040 -Os -static -nostdlib -nostdinc -fno-stack-protector \
         -fno-asynchronous-unwind-tables -fno-ident \
         -isystem $LINUX/tools/include/nolibc -isystem /tmp/sun3-khdr/include \
         -include nolibc.h -Wl,-Ttext-segment=0x04000000 \
         -Wl,-z,max-page-size=8192 -Wl,-z,common-page-size=8192 \
         -o /tmp/sun3-ir/files/init init.c -lgcc
    cp initramfs.spec /tmp/sun3-ir/   # paths in it are relative: files/...
    sed -i 's|file \(\S*\) files/|file \1 /tmp/sun3-ir/files/|' /tmp/sun3-ir/initramfs.spec

(CPU=68040 only picks the integer/FPU instruction subset; it runs on the
68020+68881 of the 3/60 and the 68030 of the 3/80.  smolutils' Makefile
refuses CPU=68020.)

The same initramfs is embedded in both the sun3 and the sun3x kernel.
