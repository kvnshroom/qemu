/*
 * Sun 3/80 ("sun3x") system emulation.
 *
 * A Motorola 68030 workstation using the CPU's on-chip PMMU (the same unit
 * already exercised by HP9000/340 and Apollo) — NOT the discrete Sun-3 MMU of
 * the sun3-60.  Boots the Linux/m68k `sun3x` kernel (CONFIG_SUN3X) to a serial
 * shell over the Zilog Z8530 (zs) via the resident boot PROM console.
 *
 * The kernel is entered MMU-off at its ELF entry in RAM (PAGE_OFFSET==0, so
 * kernel VA==PA), with a standard m68k bootinfo block appended after the
 * image; head.S builds the page tables and engages the PMMU.  In addition the
 * sun3x head.S path expects a resident boot PROM: it copies the PROM's page
 * table (root pointer read indirectly through 0xfefe00d4) for the mapped
 * window 0xfee00000..0xff000000, and both head.S and the kernel console reach
 * the serial port only through the PROM's putchar/getchar romvec routines.
 * We therefore synthesise a minimal PROM (romvec + identity page table + a few
 * hand-assembled m68k stubs that poke the zs).  See sun3x-NOTES.md.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/reset.h"
#include "system/system.h"
#include "system/rtc.h"
#include "exec/target_page.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/scsi/ncr5380.h"
#include "hw/scsi/scsi.h"
#include "system/blockdev.h"
#include "hw/intc/m68k_irqc.h"
#include "hw/core/or-irq.h"
#include "hw/net/lance.h"
#include "net/net.h"
#include "chardev/char.h"
#include "qemu/timer.h"
#include "qemu/datadir.h"
#include "system/address-spaces.h"
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"
#include "trace.h"

/* --- physical memory map (see sun3x-NOTES.md) --- */
#define SUN3X_RAM_BASE          0x00000000
#define SUN3X_RAM_DEFAULT       (16 * MiB)
#define SUN3X_RAM_MAX           (64 * MiB)

#define SUN3X_IOMMU             0x60000000
#define SUN3X_IOMMU_SIZE        0x8000      /* DVMA I/O mapper: 2048 PTEs of 4
                                             * bytes are used (16 MiB of 8 KiB
                                             * DVMA pages); the PROM self-test
                                             * walks up to 0x600079fc */
#define SUN3X_ENAREG            0x61000000  /* enable/copro block base */
#define SUN3X_ENA_SIZE          0x2000      /* covers INTREG @+0x1400, DIAG @+0x1800 */
#define SUN3X_BUSERR            0x61000400  /* OBIO_BUSERRREG */
#define SUN3X_BE_TIMEOUT        0x20        /* bus timeout (bit 5) */
#define SUN3X_BE_MEMERR         0x80        /* memory (parity) error (bit 7):
                                             * the installed kernel's T_BUSERR
                                             * path tests exactly this bit on
                                             * machine type 0x42 before
                                             * reading the memory error reg */

/*
 * The monitor's periodic (level-7 NMI) clock.  Its handler reaches the clock
 * registers through virtual addresses 0xfef06010 (pending) and 0xfef0b400
 * (ack).  These are 8 KiB (TC.PS=13) resident pages; the 68030 PMMU discards
 * the low (PS-8) bits of a page descriptor's frame field (MC68030 UM 9.1.2 /
 * MC68851 UM 5.1.5.3.15 "Page Address" -- real silicon ignores them, they are
 * reserved for software), so the correct physical address is the descriptor's
 * frame masked to the 8 KiB boundary, OR'd with the low 13 bits of the
 * virtual address (0x010 and 0x1400 respectively) -- NOT the sum of the raw,
 * untruncated descriptor field and the offset that get_physical_address_030()
 * produced before that bug was fixed.  0xfef0b400's offset (0x1400) lands
 * exactly on SUN3X_INTREG's own offset within its 8 KiB OBIO page: the
 * "ack" is not a separate device, it is a plain write to the interrupt
 * register (folded into sun3x_intreg_write() below).  0xfef06010's offset
 * (0x010) lands inside the EEPROM/clock 8 KiB page.
 */
#define SUN3X_CLK_STAT          0x64000010  /* phys of virt 0xfef06010 */
#define SUN3X_CLK_STAT_VA       0xfef06010  /* the monitor's virtual address */
#define SUN3X_CLK_STAT2         0x64002010  /* SunOS kernel's own clock-pending
                                             * status, reached through its own
                                             * page tables at virtual
                                             * 0xfedfc010 -- a distinct
                                             * physical register from the
                                             * monitor's (offset +0x2000
                                             * further into the same OBIO
                                             * clock-chip window), read by the
                                             * kernel's level-5 hardclock ISR */
#define SUN3X_INTREG            0x61001400  /* offset 0x1400 within the block;
                                             * also where the monitor's clock
                                             * ack (virt 0xfef0b400) lands */
#define SUN3X_ZS1               0x62000000  /* console Z8530 */
#define SUN3X_ZS2               0x62002000  /* keyboard/mouse Z8530 (idle) */
#define SUN3X_EEPROM            0x64000000  /* NVRAM: idprom @+0x7d8, clock @+0xf8 */
#define SUN3X_EEPROM_SIZE       0x800
#define SUN3X_IDPROM_OFF        0x7d8
#define SUN3X_CLOCK_OFF         0xf8        /* Linux shim's BCD snapshot */
#define SUN3X_TOD_OFF           0x7f8       /* genuine MK48T02 TOD clock */

/* PROM window: identity-mapped, holds romvec + page table + stubs */
#define SUN3X_PROM_PHYS         0xfee00000
#define SUN3X_PROM_SIZE         0x200000
#define SUN3X_PROM_BASE         0xfefe0000  /* romvec (SUN3X_PROM_BASE) */

/*
 * SunOS (real-PROM) mode: when a boot PROM image is supplied (-bios), the
 * synthesised Linux romvec is replaced by the genuine Sun 3/80 boot PROM.
 * The real PROM decodes as the top 128 KiB of the 0xfee00000 window; the
 * 0xfef00000..0xfefe0000 region just below it is the monitor's private
 * scratch/stack/VBR RAM (reset SSP=0xfef60c00, VBR set to 0xfef60c00).
 * Unmapped physical space raises a genuine 68030 bus error (no catch-all
 * background), which the PROM self-test relies on to size memory and probe
 * for optional devices.
 */
#define SUN3X_ROM_SIZE          0x20000     /* 128 KiB genuine boot PROM */
#define SUN3X_PROMRAM_BASE      0xfef00000  /* monitor scratch/stack/VBR */
#define SUN3X_PROMRAM_SIZE      (SUN3X_PROM_BASE - SUN3X_PROMRAM_BASE)
#define SUN3X_MON_VBR           0xfef60c00  /* the monitor's OWN, fixed VBR
                                             * (reset SSP == VBR, see above).
                                             * The SunOS kernel installs its
                                             * own VBR elsewhere (observed
                                             * 0xf8003800) as one of its very
                                             * first acts; used to tell
                                             * "the monitor is in control"
                                             * apart from "the kernel merely
                                             * inherited/copied a vector the
                                             * monitor once wrote" -- see
                                             * sun3x_update_irq(). */

/*
 * The genuine PROM is also decoded low, at 0x63000000: the cold-start
 * self-test rebases its own data/vector references there (it sets
 * VBR := 0x63000000 and reads its TC sentinel from 0x63007860), so its
 * exception vectors (e.g. bus error, vector 2 -> ROM+8) resolve through it
 * while it probes the bus.
 */
#define SUN3X_PROM_ALIAS        0x63000000
#define SUN3X_PROM_ALIAS_SIZE   0x1000000   /* 16 MiB: ROM shadow + reloc RAM */

/* offsets within the PROM region (relative to SUN3X_PROM_PHYS) */
#define OFF(va)                 ((va) - SUN3X_PROM_PHYS)
#define ROMVEC_VA               0xfefe0000
#define D4_VA                   0xfefe00d4  /* head.S reads *(0xfefe00d4) */
#define PTP_VA                  0xfefe0200  /* page-table-pointer word */
#define PGTABLE_VA              0xfefe0400  /* 256 x u32 page descriptors */
#define PUTCHAR_VA              0xfefe0800
#define GETCHAR_VA              0xfefe0840
#define HALT_VA                 0xfefe0880
#define MONID_VA                0xfefe08a0

/* romvec field offsets used by the Linux sun3x port (openprom.h) */
#define RV_GETCHAR              20
#define RV_PUTCHAR              24
#define RV_NBGETCHAR            28
#define RV_NBPUTCHAR            32
#define RV_MONID                76
#define RV_REBOOT               96
#define RV_ABORT                152

/* 68030 short page descriptor bits for the identity PROM mapping */
#define SUN3X_PROM_PTE_FLAGS    0x59  /* PRESENT|ACCESSED|DIRTY|NOCACHE030 */

/* IDPROM: struct idprom, checksum = xor of bytes[0..14] */
#define SM_SUN3X                0x40
#define SM_3_80                 0x02
#define SUN3X_MACHTYPE          (SM_SUN3X | SM_3_80)     /* 0x42 */

/* interrupt register bits */
#define SUN3X_INT_ENABLE_ALL    0x01
#define SUN3X_INT_SOFT_1        0x02        /* software interrupt, level 1 */
#define SUN3X_INT_SOFT_2        0x04        /* software interrupt, level 2 */
#define SUN3X_INT_SOFT_3        0x08        /* software interrupt, level 3 */
#define SUN3X_INT_ENABLE_5      0x20

/* clock tick rate (Linux/m68k CONFIG_HZ default) */
#define SUN3X_HZ                100

/* Z8530 (ESCC) PCLK, it_shift==1: channel = (addr >> 2) & 1, reg = (addr >> 1) & 1 */
#define SUN3X_ZS_FREQ           4915200

typedef struct Sun3xState {
    M68kCPU *cpu;
    DeviceState *irqc;
    DeviceState *zs_orgate;      /* wire-ORs both Z8530s' 4 IRQ outputs onto
                                  * the shared level-6 autovector line, exactly
                                  * like real hardware's open-drain INT wiring */
    DeviceState *lance_orgate;   /* wire-ORs the "le" LANCE ethernet IRQ and
                                  * the software interrupt-3 bit onto the
                                  * shared level-3 autovector line (see
                                  * sun3x_update_irq()) */
    DeviceState *si_orgate;      /* wire-ORs the "si" NCR5380 SCSI IRQ and the
                                  * software interrupt-2 bit onto the shared
                                  * level-2 autovector line (see
                                  * sun3x_update_irq()) */

    MemoryRegion bg;            /* absorb/zero background */
    MemoryRegion iommu;         /* DVMA page table scratch */
    MemoryRegion enareg;        /* enable/copro + LED block (absorbed) */
    MemoryRegion intreg_mr;     /* the interrupt/enable register */
    MemoryRegion eeprom;        /* NVRAM (idprom + clock) */
    MemoryRegion prom;          /* synthesised (Linux) or real (SunOS) PROM */
    MemoryRegion prom_alias;    /* SunOS: PROM also decoded at 0x63000000 */
    MemoryRegion promram;       /* SunOS: monitor scratch/stack/VBR RAM */
    MemoryRegion obio;          /* SunOS: OBIO device window (absorbed) */
    MemoryRegion memhole;       /* SunOS: absent memory banks (read 0) */
    MemoryRegion buserr_mr;     /* SunOS: bus error register @ 0x61000400 */
    MemoryRegion memreg_mr;     /* SunOS: memory error register @ 0x61001000 */
    MemoryRegion memerraddr_mr; /* SunOS: memory error address reg @ +0x4 */
    MemoryRegion parity_overlay; /* SunOS: forced-parity RAM write snoop,
                                  * enabled only while the CSR's TEST bit is
                                  * set (see sun3x_memreg_write()) */
    AddressSpace ram_as;        /* RAM below the parity overlay */
    MemoryRegion idprobe_hole;  /* SunOS: bus-error hole @ 0x61000c00 where
                                 * the monitor's and the kernel's sun3-IDPROM
                                 * probes land */
    MemoryRegion busfault;      /* SunOS: background bus-timeout (latches) */
    MemoryRegion clkstat;       /* SunOS: clock int-pending @ 0x64000010 */
    MemoryRegion clkstat2;      /* SunOS: kernel-side clock int-pending
                                 * @ 0x64002010 (see SUN3X_CLK_STAT2) */

    NCR5380State scsi;          /* SunOS: "si" NCR5380 @ 0x66000000 */
    MemoryRegion sireg;         /* SunOS: si DMA/CSR block @ 0x66001000 */
    MemoryRegion fdc;           /* SunOS: 82072 FDC @ 0x6e000000 (no drive) */
    uint8_t fdc_st0;            /* FDC: pending SENSE-INTERRUPT ST0 result */
    uint8_t fdc_fifo[8];        /* FDC: result bytes to return via data reg */
    uint8_t fdc_res_len, fdc_res_pos;
    uint8_t fdc_cmd[8];        /* FDC: command bytes being collected */
    uint8_t fdc_cmd_len, fdc_cmd_need;
    uint32_t si_dma_addr;       /* si DMA address (into DVMA space) */
    uint32_t si_dma_count;      /* si DMA byte count */
    uint16_t si_csr;            /* si control/status register */
    bool si_irq_raw;            /* raw NCR5380 /IRQ level (forwarded as-is) */
    uint8_t si_reg20;           /* si scratch register @ +0x20 (read-back) */
    uint8_t si_reg24;           /* si dma-control scratch @ +0x24 */
    uint8_t si_reg2c;           /* si scratch @ +0x2c */
    MemoryRegion siscratch;     /* si scratch overlay reclaiming +0x20/+0x24 */

    bool sunos;                 /* real-PROM (SunOS) mode */
    uint8_t buserr;             /* SunOS bus error register latch */
    uint8_t memreg;             /* SunOS memory error/control register
                                 * (r/w control bits only: INTENA 0x40,
                                 * TEST 0x20, CHECK 0x10) */
    uint8_t memerr_status;      /* SunOS: latched error status -- INTR
                                 * (0x80) plus the byte-lane bits (ERR24
                                 * 0x08 / ERR16 0x04 / ERR08 0x02 / ERR00
                                 * 0x01); see sun3x_parity_write() */
    uint32_t memerr_addr;       /* SunOS: latched faulting address */
    GHashTable *parity_poison;  /* SunOS: byte addresses holding bad
                                 * parity (written while TEST was set) */
    MemoryRegion tod;           /* SunOS: MK48T02 TOD clock, NVRAM's last 8 B */
    MemoryRegion lpreg;         /* SunOS: printer/diagnostic port @ 0x6f00003c */
    uint8_t lpreg_val;          /* its latched byte (read/write) */
    uint8_t tod_ctrl;           /* TOD control register (WRITE/READ bits) */
    uint8_t tod_regs[8];        /* TOD shadow (frozen reads / pending writes) */
    int64_t tod_offset;         /* TOD time - host time, seconds */
    bool tod_stopped;           /* TOD oscillator halted (seconds bit 7) */
    bool clk_pending;           /* SunOS level-7 clock interrupt pending */
    bool clk_enabled;           /* SunOS: monitor has mapped in its clock */
    bool level5_active;         /* SunOS: kernel has acked >=1 real level-5
                                 * hardclock interrupt -- once true, the
                                 * level-7 line reverts to being the genuine
                                 * abort/NMI line (never auto-pulsed by the
                                 * tick) instead of standing in for the
                                 * monitor's boot-countdown clock */

    QEMUTimer *tick;
    uint8_t intreg;
    bool tick_latch;

    uint32_t reset_pc;
    uint32_t reset_sp;
} Sun3xState;

/* --- interrupt / timer --- */

static void sun3x_update_irq(Sun3xState *s)
{
    bool master = s->intreg & SUN3X_INT_ENABLE_ALL;
    /*
     * The periodic clock is level-5, gated on the interrupt register's
     * ENABLE_5 bit and acked by the handler pulsing that same bit low then
     * high again (see sun3x_intreg_write()).  This is true both for the
     * Linux shim (which owns the whole INTREG contract) and for genuine
     * SunOS: the real kernel's hardclock ISR (traced at VBR+0x74, entry
     * 0xf80045da in the 4.1.1 MINIROOT kernel) does the exact same
     * `andib #~0x20,INTREG; orib #0x20,INTREG` pulse through its own page
     * tables (virtual 0xfedf9400, confirmed via m68k_cpu_get_phys_addr_debug
     * to resolve to physical SUN3X_INTREG) -- there is no separate ack
     * device or level distinction to special-case here.
     */
    bool level5 = s->tick_latch && master && (s->intreg & SUN3X_INT_ENABLE_5);

    /* level 5 autovector -> IRQC gpio index 4 (LEVEL_1..7 are 0-based) */
    qemu_set_irq(qdev_get_gpio_in(s->irqc, 5 - 1), level5);

    /*
     * SunOS/monitor: the boot-countdown clock is the level-7 (NMI) auto-vector.
     * It goes live only once the monitor has installed its page tables (the
     * clock registers become reachable through the monitor's virtual address
     * 0xfef06010, which maps to our clock device at SUN3X_CLK_STAT) — this keeps
     * it inert during the self-test (identity mapping) and off until the monitor
     * owns the clock and has set up its interrupt stack.  Delivery is a held
     * level cleared by the handler's ack; the ack is a plain write to
     * SUN3X_INTREG (see sun3x_intreg_write()) which clears the latch so the
     * next tick re-asserts the level and re-arms the NMI edge.
     */
    if (s->sunos) {
        /* latch "clock enabled" the first time the monitor's page tables map
         * its clock register through to our device; thereafter fire every tick
         * (re-checking the live mapping each tick flakes while the handler runs
         * in a different context). */
        if (!s->clk_enabled &&
            m68k_cpu_get_phys_addr_debug(CPU(s->cpu),
                SUN3X_CLK_STAT_VA) == SUN3X_CLK_STAT) {
            s->clk_enabled = true;
        }
        /*
         * Only deliver the edge while the monitor's level-7 vector
         * (VBR+0x7c, translated through its live page tables) holds a
         * handler.  The clock-register mapping can appear a tick or
         * two before the monitor copies its vector table into the
         * currently mapped vector page; an NMI in that window vectors
         * through 0 and storms the ISP into a double bus fault.  Real
         * hardware has no such window: the monitor only programs the
         * clock chip's interrupt enable once the handler is in place.
         */
        bool vec_ok = false;
        if (s->clk_enabled) {
            CPUM68KState *env = &s->cpu->env;
            /*
             * A nonzero handler at VBR+0x7c is not enough to prove that
             * the monitor's boot-countdown handler owns level 7: the SunOS
             * kernel's head.S inherits the monitor's vector table, and once
             * the kernel installs its own level-7 handler (the memory-error
             * NMI handler, never a clock) that slot is nonzero too, so the
             * pulse storms kernel text with "stray level 7 interrupt".  The
             * monitor is unambiguously identified by its own fixed VBR
             * (SUN3X_MON_VBR); the kernel's VBR is elsewhere (0xf8003800)
             * from the moment its head.S runs 'movec %a0,%vbr'.
             */
            if (env->vbr == SUN3X_MON_VBR) {
                hwaddr vph = m68k_cpu_get_phys_addr_debug(CPU(s->cpu),
                                                          env->vbr + 0x7c);
                if (vph != -1) {
                    vec_ok = ldl_be_phys(&address_space_memory, vph) != 0;
                }
            }
        }
        /*
         * The periodic level-7 (NMI) edge exists ONLY to drive the monitor's
         * boot-countdown handler, i.e. only while the monitor itself (its own
         * fixed VBR) is in control -- see `vec_ok` above.  It is not the
         * SunOS kernel's hz clock -- that is level-5 (see `level5` above).
         * Real Sun-3/80 hardware leaves level-7 wired to the abort (L1-A)
         * NMI switch once the kernel is running; it does not keep firing
         * from the periodic timer.  `level5_active` is kept as a second,
         * belt-and-braces gate (harmless once `vec_ok` already excludes the
         * kernel by VBR).
         *
         * Memory (parity) errors are NOT a level-7 source on this board for
         * CPU cycles: they terminate the faulting cycle with a bus error
         * (see sun3x_parity_read()).
         */
        bool level7 = s->clk_pending && s->clk_enabled && vec_ok &&
                      !s->level5_active;
        qemu_set_irq(qdev_get_gpio_in(s->irqc, 7 - 1), level7);
    }

    /*
     * Software interrupts (SunOS/PROM): writing bit 1/2/3 of the interrupt
     * register raises an autovector interrupt at level 1/2/3.  The Linux shim
     * never sets these bits, so gate on SunOS mode to keep it byte-identical.
     *
     * Level 3 is shared with the "le" LANCE ethernet chip's own hardware IRQ
     * (autoconfig prints "le0 ... pri 3"), so route the software-interrupt-3
     * bit through s->lance_orgate's second input instead of driving the irqc
     * gpio directly -- the same "last writer wins" hazard fixed for the two
     * Z8530s' shared level-6 line via s->zs_orgate applies here too.
     */
    if (s->sunos) {
        qemu_set_irq(qdev_get_gpio_in(s->irqc, 1 - 1),
                     master && (s->intreg & SUN3X_INT_SOFT_1));
        qemu_set_irq(qdev_get_gpio_in(s->si_orgate, 1),
                     master && (s->intreg & SUN3X_INT_SOFT_2));
        qemu_set_irq(qdev_get_gpio_in(s->lance_orgate, 1),
                     master && (s->intreg & SUN3X_INT_SOFT_3));
    }
}

static void sun3x_tick(void *opaque)
{
    Sun3xState *s = opaque;

    s->tick_latch = true;

    /*
     * SunOS/monitor level-7 (NMI) clock edge: present a fresh 0->7 edge every
     * tick for the monitor's boot-countdown handler (sun3x_update_irq() only
     * delivers it while the monitor owns the vector table).  The only long
     * IPL-7 busy-wait it could nest in -- the kernel's FDC-probe MSR poll --
     * is answered by the 82072 responder and never times out.
     */
    s->clk_pending = false;
    sun3x_update_irq(s);
    s->clk_pending = true;
    sun3x_update_irq(s);

    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / SUN3X_HZ);
}

/* --- SunOS periodic-clock (level 7) status, in the EEPROM/clock window --- */

static uint64_t sun3x_clkstat_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    /* the monitor's level-7 handler reads this and only advances its tick
     * counter when it is non-zero (clock interrupt pending) */
    return s->clk_pending ? 0x01 : 0x00;
}

static void sun3x_clkstat_write(void *opaque, hwaddr addr, uint64_t val,
                                unsigned size)
{
}

static const MemoryRegionOps sun3x_clkstat_ops = {
    .read = sun3x_clkstat_read,
    .write = sun3x_clkstat_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * The SunOS kernel reaches the SAME clock-pending status through a second
 * virtual address, 0xfedfc010, which m68k_cpu_get_phys_addr_debug() resolves
 * (through the kernel's own live page tables) to physical SUN3X_CLK_STAT2 --
 * 0x2000 further into the OBIO clock-chip window than the monitor's
 * SUN3X_CLK_STAT.  Confirmed this is a hardware address-decode alias, not a
 * second register: the boot PROM self-test's own free-running "is the
 * oscillator running" background check also polls this exact address (as
 * well as SUN3X_CLK_STAT) expecting the identical toggling signal -- pointing
 * a *different* status bit here (originally tried: mirroring tick_latch)
 * broke that self-test ("Oscilator is not running!").  So this is wired to
 * the SAME ops/state as sun3x_clkstat_ops, just registered at the alias
 * address.
 */

/* --- SunOS "si" NCR5380 DMA/CSR block (0x66000008.. and 0x66001000) --- */

/* si control/status register bits (authoritative, NetBSD sun3 sireg.h) */
#define SI_CSR_DMA_ACTIVE   0x8000  /* r/o */
#define SI_CSR_DMA_CONFLICT 0x4000  /* r/o */
#define SI_CSR_DMA_BUS_ERR  0x2000  /* r/o */
#define SI_CSR_ID           0x1000  /* r/o: 1=SCSI-3/EMULEX */
#define SI_CSR_FIFO_FULL    0x0800  /* r/o */
#define SI_CSR_FIFO_EMPTY   0x0400  /* r/o */
#define SI_CSR_SBC_IP       0x0200  /* r/o: 5380 interrupt pending */
#define SI_CSR_DMA_IP       0x0100  /* r/o: DMA interrupt pending */
#define SI_CSR_DMA_EN       0x0010  /* r/w: DMA enable */
#define SI_CSR_SEND         0x0008  /* r/w: DMA direction 1=to device */
#define SI_CSR_INTR_EN      0x0004  /* r/w: interrupt enable */
#define SI_CSR_FIFO_RES     0x0002  /* r/w: fifo reset */
#define SI_CSR_SCSI_RES     0x0001  /* r/w: scsi bus reset */
/*
 * The real reset strobe on this hardware is bit 0x80, not SI_CSR_SCSI_RES
 * (0x0001) above despite that bit's NetBSD-derived name: bit 0 toggles on
 * every ordinary command's finalise sequence (~4000 traced si_csr writes),
 * while the PROM self-test's, ufsboot's and the kernel "sm" driver's
 * sm_reset() bus-reset sequences all write si_csr=0x80 then si_csr=0
 * (kernel text 0xf8075158/0xf8075174, matching the console's "resetting
 * scsi bus" print one-for-one) and never set 0x80 otherwise.
 */
#define SI_CSR_SUN_RESET    0x0080  /* w/o: genuine SCSI bus reset strobe */

/*
 * Forward the NCR5380's raw /IRQ output to the OBIO level-2 line,
 * unconditionally: the kernel "sm" driver never writes SI_CSR_INTR_EN
 * (0x0004), not even in post-boot I/O, so gating the line on it (once tried
 * to silence a storm) merely disabled the interrupt and left the driver's
 * interrupt-driven launch path (`tstl %d5; beqs <busy-poll>` after the
 * select write at 0xf8073170) sleeping in tsleep()/biowait() forever.  The
 * kernel masks level 2 in SR while it polls the hardware during autoconfig,
 * so 1:1 wiring does not storm there; the few unclaimable pulses left by
 * ncr5380.c's "sun_cmd_complete trailing calls" after `dev` has gone NULL
 * are bounded and reaped by ncr5380_sun_reap_stale_irq().
 */
static void sun3x_si_update_irq(Sun3xState *s)
{
    /*
     * Level 2 is SHARED: the "si" NCR5380 SCSI controller (autoconfig prints
     * "sm0 at obio 0x66000000 pri 2") and the interrupt register's
     * software-interrupt-2 bit both drive this one autovector line.  Route
     * both through s->si_orgate instead of driving the irqc gpio directly --
     * the same open-drain wire-OR topology already used for the two Z8530s'
     * shared level-6 line (s->zs_orgate) and for the LANCE/software-
     * interrupt-3 pair (s->lance_orgate).
     *
     * Driving the gpio directly from both places is "last writer wins", and
     * that lost real SCSI completions: sun3x_update_irq() unconditionally
     * re-drives the level-2 input from the SOFT_2 bit alone, and it runs
     * 200 times a second from sun3x_tick() plus on every single interrupt-
     * register write -- including the hardclock ISR's own two-write
     * `andib #~0x20 / orib #0x20` ack pulse, every 10 ms tick.  Any si
     * interrupt request the CPU had not yet taken was therefore destroyed at
     * the interrupt controller (m68k_irqc's per-level `ipr` bit cleared),
     * while the si model itself still held its own line asserted
     * (s->si_irq_raw == 1, BSR_IRQ set) with nothing left to re-assert the
     * gpio.  The sleeping process was then never woken and the driver's own
     * 60-140 s watchdog eventually declared "sm0: sm_deque: lost interrupt"
     * / "sd0: I/O request timeout" / "sm0: resetting scsi bus".  Evidence:
     * 557 consecutive 200 ms samples of a stalled guest, all in the
     * kernel's idle loop (pc 0xf80645d8) at SR 0x2000, i.e. IPL 0, with
     * s->si_irq_raw == 1 and a command still in flight.
     */
    qemu_set_irq(qdev_get_gpio_in(s->si_orgate, 0), s->si_irq_raw);
}

static void sun3x_si_irq(void *opaque, int n, int level)
{
    Sun3xState *s = opaque;

    s->si_irq_raw = level;
    sun3x_si_update_irq(s);
}

/*
 * Translate a sun3x DVMA address to a physical main-memory address using the
 * IOMMU page table the PROM builds at SUN3X_IOMMU: 16 MiB DVMA space, 8 KiB
 * pages, 2048 PTEs of 4 bytes.  PTE valid when the low bits are set;
 * phys = (pte & 0xffffe000) | (dva & 0x1fff).
 */
static bool sun3x_dvma_to_phys(uint32_t dva, hwaddr *pa)
{
    uint32_t idx = (dva >> 13) & 0x7ff;
    uint32_t pte = ldl_be_phys(&address_space_memory, SUN3X_IOMMU + idx * 4);

    if (!(pte & 0x3)) {
        return false;               /* invalid mapping */
    }
    *pa = ((hwaddr)(pte & 0xffffe000)) | (dva & 0x1fff);
    return true;
}

/*
 * Copy a run of bytes between a sun3x DVMA address and main memory, one
 * byte at a time (exactly like sun3x_si_dma_run() below), so an invalid PTE
 * partway through a buffer is caught rather than assumed contiguous.
 */
static bool sun3x_lance_dvma_copy(uint32_t dva, uint8_t *buf, int len,
                                  bool to_mem)
{
    while (len-- > 0) {
        hwaddr pa;

        if (!sun3x_dvma_to_phys(dva, &pa)) {
            return false;
        }
        if (to_mem) {
            stb_phys(&address_space_memory, pa, *buf);
        } else {
            *buf = ldub_phys(&address_space_memory, pa);
        }
        buf++;
        dva++;
    }
    return true;
}

/*
 * hw/net/lance.c's DMA hooks for the "le" LANCE (Am7990), translating the
 * chip's 24-bit DVMA addresses through the same IOMMU used by the "si" SCSI
 * pseudo-DMA above, instead of sparc/ledma's IOMMU.
 *
 * The sun3lance.c driver (drivers/net/ethernet/amd/sun3lance.c, and SunOS's
 * equivalent "le" driver) always programs CSR3 with BSWP set, matching the
 * sun4m sunlance.c driver -- so reuse the exact do_bswap contract hw/net/
 * lance.c already relies on for sun4m's ledma_memory_read/write() (see
 * "on sparc, the lance 16 bit bus is swapped" in hw/dma/sparc32_dma.c):
 * pcnet.c always calls phys_mem_read/write for descriptor/init-block DMA
 * with do_bswap==0 (so those always get the 16-bit swap below, converting
 * between memory's big-endian words and the LE fields pcnet.c decodes them
 * into), and calls it with do_bswap==CSR_BSWP(s) for frame body DMA (BSWP
 * set means "plain copy", i.e. raw bytes pass through unswapped, which is
 * what a byte-for-byte Ethernet frame needs).
 */
static void sun3x_lance_dma_read(void *opaque, hwaddr addr,
                                 uint8_t *buf, int len, int do_bswap)
{
    if (!sun3x_lance_dvma_copy(addr, buf, len, false)) {
        memset(buf, 0, len);
        return;
    }
    if (!do_bswap) {
        int i;

        for (i = 0; i + 1 < len; i += 2) {
            bswap16s((uint16_t *)(buf + i));
        }
    }
}

static void sun3x_lance_dma_write(void *opaque, hwaddr addr,
                                  uint8_t *buf, int len, int do_bswap)
{
    if (do_bswap) {
        sun3x_lance_dvma_copy(addr, buf, len, true);
    } else {
        g_autofree uint8_t *tmp = g_malloc(len);
        int i;

        memcpy(tmp, buf, len);
        for (i = 0; i + 1 < len; i += 2) {
            uint8_t t = tmp[i];

            tmp[i] = tmp[i + 1];
            tmp[i + 1] = t;
        }
        sun3x_lance_dvma_copy(addr, tmp, len, true);
    }
}

/*
 * Run the si pseudo-DMA engine: move up to si_dma_count bytes between the
 * NCR5380 data phase and DVMA memory (through the IOMMU).  SI_CSR_SEND set
 * means data-out (initiator -> target).  Sets DMA_IP when the run drains.
 */
static void sun3x_si_dma_run(Sun3xState *s)
{
    NCR5380State *n = &s->scsi;
    bool out;

    /*
     * Direction comes from the 5380's live data phase, NOT the si_csr bits.
     * The PROM's DMA-arm (0xfeff0e04) encodes a data-IN read as si_csr|=0x300
     * (SBC_IP|DMA_IP) and a data-OUT write as si_csr|=0x200 (SBC_IP only) —
     * the opposite of a naive SEND/DMA_IP reading — so inferring the direction
     * from those bits would run the transfer the wrong way and move nothing.
     * The bus phase (set by the SCSI command executed at selection) is
     * authoritative.
     */
    if (ncr5380_pdma_ready(n, false)) {
        out = false;                    /* data-in  (READ/INQUIRY/...) */
    } else if (ncr5380_pdma_ready(n, true)) {
        out = true;                     /* data-out (WRITE/MODE SELECT) */
    } else {
        s->si_csr |= SI_CSR_DMA_IP;     /* nothing to move */
        return;
    }

    while (s->si_dma_count > 0 && ncr5380_pdma_ready(n, out)) {
        hwaddr pa;

        if (!sun3x_dvma_to_phys(s->si_dma_addr, &pa)) {
            s->si_csr |= SI_CSR_DMA_BUS_ERR;
            break;
        }
        if (out) {
            uint8_t b = ldub_phys(&address_space_memory, pa);
            ncr5380_pdma_write(n, b);
        } else {
            uint8_t b = ncr5380_pdma_read(n);
            stb_phys(&address_space_memory, pa, b);
        }
        s->si_dma_addr++;
        s->si_dma_count--;
    }
    s->si_csr |= SI_CSR_DMA_IP;      /* DMA interrupt / transfer done */
}

/* the guest PC, for the register trace and the kernel-vs-PROM/ufsboot gate */
static unsigned sun3x_guest_pc(void)
{
    if (current_cpu) {
        CPUClass *cc = CPU_GET_CLASS(current_cpu);
        if (cc->get_pc) {
            return (unsigned)cc->get_pc(current_cpu);
        }
    }
    return 0;
}

static uint64_t sun3x_sireg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;
    uint64_t v = 0;

    switch (addr) {
    case 0x00:              /* DMA transfer count, low byte */
        v = s->si_dma_count & 0xff;
        break;
    case 0x04:              /* DMA transfer count, high byte */
        v = (s->si_dma_count >> 8) & 0xff;
        break;
    case 0x20:              /* si scratch register (read-back register test) */
        v = s->si_reg20;
        break;
    case 0x24:              /* si dma-control scratch (read-back) */
        v = s->si_reg24;
        break;
    case 0x2c:              /* si scratch (read-back; failure tolerated) */
        v = s->si_reg2c;
        break;
    case 0x1000: {          /* si control/status register.
                             * Progress gate needs bit0(0x1)=1, FIFO_RES(0x2)=0,
                             * FIFO_EMPTY(0x400)=0.  SBC_IP/DMA_IP are the
                             * driver's DMA-sequencing latches; the 0x40
                             * handshake bit reads back clear (completes at
                             * once). */
        uint16_t v16 = SI_CSR_ID;
        unsigned pc = sun3x_guest_pc();
        v16 |= (s->si_csr & (SI_CSR_SBC_IP | SI_CSR_DMA_IP));
        /*
         * bit0 = "bus/command busy".  The SunOS kernel "sm" driver polls bit0
         * as its completion gate: its state-machine inner loop (0xf8074dc2)
         * re-runs the body WHILE bit0 == 1 and exits (command complete) when
         * bit0 == 0, its entry gate (0xf80735c2, si_csr & 3) needs bit0 == 1 to
         * run at all, and its poll loop (0xf8073548) calls the state machine
         * only while si_csr & 3.  So for the kernel present bit0 = 1 only while
         * a device is selected (a command in flight): a present target holds
         * bit0 = 1 through the command and drops to 0 at bus-free (-> its
         * state-28 completion); an absent target reads bit0 = 0 (-> its
         * no-device path) instead of an infinite spin.  The PROM/ufsboot polled
         * path keeps bit0 hardwired to 1 as before -- their self-test and
         * arbitration prologue read si_csr bit0 expecting 1 even with no device
         * selected.  The kernel and PROM read the si_csr at the SAME offset
         * (0x1000), so distinguish by the guest PC: only the kernel text
         * (0xf8000000..0xf9000000) sees the busy semantics; the PROM
         * (0xfefexxxx) and ufsboot (0x0021xxxx) keep bit0 = 1.
         */
        if (pc >= 0xf8000000u && pc < 0xf9000000u) {
            v16 |= s->scsi.dev ? 0x0001 : 0;
            if (!(v16 & 0x0001)) {
                if (s->scsi.sun_sel_failed) {
                    /*
                     * A selection just failed (no target) under the
                     * interrupt-driven regime: ncr5380_select_target() raised
                     * BSR_IRQ for exactly this read.  Present
                     * SBC_IP|FIFO_EMPTY -- the kernel "sm" interrupt handler's
                     * own gate (SBC_IP) and its next check (FIFO_EMPTY, since
                     * no data phase ever ran) -- so it takes its existing
                     * bounded spin -> "err 0x12" -> claimed-complete path
                     * instead of no-op'ing on a clear SBC_IP.  One-shot: a
                     * fresh selection clears sun_sel_failed before this can
                     * be read again, and normal reg7-less reaping resumes.
                     */
                    v16 |= SI_CSR_SBC_IP | SI_CSR_FIFO_EMPTY;
                    s->scsi.sun_sel_failed = false;
                } else if (s->scsi.sun_cmd_irq) {
                    /*
                     * A kernel-mode, interrupt-driven command just completed
                     * (ncr5380_ack()'s kernel-gated PHASE_MI branch raised
                     * BSR_IRQ -- see the sun_cmd_irq comment in ncr5380.h).
                     * The isrlink contract, from the real /vmunix, needs
                     * bit1 set for a short, fixed run of si_csr reads while
                     * one completion is dispatched: the level-2 dispatch
                     * loop's claim test (`(reg)&3`) before it JSRs into the
                     * device handler, the handler's own entry recheck (same
                     * `&3` mask, "err 0x13" if clear) and its state-machine
                     * entry gate (`(reg) & 0x402`).  Gating those on the
                     * handler's PC locks the model to one kernel binary's
                     * link addresses (GENERIC links _sm_intr at 0xf80bdf0e,
                     * the MINIROOT at 0xf8073586) and stormed the installed
                     * system; the access pattern is what is invariant --
                     * one bit0-only read from the state-machine inner loop,
                     * then exactly three reads that need bit1 (~115 traced
                     * completions) -- so bit1 is presented for the first
                     * four si_csr reads after arming (sun_cmd_irq_reads),
                     * with no PC check.
                     */
                    v16 |= 0x0002;
                    if (--s->scsi.sun_cmd_irq_reads <= 0) {
                        s->scsi.sun_cmd_irq = false;
                    }
                } else {
                    /*
                     * The kernel's isrlink claim test just found nothing to
                     * claim (si_csr & 3 == 0).  If the raw NCR5380 /IRQ is
                     * still asserted, its cause is already stale (see the
                     * comment on ncr5380_sun_reap_stale_irq()) -- reap it now
                     * instead of leaving the physical line permanently up,
                     * which would storm every single instruction the CPU is
                     * unmasked.  ncr5380_update_irq() -> qemu_set_irq() ->
                     * sun3x_si_irq() keeps si_irq_raw and the forwarded OBIO
                     * line in sync.
                     */
                    ncr5380_sun_reap_stale_irq(&s->scsi);
                }
            }
        } else {
            v16 |= 0x0001;
        }
        v = v16;
        break;
    }
    case 0x1004:            /* si DMA address (HW auto-increments during xfer) */
        v = s->si_dma_addr;
        break;
    case 0x1008:            /* si DMA byte count (register test) */
        v = s->si_dma_count;
        break;
    default:
        v = 0;
        break;
    }
    qemu_log_mask(CPU_LOG_MMU, "si rd  @%08x.%u = %08x pc=%08x\n",
                  0x66000000 + (uint32_t)addr, size, (uint32_t)v,
                  sun3x_guest_pc());
    return v;
}

/*
 * Arm the si DMA engine once every register it needs is actually in place.
 * The PROM/ufsboot driver loads dma_addr and the count latches BEFORE it
 * writes si_csr|=SBC_IP (the old, sole call site below), so triggering the
 * run on that write alone was sufficient for it.  The SunOS kernel "sm"
 * driver's real, interrupt-driven regime writes these same registers in a
 * DIFFERENT order -- traced live (READ(6)/INQUIRY, `-d mmu` with PC on every
 * si access): dma_addr, THEN si_csr|=SBC_IP|DMA_IP (0xf80732b2/0xf80732c6),
 * THEN the count latches (0xf807335a/0xf8073364) -- so the old single call
 * site always ran the engine while si_dma_count was still 0, moved zero
 * bytes, and immediately latched sun_dma_done, permanently swallowing the
 * transfer: the driver read dma_addr back unchanged and reported a full
 * residual ("count= 56(56)"/"count= 512(512)", 0 bytes moved) even though
 * ncr5380's own synchronous SCSI-layer pump had the whole reply sitting in
 * dbuf already, then hit the *Lost Busy* wall.  Call this from every one of
 * the four register writes that can complete the "armed" state (both count
 * bytes, the 32-bit count alias, and the si_csr arm write itself) so the run
 * fires the moment the last of them lands, regardless of order: real si
 * hardware does not care which of its own registers the driver loads first,
 * only that all of them are loaded before the transfer is expected to move
 * data.
 */
static void sun3x_si_maybe_run_dma(Sun3xState *s)
{
    if ((s->si_csr & SI_CSR_SBC_IP) && !s->scsi.sun_dma_done &&
        s->si_dma_count > 0 &&
        (ncr5380_pdma_ready(&s->scsi, false) ||
         ncr5380_pdma_ready(&s->scsi, true))) {
        sun3x_si_dma_run(s);
        s->scsi.sun_dma_done = true;
        /*
         * Real hardware re-asserts /REQ for the driver's very next poll
         * the instant this drain happens; latch that so the kernel "sm"
         * driver's reg3 dispatch does not mis-route (see the
         * sun_dma_drained_pending comment in ncr5380.h).
         */
        ncr5380_sun_dma_drained(&s->scsi);
    }
}

static void sun3x_sireg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Sun3xState *s = opaque;

    switch (addr) {
    case 0x00:              /* DMA transfer count, low byte */
        s->si_dma_count = (s->si_dma_count & 0xff00) | (val & 0xff);
        sun3x_si_maybe_run_dma(s);
        break;
    case 0x04:              /* DMA transfer count, high byte */
        s->si_dma_count = (s->si_dma_count & 0x00ff) | ((val & 0xff) << 8);
        sun3x_si_maybe_run_dma(s);
        break;
    case 0x20:
        s->si_reg20 = val;
        break;
    case 0x24:
        s->si_reg24 = val;
        break;
    case 0x2c:
        s->si_reg2c = val;
        break;
    case 0x1000: {
        uint16_t old = s->si_csr;
        s->si_csr = val & 0x03ff;
        if (val & SI_CSR_SUN_RESET) {
            /*
             * Genuine bus reset (see the SI_CSR_SUN_RESET comment above):
             * cancel any still-open SCSI request and clear the 5380's whole
             * bus/transaction state, then drop the si-side DMA latches too
             * so a subsequent, freshly-armed transfer can never inherit a
             * stale address/count from whatever this reset just abandoned.
             */
            ncr5380_sun_bus_reset(&s->scsi);
            s->si_dma_addr = 0;
            s->si_dma_count = 0;
        }
        /*
         * The driver arms a transfer by writing SBC_IP(0x200) [plus DMA_IP
         * for data-in] after loading dma_addr and the count latches; the
         * engine latches DMA_IP on completion and advances dma_addr by the
         * transfer length, which is the byte count the PROM reads back
         * (dma_addr - start) to decide the transfer succeeded.  Gate on
         * sun_dma_done (cleared by ncr5380_do_command
         * for each new command) rather than a rising-edge of SBC_IP -- si_csr
         * is not reliably cleared to 0 between commands (its bits are
         * synthesised on read), so a stale SBC_IP from the previous transfer
         * would otherwise swallow the next arm's edge and the DMA would never
         * run.  See sun3x_si_maybe_run_dma(): this call handles the
         * PROM/ufsboot ordering (count already loaded); the kernel's
         * count-after-arm ordering is handled by the same helper called from
         * the count-register writes above/below.
         */
        sun3x_si_maybe_run_dma(s);
        /*
         * The driver's data-complete path (0xfeff0e6e) clears the DMA
         * interrupt latches (si_csr &= ~0x0300) once it has read the byte
         * count back.  On that falling edge, advance the SCSI bus to STATUS
         * so the driver's status/message handshake can run.
         */
        if (s->scsi.sun_dma_done &&
            (old & (SI_CSR_SBC_IP | SI_CSR_DMA_IP)) &&
            !(val & (SI_CSR_SBC_IP | SI_CSR_DMA_IP))) {
            ncr5380_sun_to_status(&s->scsi);
        }
        /* re-drive the level-2 line from the 5380's current raw /IRQ */
        sun3x_si_update_irq(s);
        break;
    }
    case 0x1004:
        s->si_dma_addr = val;
        sun3x_si_maybe_run_dma(s);
        break;
    case 0x1008:
        s->si_dma_count = val;
        sun3x_si_maybe_run_dma(s);
        break;
    default:
        break;
    }
    qemu_log_mask(CPU_LOG_MMU, "si wr  @%08x.%u = %08x pc=%08x\n",
                  0x66000000 + (uint32_t)addr, size, (uint32_t)val,
                  sun3x_guest_pc());
}

static const MemoryRegionOps sun3x_sireg_ops = {
    .read = sun3x_sireg_read,
    .write = sun3x_sireg_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * The NCR5380 sits at 0x66000008..0x66000027 (reg0 = data @ +0x08), which
 * overlaps the si scratch registers at +0x20/+0x24.  Reclaim those two words
 * at higher priority and forward them to the sireg handlers above.
 */
static uint64_t sun3x_siscratch_read(void *opaque, hwaddr addr, unsigned size)
{
    return sun3x_sireg_read(opaque, addr + 0x20, size);
}

static void sun3x_siscratch_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    sun3x_sireg_write(opaque, addr + 0x20, val, size);
}

static const MemoryRegionOps sun3x_siscratch_ops = {
    .read = sun3x_siscratch_read,
    .write = sun3x_siscratch_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

static uint64_t sun3x_intreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    return s->intreg;
}

static void sun3x_intreg_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Sun3xState *s = opaque;

    /* the tick handler acks by pulsing the level-5 enable low->high */
    if (!(val & SUN3X_INT_ENABLE_5)) {
        /*
         * Was a real level-5 IRQ actually being asserted to the CPU right
         * before this write lands (same formula as the `level5` local in
         * sun3x_update_irq, evaluated against the state as it stood just
         * before this write)?  tick_latch alone is NOT enough to tell: it is
         * set unconditionally by every tick regardless of whether ENABLE_5
         * or master were ever on, so using it by itself here mistook the
         * monitor's own unrelated INTREG writes (e.g. its 0x80->0x81
         * self-test housekeeping, which never touches bit 0x20) for a real
         * kernel ack and switched off the level-7 boot-countdown far too
         * early, hanging the self-test.  Requiring bits ENABLE_ALL and
         * ENABLE_5 to have ALSO been set (mirroring `level5`'s own gating)
         * makes this true only when a real level-5 edge was live.
         */
        bool was_level5_up = s->tick_latch && (s->intreg & SUN3X_INT_ENABLE_ALL)
                              && (s->intreg & SUN3X_INT_ENABLE_5);
        if (s->sunos && was_level5_up) {
            /*
             * A genuine level-5 edge was asserted and is now being ack'd by
             * the real SunOS kernel's hardclock ISR.  This is proof the real
             * hz clock is alive: from here on the level-7 auto-pulse (see
             * sun3x_update_irq) is no longer needed to stand in for it and
             * is switched off so it can't storm once the kernel's own
             * vector table takes over that slot.
             */
            s->level5_active = true;
        }
        s->tick_latch = false;
    }
    /*
     * SunOS/monitor: virtual 0xfef0b400, once the page-address fix (real
     * hardware ignores the low bits of a page descriptor's frame field)
     * is applied to the walk, lands on this SAME physical register --
     * there is no separate "clock ack" device.  Any write here also acks
     * the level-7 clock latch, exactly like the level-5 tick_latch ack
     * just above.
     */
    if (s->sunos) {
        s->clk_pending = false;
    }
    s->intreg = val;
    sun3x_update_irq(s);
}

static const MemoryRegionOps sun3x_intreg_ops = {
    .read = sun3x_intreg_read,
    .write = sun3x_intreg_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- enable/copro/LED block: reads 0, writes absorbed --- */

static uint64_t sun3x_absorb_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void sun3x_absorb_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
}

static const MemoryRegionOps sun3x_absorb_ops = {
    .read = sun3x_absorb_read,
    .write = sun3x_absorb_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Minimal Intel 82072 floppy controller (Sun-3/80 "fd") at OBIO 0x6e000000.
 * We have no floppy image, so this models an empty controller with NO DRIVE
 * present: the SunOS kernel's fd probe drives the standard NEC/82072 command/
 * result handshake and must conclude "drive absent" and return WITHOUT ever
 * hitting its timeout/reset path (which overflows the tiny early ISP).  The
 * OBIO absorb returned 0 for the MSR, so the poll for (msr & 0xc0)==0x80 never
 * saw "ready" and timed out -> crash.  Register map (asm/sun3x.h): MSR/data at
 * 0x6e000000 (status @+0, data @+1), FCR @0x6e000400, FVR @0x6e000800.
 */
#define FDC_MSR_RQM   0x80   /* request for master (host may xfer a byte) */
#define FDC_MSR_DIO   0x40   /* data direction: 1 = controller -> host */
#define FDC_MSR_CB    0x10   /* command busy (in command/exec/result phase) */

static const uint8_t fdc_cmd_len_tbl[32] = {
    /* command byte low 5 bits (0x1f) -> total command length incl. opcode */
    [0x03] = 3,  /* SPECIFY */
    [0x04] = 2,  /* SENSE DRIVE STATUS */
    [0x07] = 2,  /* RECALIBRATE */
    [0x08] = 1,  /* SENSE INTERRUPT STATUS */
    [0x0a] = 2,  /* READ ID */
    [0x0f] = 3,  /* SEEK */
    [0x0e] = 1,  /* DUMPREG (82072) */
    [0x10] = 1,  /* VERSION */
    [0x12] = 2,  /* PERPENDICULAR MODE */
    [0x13] = 4,  /* CONFIGURE */
    [0x14] = 1,  /* LOCK */
    [0x02] = 9, [0x05] = 9, [0x06] = 9, [0x09] = 9, [0x0c] = 9, [0x0d] = 9,
};

/* set up the result bytes the host will read from the data register */
static void fdc_set_result(Sun3xState *s, const uint8_t *r, int n)
{
    int i;
    for (i = 0; i < n && i < (int)sizeof(s->fdc_fifo); i++) {
        s->fdc_fifo[i] = r[i];
    }
    s->fdc_res_len = n;
    s->fdc_res_pos = 0;
}

/* a full command has been collected in s->fdc_cmd[] -> execute it */
static void fdc_exec(Sun3xState *s)
{
    uint8_t cmd = s->fdc_cmd[0] & 0x1f;
    uint8_t unit = s->fdc_cmd_len > 1 ? (s->fdc_cmd[1] & 3) : 0;
    uint8_t r[10];

    switch (cmd) {
    case 0x08: /* SENSE INTERRUPT STATUS: ST0, PCN */
        r[0] = s->fdc_st0;      /* the ST0 latched by the last seek/recal */
        r[1] = 0;               /* present cylinder = 0 */
        fdc_set_result(s, r, 2);
        s->fdc_st0 = 0x80;      /* consumed; further senses -> invalid cmd */
        break;
    case 0x07: /* RECALIBRATE (no result phase) */
    case 0x0f: /* SEEK (no result phase) */
        /* absent drive: seek/recalibrate ends with Equipment Check / not
         * ready -> ST0 = IC(01)|SE(20)|EC(10)|unit.  The driver reads this
         * via the following SENSE INTERRUPT STATUS and marks the drive
         * absent. */
        s->fdc_st0 = 0x70 | unit;
        s->fdc_res_len = 0;
        break;
    case 0x04: /* SENSE DRIVE STATUS: ST3 (drive not ready, no track0) */
        r[0] = 0x00 | unit;     /* ST3: not ready, WP/T0/ready all clear */
        fdc_set_result(s, r, 1);
        break;
    case 0x0a: /* READ ID: absent -> ST0 abnormal, ST1 no-data, ... (7) */
        r[0] = 0x40 | unit; r[1] = 0x05; r[2] = 0x00;
        r[3] = 0; r[4] = 0; r[5] = 0; r[6] = 0;
        fdc_set_result(s, r, 7);
        break;
    case 0x10: /* VERSION -> 0x90 (enhanced 82072/765B) */
        r[0] = 0x90; fdc_set_result(s, r, 1);
        break;
    case 0x14: /* LOCK -> lock bit echoed */
        r[0] = (s->fdc_cmd[0] & 0x80) ? 0x10 : 0x00;
        fdc_set_result(s, r, 1);
        break;
    case 0x0e: /* DUMPREG -> 10 zero bytes */
        memset(r, 0, 10); fdc_set_result(s, r, 10);
        break;
    case 0x03: /* SPECIFY - no result */
    case 0x12: /* PERPENDICULAR MODE - no result */
    case 0x13: /* CONFIGURE - no result */
        s->fdc_res_len = 0;
        break;
    default:   /* unknown/data commands: invalid-command result */
        r[0] = 0x80; fdc_set_result(s, r, 1);
        break;
    }
}

static uint64_t sun3x_fdc_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;
    uint8_t v = 0;

    switch (addr) {
    case 0x00: /* Main Status Register */
        v = FDC_MSR_RQM;                    /* always ready for a byte */
        if (s->fdc_res_pos < s->fdc_res_len) {
            v |= FDC_MSR_DIO | FDC_MSR_CB;  /* result phase: read from us */
        } else if (s->fdc_cmd_len) {
            v |= FDC_MSR_CB;                /* mid command phase */
        }
        break;
    case 0x01: /* data / FIFO */
        if (s->fdc_res_pos < s->fdc_res_len) {
            v = s->fdc_fifo[s->fdc_res_pos++];
        }
        break;
    default:   /* FCR (0x400) / FVR (0x800) read back */
        v = 0;
        break;
    }
    return v;
}

static void sun3x_fdc_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Sun3xState *s = opaque;

    switch (addr) {
    case 0x01: /* data / FIFO: collect a command byte */
        if (s->fdc_cmd_len == 0) {
            s->fdc_cmd[0] = val;
            s->fdc_cmd_need = fdc_cmd_len_tbl[val & 0x1f];
            if (s->fdc_cmd_need == 0) {
                s->fdc_cmd_need = 1;        /* unknown: 1-byte, invalid */
            }
            s->fdc_cmd_len = 1;
        } else if (s->fdc_cmd_len < (int)sizeof(s->fdc_cmd)) {
            s->fdc_cmd[s->fdc_cmd_len++] = val;
        }
        if (s->fdc_cmd_len >= s->fdc_cmd_need) {
            fdc_exec(s);
            s->fdc_cmd_len = 0;
        }
        break;
    case 0x00: /* status/DCR write - absorb */
    default:   /* FCR (0x400) / FVR (0x800) - absorb */
        break;
    }
}

static const MemoryRegionOps sun3x_fdc_ops = {
    .read = sun3x_fdc_read,
    .write = sun3x_fdc_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Absent-memory (floating bus) reads.  A depopulated 3/80 memory bank still
 * ACKs the access (no bus error) but the data lines float high, so a read
 * returns all-ones.  The PROM's memory sizer relies on this: after writing a
 * test pattern to a bank it reads back and treats 0xffffffff as "bank empty"
 * (cleanly absent); any other value is flagged "Bank is partially filled" and
 * drops it into an interactive diagnostic.  Returning 0 here is therefore
 * wrong — absent banks must read as -1.
 */
static uint64_t sun3x_memhole_read(void *opaque, hwaddr addr, unsigned size)
{
    return (size >= 8) ? ~0ULL : (~0ULL >> (64 - size * 8));
}

static const MemoryRegionOps sun3x_memhole_ops = {
    .read = sun3x_memhole_read,
    .write = sun3x_absorb_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- SunOS bus error register (0x61000400) + bus-timeout background --- */

static uint64_t sun3x_buserr_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    return s->buserr;
}

static void sun3x_buserr_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Sun3xState *s = opaque;

    /* the PROM writes 0 here to clear the latched fault bits */
    s->buserr = val;
}

static const MemoryRegionOps sun3x_buserr_ops = {
    .read = sun3x_buserr_read,
    .write = sun3x_buserr_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Memory error/control register (0x61001000), memory error address
 * register (0x61001004), and the bad-parity RAM overlay that backs the
 * PROM's "Parity Memory Test" / "Parity Memory Forced Error Test" and the
 * SunOS kernel's parity-error trap path.
 *
 * Control/status byte (confirmed by the real /vmunix's own field names in
 * its "unknown memory error" panic dump and by the PROM's writes of 0x40 /
 * 0x70 / 0x10 in the two parity subtests, sun3_80_v3.0.3.bin
 * 0xfefe7d4e-0xfefe8094): INTR 0x80 (r/o, error latched), INTENA 0x40,
 * TEST 0x20 (writes store bad parity), CHECK 0x10 (reads check parity),
 * ERR24/ERR16/ERR08/ERR00 0x08/0x04/0x02/0x01 (r/o byte-lane nibble).
 *
 * It is an 8-bit port decoded without A1/A0: the PROM reads it back both
 * with `moveb 0x61001000` and with `movel 0x61001000` followed by
 * `andil #0xf` (0xfefe7fd2), i.e. the nibble it compares against comes
 * from the LOW byte of the longword -- the 68030's dynamic bus sizing
 * runs four byte cycles at +0..+3 and the same register answers all of
 * them.  So the register is mapped 4 bytes wide with every byte a mirror.
 *
 * THE DELIVERY MECHANISM IS A BUS ERROR, NOT A LEVEL-7 INTERRUPT.  Both
 * PROM subtests hook their "parity error (level 7) interrupt" continuation
 * into exception vector 2 (`lea 0x20008,%a0; movel %a1,%a0@`, 0xfefe7dbc
 * and 0xfefe7f1e) with VBR = 0x20000, never into vector 31; the VBR table
 * they run under is the ROM's own romvec relocated by the self-test's
 * installer (0xfefea1ee), whose slot 31 is the `fbthere` char pointer --
 * not code -- so a genuine level-7 autovector at that moment could only
 * ever crash real hardware too.  The installed SunOS 4.1.1 kernel agrees:
 * its T_BUSERR path in _trap (munix_sun3x 0xf805826e) tests the machine
 * type for 0x42 (Sun 3/80) and bit 7 of the BUS ERROR register
 * (0xfedf8400 -> phys 0x61000400), and only then reads this register
 * (0xfedf9000), calls parity_report(), clears the error address register
 * with `andil #0xc0000000` and rewrites the faulting longword; its level-7
 * handler (0xf8004882) merely reports "stray level 7 interrupt" unless
 * INTR is set.  So: a CPU read of a byte lane carrying bad parity, with
 * CHECK set, terminates that cycle with a 68030 bus error, loads the bus
 * error register with 0x80 (MEMERR), and latches INTR + the lane nibble
 * and the byte address here.  No autovector is raised for CPU cycles (the
 * kernel's INTR-gated level-7 path is for non-CPU, i.e. DVMA, errors,
 * which this model never generates).
 *
 * The lane nibble the PROM expects after a single-byte forced write and
 * read-back (0xfefe7fa2-0xfefe7fd2) is 0x7/0xb/0xd/0xe for byte 0/1/2/3
 * of the addressed longword -- 0xf with exactly the bad lane's bit
 * CLEARED.  Modelled literally: nibble = 0xf & ~(lanes read that carry
 * bad parity).  The error address register's low 30 bits hold the byte
 * address (the PROM masks with 0x3fffffff, 0xfefe7f88).
 *
 * Bad parity cannot be stored in QEMU RAM, so (like hw/m68k/sun3.c) a set
 * of poisoned byte addresses is kept: a write with TEST set poisons the
 * bytes written, any other write cleans them.  The MMIO overlay that
 * snoops RAM is enabled only while TEST is set or poison exists, so
 * ordinary boot/kernel RAM traffic never pays for it.
 */
#define SUN3X_MEMERR_CHECK      0x10
#define SUN3X_MEMERR_TEST       0x20
#define SUN3X_MEMERR_INTENA     0x40
#define SUN3X_MEMERR_INTR       0x80
#define SUN3X_MEMERR_CTRL_BITS  (SUN3X_MEMERR_INTENA | SUN3X_MEMERR_TEST | \
                                 SUN3X_MEMERR_CHECK)
#define SUN3X_MEMERR_LANE(a)    (0x8 >> ((a) & 3))
#define SUN3X_MEMERR_ADDR_MASK  0x3fffffff

static void sun3x_parity_overlay_update(Sun3xState *s)
{
    bool need = (s->memreg & SUN3X_MEMERR_TEST) ||
                g_hash_table_size(s->parity_poison);

    memory_region_set_enabled(&s->parity_overlay, need);
}

static uint64_t sun3x_memreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    /*
     * Control bits plus the latched error status.  Masking the control
     * bits matters on its own: the PROM's own NMI stub (0xfefe0566), which
     * the level-7 clock edge can still vector through early in boot,
     * writes 0xff/0xfe here, and reading that back verbatim used to make
     * the kernel see INTR + every error bit set and panic "unknown memory
     * error".
     */
    return (s->memreg & SUN3X_MEMERR_CTRL_BITS) | s->memerr_status;
}

static void sun3x_memreg_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    Sun3xState *s = opaque;

    /* any write clears the latched error status (INTR + lane bits) --
     * matches the PROM's own read-then-clear ack (0xfefe7fd2/0xfefe7fd8)
     * and mirrors hw/m68k/sun3.c's sun3_memerr_write(). */
    s->memreg = val & SUN3X_MEMERR_CTRL_BITS;
    s->memerr_status = 0;
    sun3x_parity_overlay_update(s);
}

static const MemoryRegionOps sun3x_memreg_ops = {
    .read = sun3x_memreg_read,
    .write = sun3x_memreg_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

static uint64_t sun3x_memerraddr_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    return s->memerr_addr;
}

static void sun3x_memerraddr_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned size)
{
    Sun3xState *s = opaque;

    /* the kernel's parity path acks with `andil #0xc0000000` here (a
     * longword read-modify-write) and never touches the control register,
     * so this write must release the latch too */
    s->memerr_addr = val;
    s->memerr_status = 0;
}

static const MemoryRegionOps sun3x_memerraddr_ops = {
    .read = sun3x_memerraddr_read,
    .write = sun3x_memerraddr_write,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Bad-parity RAM overlay (see the big comment above).  Reads/writes pass
 * through to real RAM; a read of a poisoned byte with CHECK set latches
 * the error and bus-errors the cycle; a write poisons or cleans the bytes
 * it covers depending on TEST.
 */
static MemTxResult sun3x_parity_read(void *opaque, hwaddr addr, uint64_t *val,
                                     unsigned size, MemTxAttrs attrs)
{
    Sun3xState *s = opaque;
    uint8_t buf[8];
    unsigned i;

    address_space_read(&s->ram_as, addr, attrs, buf, size);
    *val = ldn_be_p(buf, size);

    if (s->memreg & SUN3X_MEMERR_CHECK) {
        uint8_t bad = 0;
        hwaddr first = 0;

        for (i = 0; i < size; i++) {
            if (g_hash_table_contains(s->parity_poison,
                                      GUINT_TO_POINTER(addr + i))) {
                if (!bad) {
                    first = addr + i;
                }
                bad |= SUN3X_MEMERR_LANE(addr + i);
            }
        }
        if (bad) {
            s->memerr_status = SUN3X_MEMERR_INTR | (0xf & ~bad);
            s->memerr_addr = (s->memerr_addr & ~SUN3X_MEMERR_ADDR_MASK) |
                             (first & SUN3X_MEMERR_ADDR_MASK);
            s->buserr = SUN3X_BE_MEMERR;
            trace_sun3x_parity_error(first, s->memerr_status);
            return MEMTX_DECODE_ERROR;   /* -> 68030 bus error */
        }
    }
    return MEMTX_OK;
}

static MemTxResult sun3x_parity_write(void *opaque, hwaddr addr,
                                      uint64_t val, unsigned size,
                                      MemTxAttrs attrs)
{
    Sun3xState *s = opaque;
    uint8_t buf[8];
    unsigned i;

    stn_be_p(buf, size, val);
    address_space_write(&s->ram_as, addr, attrs, buf, size);

    for (i = 0; i < size; i++) {
        if (s->memreg & SUN3X_MEMERR_TEST) {
            g_hash_table_add(s->parity_poison, GUINT_TO_POINTER(addr + i));
        } else {
            g_hash_table_remove(s->parity_poison, GUINT_TO_POINTER(addr + i));
        }
    }
    if (!(s->memreg & SUN3X_MEMERR_TEST)) {
        sun3x_parity_overlay_update(s);
    }
    return MEMTX_OK;
}

static const MemoryRegionOps sun3x_parity_ops = {
    .read_with_attrs = sun3x_parity_read,
    .write_with_attrs = sun3x_parity_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/*
 * Background "bus timeout": any access that decodes to nothing (a nonexistent
 * on-board/VME address) latches the TIMEOUT bit in the bus error register and
 * terminates in a genuine 68030 bus error.  The PROM's Bus Error Register test
 * reads a nonexistent address (0x40000000) and then requires TIMEOUT (bit 5)
 * to be set; memory sizing and device probing likewise depend on the fault.
 * Installed at the lowest priority so RAM, the memhole, OBIO and the PROM
 * windows all take precedence.
 */
static MemTxResult sun3x_busfault_read(void *opaque, hwaddr addr,
                                       uint64_t *data, unsigned size,
                                       MemTxAttrs attrs)
{
    Sun3xState *s = opaque;

    s->buserr = SUN3X_BE_TIMEOUT;
    *data = 0;
    return MEMTX_DECODE_ERROR;   /* -> m68k bus error (access fault) */
}

static MemTxResult sun3x_busfault_write(void *opaque, hwaddr addr,
                                        uint64_t val, unsigned size,
                                        MemTxAttrs attrs)
{
    Sun3xState *s = opaque;

    s->buserr = SUN3X_BE_TIMEOUT;
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps sun3x_busfault_ops = {
    .read_with_attrs = sun3x_busfault_read,
    .write_with_attrs = sun3x_busfault_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- Printer / diagnostic port (0x6f00003c) ---
 *
 * A plain 8-bit read/write latch.  The PROM uses it three ways: as the
 * diagnostic LED register (it writes the running subtest number, e.g.
 * 0xfefe9c44 `moveb %d7,%d6; andib #63,%d6; moveb %d6,0x6f00003c`), as its
 * own one-bit scratch for the progress spinner's phase (`btst #6` /
 * `bclr #6` / `bset #6` at 0xfefe9e08-0xfefe9e2a) and as the subject of the
 * "Printer Controller Check" subtest (0xfefee2c4), which writes all 256
 * values in turn and requires each one to read back exactly:
 *
 *      d1 = 0
 *   loop:  moveb %d1,0x6f00003c ; delay ; moveb 0x6f00003c,%d0
 *          cmpb %d0,%d1 ; bne -> "Printer Controller Data Error Expected
 *                                 0x%d1, OBS 0x%d0"
 *          addqb #1,%d1 ; cmpib #0,%d1 ; bne loop
 *
 * It used to be swallowed by the broad OBIO absorb (write ignored, read 0),
 * so that subtest failed on its very first value (expected 0x1, observed
 * 0x0) and, per the ROM's `bclr #7,%d7` loop-on-error contract, wedged the
 * unattended self-test permanently -- it is the LAST subtest before
 * "Selftest Completed." and the autoboot.
 */
#define SUN3X_LPREG             0x6f00003c

static uint64_t sun3x_lpreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    return s->lpreg_val;
}

static void sun3x_lpreg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Sun3xState *s = opaque;

    s->lpreg_val = val;
}

static const MemoryRegionOps sun3x_lpreg_ops = {
    .read = sun3x_lpreg_read,
    .write = sun3x_lpreg_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- TOD clock: Mostek MK48T02, the NVRAM's last 8 bytes (SUN3X_TOD_OFF) ---
 *
 * Register layout (confirmed against the PROM's own TOD subtest, which writes
 * exactly these fields at 0xfefe769e-0xfefe76b0: year=0x89, month=1, date=1,
 * day=7): +0 control, +1 seconds, +2 minutes, +3 hours, +4 day-of-week,
 * +5 date, +6 month, +7 year, all BCD.
 *
 * Why this has to be a real, ticking clock and not a RAM snapshot: the PROM's
 * TOD subtest (0xfefe75f8) reads the seconds byte and then polls up to 65536
 * times for it to CHANGE, and reports "Oscilator is not running!" if it never
 * does.  A snapshot passes that check only by luck -- its first comparison is
 * against (seconds & 0x80), which differs from the live byte whenever seconds
 * is nonzero -- so the subtest passed on most boots and hung on the rest.  It
 * is worse than a coin flip, because the PROM's own recovery path
 * (0xfefe765a) re-initialises the part with seconds = 0, which makes the very
 * next comparison match and the loop permanent; and a failing subtest is
 * retried forever (see sun3x_update_irq()'s notes and the `bclr #7,%d7`
 * contract documented in sun3x-NOTES.md).
 *
 * Control register: bit 7 WRITE (freeze and accept a new time; the written
 * fields take effect when it is cleared), bit 6 READ (freeze the counters so
 * a multi-byte read is consistent).  Seconds bit 7 is the MK48T02 ST (stop)
 * bit: the PROM tests it to recognise a fresh, never-started part.  Time is
 * derived from the host clock (qemu_get_timedate, like every other QEMU RTC)
 * plus an offset the guest can set, so it advances at exactly 1 Hz in the
 * clock QEMU is configured with and SunOS's own inittodr() reads a correct
 * current date.
 */
#define SUN3X_TOD_CTRL_WRITE    0x80
#define SUN3X_TOD_CTRL_READ     0x40
#define SUN3X_TOD_SEC_STOP      0x80

#define TOD_BCD(x)              ((((x) / 10) << 4) | ((x) % 10))
#define TOD_BIN(x)              ((((x) >> 4) & 0xf) * 10 + ((x) & 0xf))

static void sun3x_tod_latch(Sun3xState *s)
{
    struct tm tm;

    qemu_get_timedate(&tm, s->tod_offset);
    s->tod_regs[1] = TOD_BCD(tm.tm_sec);
    s->tod_regs[2] = TOD_BCD(tm.tm_min);
    s->tod_regs[3] = TOD_BCD(tm.tm_hour);
    s->tod_regs[4] = TOD_BCD(tm.tm_wday + 1);   /* MK48T02 day = 1..7 */
    s->tod_regs[5] = TOD_BCD(tm.tm_mday);
    s->tod_regs[6] = TOD_BCD(tm.tm_mon + 1);
    s->tod_regs[7] = TOD_BCD(tm.tm_year % 100);
}

static void sun3x_tod_commit(Sun3xState *s)
{
    struct tm tm = { 0 };
    int year = TOD_BIN(s->tod_regs[7]);

    s->tod_stopped = (s->tod_regs[1] & SUN3X_TOD_SEC_STOP) != 0;
    tm.tm_sec = TOD_BIN(s->tod_regs[1] & 0x7f);
    tm.tm_min = TOD_BIN(s->tod_regs[2] & 0x7f);
    tm.tm_hour = TOD_BIN(s->tod_regs[3] & 0x3f);
    tm.tm_mday = TOD_BIN(s->tod_regs[5] & 0x3f);
    tm.tm_mon = TOD_BIN(s->tod_regs[6] & 0x1f) - 1;
    /* two-digit year: this hardware's own era starts in the 1970s */
    tm.tm_year = year + (year >= 70 ? 0 : 100);
    if (tm.tm_mday < 1) {
        tm.tm_mday = 1;
    }
    if (tm.tm_mon < 0 || tm.tm_mon > 11) {
        tm.tm_mon = 0;
    }
    s->tod_offset = qemu_timedate_diff(&tm);
}

static uint64_t sun3x_tod_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3xState *s = opaque;

    if (addr == 0) {
        return s->tod_ctrl;
    }
    if (!(s->tod_ctrl & (SUN3X_TOD_CTRL_READ | SUN3X_TOD_CTRL_WRITE)) &&
        !s->tod_stopped) {
        sun3x_tod_latch(s);
    }
    return s->tod_regs[addr & 7];
}

static void sun3x_tod_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    Sun3xState *s = opaque;
    uint8_t old = s->tod_ctrl;

    if (addr != 0) {
        s->tod_regs[addr & 7] = val;
        if (!(s->tod_ctrl & SUN3X_TOD_CTRL_WRITE)) {
            /* a write without the WRITE bit set still has to stick */
            sun3x_tod_commit(s);
        }
        return;
    }

    s->tod_ctrl = val & (SUN3X_TOD_CTRL_WRITE | SUN3X_TOD_CTRL_READ);
    if ((s->tod_ctrl & (SUN3X_TOD_CTRL_WRITE | SUN3X_TOD_CTRL_READ)) &&
        !(old & (SUN3X_TOD_CTRL_WRITE | SUN3X_TOD_CTRL_READ))) {
        /* entering freeze: latch the current time so partial updates keep
         * the fields the guest does not rewrite */
        sun3x_tod_latch(s);
    } else if ((old & SUN3X_TOD_CTRL_WRITE) &&
               !(s->tod_ctrl & SUN3X_TOD_CTRL_WRITE)) {
        sun3x_tod_commit(s);
    }
}

static const MemoryRegionOps sun3x_tod_ops = {
    .read = sun3x_tod_read,
    .write = sun3x_tod_write,
    .valid = { .min_access_size = 1, .max_access_size = 1 },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- IDPROM + Mostek clock (in the NVRAM) --- */

static void sun3x_build_nvram(Sun3xState *s)
{
    uint8_t *p = memory_region_get_ram_ptr(&s->eeprom);
    uint8_t *id = p + SUN3X_IDPROM_OFF;
    /* The genuine 3/80 clock is the Mostek MK48T02 in the NVRAM's last 8
     * bytes (SUN3X_TOD_OFF, OBIO_CLOCK2), modelled as a real ticking device
     * in SunOS mode (see sun3x_tod_ops).  The Linux shim instead reads a
     * plain BCD snapshot at +0xf8, which is all its read-only hwclk needs. */
    uint8_t *clk = p + SUN3X_CLOCK_OFF;
    struct tm tm;
    uint8_t sum = 0;
    int i;

    memset(p, 0, SUN3X_EEPROM_SIZE);

    /*
     * EEPROM configuration bytes (SunOS mode).  Byte 0x1f selects the console
     * device; force ttya so the genuine monitor talks to the zs serial port
     * and does not wait on an (absent) on-board framebuffer + keyboard.
     */
    if (s->sunos) {
        p[0x1f] = 0x10;         /* EE_CONS_TTYA */
    }

    /* idprom */
    id[0] = 0x01;               /* id_format */
    id[1] = SUN3X_MACHTYPE;     /* Sun 3/80 */
    id[2] = 0x08; id[3] = 0x00; id[4] = 0x20;   /* Sun OUI ethaddr */
    id[5] = 0x11; id[6] = 0x22; id[7] = 0x33;
    /* bytes 8..14 date/serial left plausible (zero); byte15 = checksum */
    for (i = 0; i <= 0x0e; i++) {
        sum ^= id[i];
    }
    id[0x0f] = sum;

    /* Linux shim's read-only BCD snapshot: csr, sec, min, hour, wday, mday,
     * month, year.  SunOS mode uses the real TOD device instead. */
    if (s->sunos) {
        return;
    }
    qemu_get_timedate(&tm, 0);
#define BCD(x) ((((x) / 10) << 4) | ((x) % 10))
    clk[0] = 0;                         /* csr */
    clk[1] = BCD(tm.tm_sec);
    clk[2] = BCD(tm.tm_min);
    clk[3] = BCD(tm.tm_hour);
    clk[4] = BCD(tm.tm_wday);
    clk[5] = BCD(tm.tm_mday);
    clk[6] = BCD(tm.tm_mon + 1);
    clk[7] = BCD(tm.tm_year % 100);
#undef BCD
}

/* --- the synthesised boot PROM --- */

/*
 * Hand-assembled m68k stubs (big-endian).  They drive channel A of the Z8530
 * at 0x62000000 (it_shift==1: ch-A CTRL @ +4, ch-A DATA @ +6).  The minimal
 * PROM does no zs init, and the ESCC drops TX unless WR5.TXEN and blocks RX
 * unless WR3.RXEN, so each routine (re)enables its direction first.
 */
static const uint8_t sun3x_putchar_code[] = {
    /* pv_putchar/pv_nbputchar: char = low byte of arg long at 4(sp) */
    0x13, 0xfc, 0x00, 0x05, 0x62, 0x00, 0x00, 0x04, /* move.b #5,0x62000004  (sel WR5) */
    0x13, 0xfc, 0x00, 0x68, 0x62, 0x00, 0x00, 0x04, /* move.b #0x68,..       (TXEN|Tx8) */
    0x12, 0x39, 0x62, 0x00, 0x00, 0x04,             /* poll: move.b 0x62000004,d1 (RR0) */
    0x08, 0x01, 0x00, 0x02,                         /* btst #2,d1            (TXEMPTY) */
    0x67, 0xf4,                                     /* beq.s poll */
    0x12, 0x2f, 0x00, 0x07,                         /* move.b (7,sp),d1      (char) */
    0x13, 0xc1, 0x62, 0x00, 0x00, 0x06,             /* move.b d1,0x62000006  (DATA) */
    0x70, 0x00,                                     /* moveq #0,d0 */
    0x4e, 0x75,                                     /* rts */
};

static const uint8_t sun3x_getchar_code[] = {
    /* pv_getchar/pv_nbgetchar: return char in d0, or -1 if none */
    0x13, 0xfc, 0x00, 0x03, 0x62, 0x00, 0x00, 0x04, /* move.b #3,0x62000004  (sel WR3) */
    0x13, 0xfc, 0x00, 0xc1, 0x62, 0x00, 0x00, 0x04, /* move.b #0xc1,..       (RXEN|Rx8) */
    0x12, 0x39, 0x62, 0x00, 0x00, 0x04,             /* move.b 0x62000004,d1  (RR0) */
    0x08, 0x01, 0x00, 0x00,                         /* btst #0,d1            (RXAV) */
    0x66, 0x04,                                     /* bne.s haschar */
    0x70, 0xff,                                     /* moveq #-1,d0 */
    0x4e, 0x75,                                     /* rts */
    0x10, 0x39, 0x62, 0x00, 0x00, 0x06,             /* haschar: move.b 0x62000006,d0 */
    0x02, 0x80, 0x00, 0x00, 0x00, 0xff,             /* andi.l #0xff,d0 */
    0x4e, 0x75,                                     /* rts */
};

static const uint8_t sun3x_halt_code[] = {
    0x60, 0xfe,                                     /* bra.s . */
};

static void sun3x_build_prom(Sun3xState *s)
{
    uint8_t *p = memory_region_get_ram_ptr(&s->prom);
    uint32_t i;

    memset(p, 0, SUN3X_PROM_SIZE);

    /* romvec function pointers */
    stl_be_p(p + OFF(ROMVEC_VA) + RV_GETCHAR,   GETCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_PUTCHAR,   PUTCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_NBGETCHAR, GETCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_NBPUTCHAR, PUTCHAR_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_MONID,     MONID_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_REBOOT,    HALT_VA);
    stl_be_p(p + OFF(ROMVEC_VA) + RV_ABORT,     HALT_VA);

    /* head.S: a1 = *(0xfefe00d4); a1 = *a1; copy 256 descriptors from a1 */
    stl_be_p(p + OFF(D4_VA),  PTP_VA);
    stl_be_p(p + OFF(PTP_VA), PGTABLE_VA);

    /* identity page table for the 0xfee00000..0xff000000 window (8K pages) */
    for (i = 0; i < 256; i++) {
        stl_be_p(p + OFF(PGTABLE_VA) + i * 4,
                 (SUN3X_PROM_PHYS + i * 0x2000) | SUN3X_PROM_PTE_FLAGS);
    }

    /* stub routines + monitor id string */
    memcpy(p + OFF(PUTCHAR_VA), sun3x_putchar_code, sizeof(sun3x_putchar_code));
    memcpy(p + OFF(GETCHAR_VA), sun3x_getchar_code, sizeof(sun3x_getchar_code));
    memcpy(p + OFF(HALT_VA),    sun3x_halt_code,    sizeof(sun3x_halt_code));
    strcpy((char *)(p + OFF(MONID_VA)), "QEMU sun3x");
}

/* --- boot --- */

static void sun3x_cpu_reset(void *opaque)
{
    Sun3xState *s = opaque;
    CPUM68KState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->reset_pc;
    env->aregs[7] = s->reset_sp;
    env->sp[env->current_sp] = s->reset_sp;
}

static void sun3x_load_kernel(Sun3xState *s, MachineState *machine)
{
    CPUState *cs = CPU(s->cpu);
    uint64_t elf_entry, high;
    ssize_t kernel_size;
    hwaddr parameters_base;
    hwaddr top_of_ram = SUN3X_RAM_BASE + machine->ram_size;
    void *param_blob, *param_ptr;

    /* PAGE_OFFSET==0: kernel VA==PA, RAM at phys 0, so no bias needed. */
    kernel_size = load_elf(machine->kernel_filename, NULL, NULL, NULL,
                           &elf_entry, NULL, &high, NULL, ELFDATA2MSB,
                           EM_68K, 0, 0);
    if (kernel_size < 0) {
        error_report("could not load kernel '%s'", machine->kernel_filename);
        exit(1);
    }
    s->reset_pc = elf_entry;

    parameters_base = (high + 1) & ~1;
    param_blob = g_malloc(machine->kernel_cmdline ?
                          strlen(machine->kernel_cmdline) + 1024 : 1024);
    param_ptr = param_blob;

    BOOTINFO1(param_ptr, BI_MACHTYPE, MACH_SUN3X);
    BOOTINFO1(param_ptr, BI_MMUTYPE, MMU_68030);
    BOOTINFO1(param_ptr, BI_CPUTYPE, CPU_68030);
    BOOTINFO2(param_ptr, BI_MEMCHUNK, SUN3X_RAM_BASE, machine->ram_size);

    if (machine->kernel_cmdline) {
        BOOTINFOSTR(param_ptr, BI_COMMAND_LINE, machine->kernel_cmdline);
    }

    if (machine->initrd_filename) {
        int64_t initrd_size = get_image_size(machine->initrd_filename, NULL);
        hwaddr initrd_base;

        if (initrd_size < 0) {
            error_report("could not load initial ram disk '%s'",
                         machine->initrd_filename);
            exit(1);
        }
        initrd_base = (top_of_ram - initrd_size) & TARGET_PAGE_MASK;
        load_image_targphys(machine->initrd_filename, initrd_base,
                            top_of_ram - initrd_base, &error_fatal);
        BOOTINFO2(param_ptr, BI_RAMDISK, initrd_base, initrd_size);
        top_of_ram = initrd_base;
    }

    BOOTINFO0(param_ptr, BI_LAST);
    rom_add_blob_fixed_as("bootinfo", param_blob, param_ptr - param_blob,
                          parameters_base, cs->as);
    g_free(param_blob);

    s->reset_sp = (top_of_ram - 0x1000) & ~3;
}

static void sun3x_init(MachineState *machine)
{
    Sun3xState *s = g_new0(Sun3xState, 1);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *dev;
    SysBusDevice *sbd;

    if (machine->ram_size > SUN3X_RAM_MAX) {
        error_report("sun3x supports at most %d MiB of RAM",
                     (int)(SUN3X_RAM_MAX / MiB));
        exit(1);
    }

    s->sunos = machine->firmware != NULL;

    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));

    /* autovector interrupt controller */
    s->irqc = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(s->irqc), "m68k-cpu", OBJECT(s->cpu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->irqc), &error_fatal);

    /*
     * Real hardware wire-ORs (open-drain) both Z8530s' four IRQ outputs
     * (2 chips x channel A/B) onto the single shared level-6 autovector
     * line.  Connecting all four sysbus IRQ outputs directly to the same
     * m68k_irqc gpio input would be "last writer wins" -- whichever
     * channel last called qemu_set_irq() clobbers the others' state, so
     * e.g. channel A dropping its request after being serviced can drop
     * the level even while channel B (which just raised DCD, or has a
     * pending ext/status change) is still asserting, or vice versa.  That
     * produces exactly the "held autovector line retaken at every splx()"
     * storm signature seen booting SunOS.  Use a proper OR gate instead,
     * same as q800.c does for its own (single-chip, 2-line) ESCC.
     *
     * (Retargeting this group to level 3 -- the kernel's own autoconfig
     * table prints "zs0/zs1/le0 ... pri 3" -- double-faults right after
     * "dump on"; level 6 is what this kernel actually services.)
     */
    s->zs_orgate = qdev_new(TYPE_OR_IRQ);
    qdev_prop_set_uint32(s->zs_orgate, "num-lines", 4);
    qdev_realize_and_unref(s->zs_orgate, NULL, &error_fatal);
    qdev_connect_gpio_out(s->zs_orgate, 0, qdev_get_gpio_in(s->irqc, 6 - 1));

    /*
     * Same open-drain-style OR wiring for the shared level-3 autovector
     * line: line 0 is the "le" LANCE ethernet IRQ (SunOS mode only, wired
     * below), line 1 is the software interrupt-3 bit (see
     * sun3x_update_irq()).  Created unconditionally, like s->zs_orgate,
     * even though only the softint input is ever driven in Linux mode.
     */
    s->lance_orgate = qdev_new(TYPE_OR_IRQ);
    qdev_prop_set_uint32(s->lance_orgate, "num-lines", 2);
    qdev_realize_and_unref(s->lance_orgate, NULL, &error_fatal);
    qdev_connect_gpio_out(s->lance_orgate, 0, qdev_get_gpio_in(s->irqc, 3 - 1));

    /*
     * ...and for the shared level-2 autovector line: line 0 is the "si"
     * NCR5380 SCSI controller's IRQ (SunOS mode only, wired below), line 1
     * is the software interrupt-2 bit.  See the comment on
     * sun3x_si_update_irq() for the completion-losing bug this fixes.
     */
    s->si_orgate = qdev_new(TYPE_OR_IRQ);
    qdev_prop_set_uint32(s->si_orgate, "num-lines", 2);
    qdev_realize_and_unref(s->si_orgate, NULL, &error_fatal);
    qdev_connect_gpio_out(s->si_orgate, 0, qdev_get_gpio_in(s->irqc, 2 - 1));

    /*
     * Linux shim mode: zero/absorb background over the whole space (the Linux
     * sun3x port does no bus-error probing; RAM comes from BI_MEMCHUNK and
     * devices are at fixed addrs).  SunOS mode deliberately omits this so that
     * unmapped accesses raise genuine 68030 bus errors, which the real PROM
     * self-test uses to size memory and probe for optional devices.
     */
    if (!s->sunos) {
        memory_region_init_io(&s->bg, NULL, &sun3x_absorb_ops, s,
                              "sun3x.bg", UINT64_MAX);
        memory_region_add_subregion_overlap(sysmem, 0, &s->bg, -1);
    }

    /* main RAM */
    memory_region_add_subregion(sysmem, SUN3X_RAM_BASE, machine->ram);

    if (s->sunos) {
        /*
         * Bad-parity RAM overlay for the PROM's parity subtests and the
         * kernel's parity trap path (see sun3x_parity_read()).  Mirrors
         * hw/m68k/sun3.c's own parity_overlay mechanism: an MMIO overlay
         * shadows RAM, enabled only while the memory-error CSR's TEST bit
         * is set or poisoned bytes remain, so ordinary boot/kernel RAM
         * traffic never pays for it.
         */
        s->parity_poison = g_hash_table_new(NULL, NULL);
        address_space_init(&s->ram_as, machine->ram, "sun3x.ram-as");
        memory_region_init_io(&s->parity_overlay, NULL, &sun3x_parity_ops, s,
                              "sun3x.parity-overlay", machine->ram_size);
        memory_region_set_enabled(&s->parity_overlay, false);
        memory_region_add_subregion_overlap(sysmem, SUN3X_RAM_BASE,
                                            &s->parity_overlay, 1);
    }

    /* DVMA IOMMU page table (scratch RAM) */
    memory_region_init_ram(&s->iommu, NULL, "sun3x.iommu",
                           SUN3X_IOMMU_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SUN3X_IOMMU, &s->iommu);

    /* enable/copro/LED block, with the interrupt register overlaid */
    memory_region_init_io(&s->enareg, NULL, &sun3x_absorb_ops, s,
                          "sun3x.enareg", SUN3X_ENA_SIZE);
    memory_region_add_subregion(sysmem, SUN3X_ENAREG, &s->enareg);
    memory_region_init_io(&s->intreg_mr, NULL, &sun3x_intreg_ops, s,
                          "sun3x.intreg", 1);
    memory_region_add_subregion_overlap(sysmem, SUN3X_INTREG,
                                        &s->intreg_mr, 1);

    /* NVRAM: idprom + Mostek clock */
    memory_region_init_ram(&s->eeprom, NULL, "sun3x.eeprom",
                           SUN3X_EEPROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SUN3X_EEPROM, &s->eeprom);
    sun3x_build_nvram(s);

    /*
     * Console Z8530.  In the Linux shim the console is ZS1 (0x62000000, driven
     * by the synthesised PROM stubs).  The genuine Sun 3/80 PROM instead uses
     * the higher-addressed zs (0x62002000) as ttya/ttyb, exactly like the 3/60
     * (hw/m68k/sun3.c) — so in SunOS mode wire serial_hd(0/1) there and leave
     * ZS1 (keyboard/mouse) idle.
     */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", SUN3X_ZS_FREQ);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnAtype", escc_serial);
    qdev_prop_set_uint32(dev, "chnBtype", escc_serial);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, s->sunos ? SUN3X_ZS2 : SUN3X_ZS1);
    /* zs interrupt is autovector level 6 on the Sun-3; the genuine monitor
     * and SunOS drive an interrupt-driven console through it.  Both channel
     * outputs are wire-ORed onto the shared line via s->zs_orgate (see the
     * comment where it is created) rather than connected directly to the
     * same irqc input. */
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->zs_orgate, 0));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(s->zs_orgate, 1));
    /*
     * Carrier is hardwired on the Sun-3 serial ports, exactly like the 3/60
     * (hw/m68k/sun3.c); without it, the SunOS console open() sleeps in the
     * carrier wait forever once init tries to open /dev/console as the
     * controlling tty (the chardev backend, e.g. a stdio pipe, has no real
     * modem-control lines to assert DCD itself).
     */
    qemu_irq_raise(qdev_get_gpio_in_named(dev, "dcd", 0));
    qemu_irq_raise(qdev_get_gpio_in_named(dev, "dcd", 1));

    /* the other Z8530 (keyboard/mouse in SunOS mode), left idle */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", SUN3X_ZS_FREQ);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_chr(dev, "chrA", NULL);
    qdev_prop_set_chr(dev, "chrB", NULL);
    qdev_prop_set_uint32(dev, "chnAtype", escc_serial);
    qdev_prop_set_uint32(dev, "chnBtype", escc_serial);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, s->sunos ? SUN3X_ZS1 : SUN3X_ZS2);
    /*
     * This chip is otherwise idle (no chardev backend) but its two IRQ
     * outputs are real, wired hardware outputs on the shared level-6 line
     * exactly like the console chip's -- left unconnected, the kernel could
     * never see/ack anything from this chip's slot.  Wire them into the
     * same OR gate.
     */
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->zs_orgate, 2));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(s->zs_orgate, 3));

    if (!s->sunos) {
        /* synthesised boot PROM window (identity mapped) */
        memory_region_init_ram(&s->prom, NULL, "sun3x.prom",
                               SUN3X_PROM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROM_PHYS, &s->prom);
        sun3x_build_prom(s);
    } else {
        /* genuine Sun 3/80 boot PROM (128 KiB ROM) at 0xfefe0000, with its
         * private scratch/stack/VBR RAM in the 0xfef00000 window below it. */
        char *filename;
        int rsize;

        memory_region_init_ram(&s->promram, NULL, "sun3x.promram",
                               SUN3X_PROMRAM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROMRAM_BASE, &s->promram);

        memory_region_init_rom(&s->prom, NULL, "sun3x.prom",
                               SUN3X_ROM_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROM_BASE, &s->prom);

        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
        rsize = filename ? load_image_size(filename,
                    memory_region_get_ram_ptr(&s->prom), SUN3X_ROM_SIZE) : -1;
        if (rsize != SUN3X_ROM_SIZE) {
            error_report("sun3x: cannot load %d-byte boot PROM '%s'",
                         SUN3X_ROM_SIZE, machine->firmware);
            exit(1);
        }
        g_free(filename);

        /*
         * The PROM is also decoded low, at 0x63000000, but NOT as a read-only
         * alias: this is the monitor's relocation window.  The cold-start reads
         * its exception vectors / TC sentinel here (so it is shadow-initialised
         * with the ROM image), then the monitor copies itself into this RAM and
         * runs from it (patching e.g. the level-5 vector to its clock handler),
         * jumping well past the first 128 KiB.  Model it as writable RAM
         * preloaded with the ROM so the self-copy sticks and execution beyond
         * 128 KiB lands on real code instead of a bus-timeout NOP-slide.
         */
        memory_region_init_ram(&s->prom_alias, NULL, "sun3x.prom.shadow",
                               SUN3X_PROM_ALIAS_SIZE, &error_fatal);
        memory_region_add_subregion(sysmem, SUN3X_PROM_ALIAS, &s->prom_alias);
        memcpy(memory_region_get_ram_ptr(&s->prom_alias),
               memory_region_get_ram_ptr(&s->prom), SUN3X_ROM_SIZE);

        /*
         * Low-priority absorb over the whole OBIO (Type-1) device window
         * (0x58000000..0x7bffffff, see NetBSD sun3x obio.h).  The PROM
         * self-test pokes many on-board device registers (LANCE ethernet at
         * 0x65002000, EMULEX SCSI+DMA at 0x66000000/0x66001000, the FDC at
         * 0x6e000000, the printer port at 0x6f00003c, cache-tag RAMs, ...)
         * before it installs its own bus-error probe handler, so an unmapped
         * access here would be a fatal early bus error.  Absorbed (read 0 /
         * write ignored); the real modelled registers (iommu, enable/diag/
         * memerr/interrupt, zs, eeprom) overlay this at higher priority.  Main
         * memory (sized low) and the OBMEM framebuffer stay unmapped, so RAM
         * sizing still bus-errors correctly and the PROM finds no framebuffer
         * and selects the ttya serial console.
         */
        memory_region_init_io(&s->obio, NULL, &sun3x_absorb_ops, s,
                              "sun3x.obio", 0x24000000);
        memory_region_add_subregion_overlap(sysmem, 0x58000000,
                                             &s->obio, -1);

        /*
         * 82072 floppy controller (obio 0x6e000000), overlaid on the OBIO
         * absorb.  Models an empty controller (no drive) so the SunOS kernel's
         * fd probe concludes "drive absent" via the NEC handshake instead of
         * timing out on a dead MSR and crashing its timeout/reset handler.
         */
        s->fdc_st0 = 0x80;
        memory_region_init_io(&s->fdc, NULL, &sun3x_fdc_ops, s,
                              "sun3x.fdc", 0x1000);
        memory_region_add_subregion_overlap(sysmem, 0x6e000000, &s->fdc, 1);

        /* real bus error register (overlaid on the enareg absorb block) */
        memory_region_init_io(&s->buserr_mr, NULL, &sun3x_buserr_ops, s,
                              "sun3x.buserr", 1);
        memory_region_add_subregion_overlap(sysmem, SUN3X_BUSERR,
                                            &s->buserr_mr, 1);

        /* memory error/control register (overlaid on the enareg absorb):
         * an 8-bit port mirrored across the longword, see sun3x_memreg_ops */
        memory_region_init_io(&s->memreg_mr, NULL, &sun3x_memreg_ops, s,
                              "sun3x.memreg", 4);
        memory_region_add_subregion_overlap(sysmem, 0x61001000,
                                            &s->memreg_mr, 1);

        /* memory error address register (overlaid on the enareg absorb) */
        memory_region_init_io(&s->memerraddr_mr, NULL, &sun3x_memerraddr_ops,
                              s, "sun3x.memerraddr", 4);
        memory_region_add_subregion_overlap(sysmem, 0x61001004,
                                            &s->memerraddr_mr, 1);

        /*
         * Bus-error hole at 0x61000c00 (overlaid on the enareg absorb).  Both
         * the standalone boot's and the kernel's IDPROM readers probe the
         * *sun3* discrete IDPROM location before falling back to the real
         * *sun3x* IDPROM (MK48T02 in the EEPROM, format=1, machtype=0x42):
         * the standalone loader (ufsboot 0x211a68) through the monitor's
         * virtual 0xfef1cc00, and the kernel (idprom_fetch, text 0xf8061842,
         * bus-error-guarded peek at 0xf8059d92) through its own virtual
         * 0xfedf8c00.  Both virtual addresses have offset 0xc00 within their
         * (8 KiB) page, and -- now that get_physical_address_030() discards
         * the low, sub-page bits of a page descriptor's frame field per
         * MC68030 UM 9.1.2 instead of adding them in -- both resolve to the
         * SAME physical address, 0x61000c00, regardless of whatever tag each
         * OS's own page table happened to carry in those now-ignored bits.
         * (Before that fix, the monitor's probe read raw+tag = 0x61001800,
         * one page table's software tag away from the kernel's; this file
         * used to model two separate holes, one per double-counted address.)
         * On a real 3/80 the sun3 location is simply absent and bus-errors,
         * forcing both readers to their sun3x fallback; our broad enareg
         * absorb was ACKing it (read 0) instead, so the standalone loader
         * saw "bad idprom format (0 s.b. 1)" and the kernel defaulted the
         * machine type to SUN3X_470 and double-faulted.  0x61000c00 sits in
         * the free gap between DIAGREG (+0x800) and MEMREG (+0x1000); the
         * real control registers are elsewhere in the block, and the PROM
         * self-test never touches this address (verified).
         */
        memory_region_init_io(&s->idprobe_hole, NULL, &sun3x_busfault_ops, s,
                              "sun3x.idprobe-hole", 0x400);
        memory_region_add_subregion_overlap(sysmem, 0x61000c00,
                                            &s->idprobe_hole, 2);

        /*
         * Level-7 clock interrupt status register (overlaid on the EEPROM/
         * clock RAM), read by the monitor's clock handler through virtual
         * 0xfef06010.  That VA's offset within its page is 0x010, which is
         * also the offset of SUN3X_CLK_STAT from SUN3X_EEPROM, so the
         * corrected walk lands here regardless of the descriptor's own tag.
         * (There is no separate "ack" device: see sun3x_intreg_write().)
         */
        memory_region_init_io(&s->clkstat, NULL, &sun3x_clkstat_ops, s,
                              "sun3x.clkstat", 1);
        memory_region_add_subregion_overlap(sysmem, SUN3X_CLK_STAT,
                                            &s->clkstat, 1);

        /* printer/diagnostic port: a real read/write latch (see above) */
        memory_region_init_io(&s->lpreg, NULL, &sun3x_lpreg_ops, s,
                              "sun3x.lpreg", 1);
        memory_region_add_subregion_overlap(sysmem, SUN3X_LPREG,
                                            &s->lpreg, 1);

        /*
         * The genuine TOD clock (MK48T02) in the NVRAM's last 8 bytes,
         * overlaid on the EEPROM RAM.  A real ticking part: the PROM's TOD
         * subtest polls the seconds register for a change and otherwise
         * reports "Oscilator is not running!" and then wedges (see
         * sun3x_tod_ops for the full contract).
         */
        memory_region_init_io(&s->tod, NULL, &sun3x_tod_ops, s,
                              "sun3x.tod", 8);
        memory_region_add_subregion_overlap(sysmem,
                                            SUN3X_EEPROM + SUN3X_TOD_OFF,
                                            &s->tod, 1);

        /*
         * Same clock-chip window, address-decode alias reached by the
         * kernel's own page tables (see SUN3X_CLK_STAT2 above).
         */
        memory_region_init_io(&s->clkstat2, NULL, &sun3x_clkstat_ops, s,
                              "sun3x.clkstat2", 1);
        memory_region_add_subregion_overlap(sysmem, SUN3X_CLK_STAT2,
                                            &s->clkstat2, 1);

        /*
         * "si" SCSI: an NCR5380 (OBIO_EMULEX_SCSI @ 0x66000000) plus a DMA/CSR
         * block in the same 8 KiB window.  The 5380's 8 registers are spaced
         * 4 bytes apart (reg-shift 2) and occupy 0x66000000..0x6600001f; the
         * si DMA/control registers overlay the rest at lower priority.
         * Interrupt is autovector level 2 (ipl 2).
         */
        object_initialize_child(OBJECT(machine), "si", &s->scsi, TYPE_NCR5380);
        qdev_prop_set_uint8(DEVICE(&s->scsi), "reg-shift", 2);
        qdev_prop_set_bit(DEVICE(&s->scsi), "sun-mode", true);
        sysbus_realize(SYS_BUS_DEVICE(&s->scsi), &error_fatal);
        memory_region_init_io(&s->sireg, NULL, &sun3x_sireg_ops, s,
                              "sun3x.si", 0x2000);
        memory_region_add_subregion_overlap(sysmem, 0x66000000, &s->sireg, 1);
        /* NCR5380 reg0 (data) is at 0x66000008; regs 0..7 are 4 bytes apart. */
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->scsi), 0, 0x66000008, 2);
        /* reclaim si scratch regs +0x20/+0x24 that fall inside the 5380 win */
        memory_region_init_io(&s->siscratch, NULL, &sun3x_siscratch_ops, s,
                              "sun3x.si.scratch", 0x08);
        memory_region_add_subregion_overlap(sysmem, 0x66000020,
                                            &s->siscratch, 3);
        /*
         * Routed through sun3x_si_irq()/sun3x_si_update_irq() rather than a
         * direct sysbus_connect_irq() 1:1 wire purely so the raw level is
         * tracked in si_irq_raw for bookkeeping; see the comment above
         * sun3x_si_update_irq() for why it is forwarded unconditionally.
         */
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->scsi), 0,
                           qemu_allocate_irq(sun3x_si_irq, s, 0));

        /* attach -drive if=scsi disks to the si bus */
        scsi_bus_legacy_handle_cmdline(&s->scsi.bus);

        /*
         * "le" ethernet: an AMD Am7990 LANCE at obio 0x65002000 (autoconfig
         * prints "le0 at obio 0x65002000 pri 3"), reusing the generic
         * hw/net/lance.c SysBus wrapper already proven on sparc/sun4m's own
         * on-board "le".  RDP/RAP sit at the model's usual +0/+2 16-bit
         * register offsets, matching drivers/net/ethernet/amd/sun3lance.c's
         * `lp->iobase[0]`/`[1]` (DREG/AREG).  DMA goes through the sun3x
         * IOMMU/DVMA translator above instead of sparc's ledma, via the
         * sun3x_lance_dma_read/write() hooks installed on the realized
         * device's phys_mem_read/phys_mem_write (the same technique used for
         * the ELTEC E17's ILACC, which likewise overrides them directly
         * rather than plumbing a new DMA-opaque device through lance.c).
         */
        {
            DeviceState *lance_dev = qdev_new(TYPE_LANCE);
            SysBusPCNetState *lance = SYSBUS_PCNET(lance_dev);
            NICInfo *nd = qemu_find_nic_info(TYPE_LANCE, true, NULL);
            static const MACAddr idprom_mac = {
                .a = { 0x08, 0x00, 0x20, 0x11, 0x22, 0x33 }
            };

            if (nd) {
                qdev_set_nic_properties(lance_dev, nd);
            }
            /*
             * SunOS reads the ethernet address from the IDPROM (see
             * sun3x_build_nvram() above), not from the chip, and the
             * sun3lance driver programs that same IDPROM address into the
             * LANCE's init block itself -- so force the qdev "mac" property
             * (which otherwise only supplies pcnet's unused emulated address
             * PROM) to match the machine's fixed IDPROM address instead of
             * whatever default/user-specified address qemu_find_nic_info()
             * came back with, keeping the two consistent as the task asks.
             */
            qdev_prop_set_macaddr(lance_dev, "mac", idprom_mac.a);

            sysbus_realize_and_unref(SYS_BUS_DEVICE(lance_dev), &error_fatal);
            lance->state.phys_mem_read = sun3x_lance_dma_read;
            lance->state.phys_mem_write = sun3x_lance_dma_write;
            sysbus_mmio_map_overlap(SYS_BUS_DEVICE(lance_dev), 0,
                                    0x65002000, 1);
            sysbus_connect_irq(SYS_BUS_DEVICE(lance_dev), 0,
                               qdev_get_gpio_in(s->lance_orgate, 0));
        }

        /* lowest-priority bus-timeout background: any otherwise-unmapped
         * access latches TIMEOUT and faults (the PROM probes 0x40000000). */
        memory_region_init_io(&s->busfault, NULL, &sun3x_busfault_ops, s,
                              "sun3x.busfault", UINT64_MAX);
        memory_region_add_subregion_overlap(sysmem, 0, &s->busfault, -2);

        /*
         * Absent memory banks.  The PROM sizes RAM by probing the four 16 MiB
         * banks (0/16/32/48 MiB, up to 64 MiB) with a write/read-back compare,
         * NOT via bus errors (its vector-2 handler stays the fatal one): the
         * memory controller ACKs the whole low space and absent banks simply
         * read back a value that mismatches the (non-zero) bank base the PROM
         * wrote.  Back the space above installed RAM with a zero-absorb region
         * so those probes mismatch (bank absent) instead of faulting.
         */
        if (machine->ram_size < SUN3X_RAM_MAX) {
            memory_region_init_io(&s->memhole, NULL, &sun3x_memhole_ops, s,
                                  "sun3x.memhole",
                                  SUN3X_RAM_MAX - machine->ram_size);
            memory_region_add_subregion(sysmem, machine->ram_size,
                                        &s->memhole);
        }
    }

    /*
     * Free-running 100 Hz level-5 clock tick.  Delivered only while the
     * interrupt register has both the master-enable (bit 0) and level-5 enable
     * (bit 5) set, so it is inert during the PROM self-test (which never sets
     * bit 5) yet available to the monitor and to SunOS/Linux, both of which
     * enable it and drive their periodic clock / timeout counters from it.
     */
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, sun3x_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / SUN3X_HZ);

    if (s->sunos) {
        /* enter the genuine PROM: reset SSP/PC come from its vector (offset
         * 0/4 of the ROM image): SSP=0xfef60c00, PC=0xfefe0228. */
        const uint8_t *rom = memory_region_get_ram_ptr(&s->prom);
        s->reset_sp = ldl_be_p(rom + 0);
        s->reset_pc = ldl_be_p(rom + 4);
    } else if (machine->kernel_filename) {
        sun3x_load_kernel(s, machine);
    } else {
        error_report("sun3x needs -kernel (Linux) or -bios (SunOS boot PROM)");
        exit(1);
    }

    qemu_register_reset(sun3x_cpu_reset, s);
}

static void sun3x_machine_init(MachineClass *mc)
{
    mc->desc = "Sun 3/80 (68030)";
    mc->init = sun3x_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68030");
    mc->max_cpus = 1;
    mc->default_ram_size = SUN3X_RAM_DEFAULT;
    mc->default_ram_id = "sun3x.ram";
    mc->no_floppy = 1;
    mc->no_parallel = 1;
    mc->no_cdrom = 1;
}

DEFINE_MACHINE("sun3x", sun3x_machine_init)
