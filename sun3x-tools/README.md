# sun3x-tools

Artifacts for building/booting the Linux `sun3x` (CONFIG_SUN3X, Sun 3/80)
kernel on the QEMU `sun3x` machine.

## Contents
* `zs-console.patch` — the **native Z8530 (zs) serial console** for sun3x
  (`drivers/tty/serial/sun3x_scc.c` + Kconfig/Makefile).  It drives channel A
  of the console zs at `0x62000000` directly and registers an interactive
  `console=ttyS0` with a real `uart_driver`/`uart_port`, so the runtime
  console runs on the native hardware — like mvme147/mvme16x/bvme6000, whose
  Z8530 console it is adapted from.  RX is polled by a 1-jiffy timer and TX
  polls `RR0.TX_EMPTY` (the zs IRQ is not wired to the CPU on this model);
  the zs registers are reached at their physical addresses because head.S
  transparently (TT1) maps the on-board I/O.  This **replaces** the old
  `hvc_sun3` PROM-tunnelled console, which is retired for sun3x (kept in the
  tree for the real SUN3).  The synthesised boot PROM is still required for
  head.S (earliest `putc` + page-table root) and is unchanged.
* `linux-sun3x.patch` — the earlier hvc_sun3 shim (interactive `hvc0` over
  the PROM).  Kept for reference / as a fallback; **not** needed for the
  native console.  For sun3x it is superseded by `zs-console.patch`.
* `sun3x-linux.config` — the kernel config used (CONFIG_SUN3X=y, MMU_MOTOROLA,
  M68030, M68KFPU_EMU=y, built-in initramfs).  Console-relevant bits:
  `CONFIG_SERIAL_SUN3X_SCC=y`, `CONFIG_SERIAL_CORE=y`,
  `CONFIG_SERIAL_CORE_CONSOLE=y`, `CONFIG_HVC_SUN3` **off**, and
  `CONFIG_BOOTPARAM_STRING="console=ttyS0"` (m68k pins the command line via
  BOOTPARAM, so `-append` is ignored — set the console here).
* The initramfs manifest now lives in `sun3-tools/initramfs/` (shared with
  the sun3 kernel): a static smolutils userland + a tiny nolibc /init that
  spawns `smolsh` on `/dev/console` (ttyS0 here).  See the README there.
* Early console: the kernel `sun3` branch adds `sun3x_scc_write()` (direct
  Z8530 register writes, hooked into earlyprintk like mvme147); the config
  sets `CONFIG_EARLY_PRINTK=y` and `console=ttyS0 earlyprintk`, so the log
  starts with `bootconsole [debug0] enabled` before ttyS0 takes over.

## Build

    cd <linux tree>            # e.g. /workspace/src/linux-drmbounce
    # the linux 'sun3' branch already contains the zs console + early console
    # (zs-console.patch is kept for reference only)
    export ARCH=m68k CROSS_COMPILE=m68k-linux-gnu-
    cp sun3x-tools/sun3x-linux.config /tmp/sun3x-kbuild/.config
    make O=/tmp/sun3x-kbuild olddefconfig
    make O=/tmp/sun3x-kbuild -j$(nproc) vmlinux

## Boot

    qemu-system-m68k -M sun3x -kernel /tmp/sun3x-kbuild/vmlinux \
        -serial mon:stdio -display none

The console is `console=ttyS0` (baked into the kernel via BOOTPARAM).  Reaches
an interactive shell on the native zs; `dmesg` shows
`printk: legacy console [ttyS0] enabled` and
`sun3x-scc: ttyS0 MMIO:0x62000000 ... is a Z8530`, `/proc/consoles` lists
`ttyS0` (no hvc0), and typing echoes.  `cat /proc/cpuinfo` shows 68030 + a
68030 MMU.  The initramfs is compiled into the kernel, so no `-initrd` is
needed (the machine also supports `-initrd`, passed via BI_RAMDISK).
