/*
 * Sun-3/60 ("Ferrari") system emulation.
 *
 * 68020 @ 20MHz with the discrete Sun-3 MMU: 8 contexts, per-context
 * segment map (2048 segments of 128KB) pointing at 256 PMEGs of 16
 * 8KB page table entries.  The MMU decodes the CPU function code
 * lines: FC1/2/5/6 translate through the maps, FC3 is the control
 * space (IDPROM, page/segment maps, context/enable/bus-error/diag
 * registers) reached with MOVES.  While the system enable register's
 * "boot" bit is clear every supervisor program fetch is redirected to
 * the boot EPROM regardless of address; the reset vectors come from
 * the EPROM the same way.
 *
 * A PTE's type field selects one of four physical buses: on-board
 * memory, on-board I/O, VME D16 and VME D32.  These are modelled as
 * four windows in the 64-bit system address space, at (type << 32).
 *
 * On-board I/O (from the 3/60 PROM and the NetBSD sun3 kernels):
 *   0x00000000 zs1 keyboard/mouse    (ipl 6)
 *   0x00020000 zs0 ttyb/ttya         (ipl 6)
 *   0x00040000 EEPROM (2KB)
 *   0x00060000 Intersil ICM7170 RTC  (ipl 5 or 7)
 *   0x00080000 memory error register
 *   0x000A0000 interrupt register
 *   0x00100000 boot EPROM (64KB)
 *   0x00120000 Am7990 LANCE Ethernet (ipl 3)
 *   0x00140000 "si" NCR5380 SCSI     (ipl 2)
 *   0x001E0000 memory control (3/60 parity control, stubbed)
 *
 * On-board memory (type 0) above RAM:
 *   0xFF000000 bwtwo video RAM, 128KB mirrored in a 256KB window
 *              (1152x900x1, MSB first, 1 = black; display enable is
 *              the system enable register's VIDEO bit, the vertical
 *              retrace interrupt is the interrupt register's VIDEO bit,
 *              level 4)
 *   0xFF1C0000 bwtwo resolution ID byte (bit 7 set = 1152x900)
 *   0xFF600000 the boot EPROM again
 *
 * Accesses that decode to nothing latch the TIMEOUT bit in the bus
 * error register and terminate in a bus error, which is how the PROM
 * sizes memory and probes for optional devices.
 *
 * -bios runs the genuine boot PROM; -kernel (without -bios) loads a
 * Linux/m68k sun3 ELF the way the monitor's boot program would and
 * stands in for the PROM with a synthesised vector table (see
 * sun3_build_prom() below).
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "exec/cputlb.h"
#include "exec/page-protection.h"
#include "qemu/datadir.h"
#include "net/net.h"
#include "exec/target_page.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/reset.h"
#include "qemu/timer.h"
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/net/lance.h"
#include "hw/rtc/icm7170.h"
#include "hw/scsi/ncr5380.h"
#include "hw/scsi/scsi.h"
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

/* --- Sun-3 MMU geometry --- */

#define SUN3_VA_BITS            28
#define SUN3_VA_MASK            ((1u << SUN3_VA_BITS) - 1)
#define SUN3_PAGE_BITS          13
#define SUN3_PAGE_SIZE          (1u << SUN3_PAGE_BITS)
#define SUN3_PAGE_MASK          (SUN3_PAGE_SIZE - 1)
#define SUN3_SEG_BITS           17
#define SUN3_NSEGS              (1u << (SUN3_VA_BITS - SUN3_SEG_BITS))
#define SUN3_NCONTEXTS          8
#define SUN3_CONTEXT_MASK       (SUN3_NCONTEXTS - 1)
#define SUN3_NPMEGS             256
#define SUN3_PAGES_PER_SEG      (1u << (SUN3_SEG_BITS - SUN3_PAGE_BITS))

/* page table entry fields */
#define SUN3_PG_VALID           0x80000000
#define SUN3_PG_WRITE           0x40000000
#define SUN3_PG_SYSTEM          0x20000000
#define SUN3_PG_NC              0x10000000
#define SUN3_PG_TYPE_SHIFT      26
#define SUN3_PG_TYPE_MASK       0x0C000000
#define SUN3_PG_REF             0x02000000
#define SUN3_PG_MOD             0x01000000
#define SUN3_PG_FRAME           0x0007FFFF

/* control space, decoded from address bits 31..28 */
#define SUN3_CTL_IDPROM         0x0
#define SUN3_CTL_PGMAP          0x1
#define SUN3_CTL_SEGMAP         0x2
#define SUN3_CTL_CONTEXT        0x3
#define SUN3_CTL_ENABLE         0x4
#define SUN3_CTL_UDVMA          0x5
#define SUN3_CTL_BUSERR         0x6
#define SUN3_CTL_DIAG           0x7

/*
 * System enable register.  The monitor leaves the boot state by
 * writing 0xA0 (NOTBOOT | SDVMA), so NOTBOOT is bit 7.
 */
#define SUN3_ENA_RES            0x01
#define SUN3_ENA_FPA            0x02
#define SUN3_ENA_COPY           0x04
#define SUN3_ENA_VIDEO          0x08
#define SUN3_ENA_CACHE          0x10
#define SUN3_ENA_SDVMA          0x20
#define SUN3_ENA_FPP            0x40
#define SUN3_ENA_NOTBOOT        0x80

/* bus error register (PROM self-test: masks with 0xFC, expects these) */
#define SUN3_BUSERR_WATCHDOG    0x01
#define SUN3_BUSERR_FPAENERR    0x02
#define SUN3_BUSERR_FPABERR     0x04
#define SUN3_BUSERR_VMEBERR     0x08
#define SUN3_BUSERR_TIMEOUT     0x20
#define SUN3_BUSERR_PROTERR     0x40
#define SUN3_BUSERR_INVALID     0x80

/* --- physical layout --- */

/* the four PTE type buses as windows in the 64-bit system space */
#define SUN3_TYPE_SPACE(t)      ((hwaddr)(t) << 32)
#define SUN3_TYPE_OBMEM         0
#define SUN3_TYPE_OBIO          1
#define SUN3_TYPE_VME16         2
#define SUN3_TYPE_VME32         3
#define SUN3_NUM_TYPES          4

#define SUN3_OBIO_ZS_KBD_MS     0x00000000
#define SUN3_OBIO_ZS_TTY        0x00020000
#define SUN3_OBIO_EEPROM        0x00040000
#define SUN3_OBIO_CLOCK         0x00060000
#define SUN3_OBIO_MEMERR        0x00080000
#define SUN3_OBIO_INTREG        0x000A0000
#define SUN3_OBIO_EPROM         0x00100000
/*
 * The EPROM's address lines are wired to the CPU's untranslated low
 * address bits: any PTE whose frame falls into the EPROM select range
 * (obio 0x100000..0x11FFFF) reads ROM[va & 0xFFFF].  The monitor
 * PROM relies on this - it maps several ROM data pages with the same
 * frame number 0x80 and expects each to show its own ROM offset.
 */
#define SUN3_OBIO_EPROM_END     0x00120000
#define SUN3_OBIO_LANCE         0x00120000
#define SUN3_OBIO_SI            0x00140000
#define SUN3_OBIO_MEMREG        0x001E0000

/*
 * "si" SCSI: NCR5380 + Am9516 UDC DMA + 16-bit FIFO at obio 0x140000.
 * Register map from the 3/60 PROM's si driver and NetBSD's sireg.h /
 * si_obio.c (see hw/m68k/sun3-NOTES.md, "si" section):
 * the 5380's eight registers are byte-spaced at +0..+7, the rest are
 * 16-bit.  Registers marked VME exist on the VME "si"/"sc" boards only;
 * the PROM still writes them, so they are plain read/write scratch here.
 */
#define SUN3_SI_SIZE            0x28
#define SI_DMA_ADDRH            0x08    /* VME */
#define SI_DMA_ADDRL            0x0a    /* VME */
#define SI_DMA_COUNTH           0x0c    /* VME */
#define SI_DMA_COUNTL           0x0e    /* VME */
#define SI_UDC_DATA             0x10    /* UDC register data */
#define SI_UDC_ADDR             0x12    /* UDC register select */
#define SI_FIFO_DATA            0x14    /* FIFO word latch (odd-byte pickup) */
#define SI_FIFO_COUNT           0x16    /* byte counter, loaded by the driver */
#define SI_CSR                  0x18
#define SI_BPACK_HI             0x1a    /* VME */
#define SI_BPACK_LO             0x1c    /* VME */
#define SI_IVECT                0x1e    /* VME (PROM writes 0x3d00 at init) */
#define SI_FIFO_CNT_HI          0x20    /* VME */

#define SI_CSR_DMA_ACTIVE       0x8000  /* r/o */
#define SI_CSR_DMA_CONFLICT     0x4000  /* r/o: register access during DMA */
#define SI_CSR_DMA_BUS_ERR      0x2000  /* r/o: DVMA bus error */
#define SI_CSR_ID               0x1000  /* r/o: 1 = SCSI-3 / Emulex (VME) */
#define SI_CSR_FIFO_FULL        0x0800  /* r/o */
#define SI_CSR_FIFO_EMPTY       0x0400  /* r/o */
#define SI_CSR_SBC_IP           0x0200  /* r/o: 5380 interrupt pending */
#define SI_CSR_DMA_IP           0x0100  /* r/o: DMA (UDC) interrupt pending */
#define SI_CSR_DMA_EN           0x0010  /* r/w: DMA enable (VME) */
#define SI_CSR_SEND             0x0008  /* r/w: DMA direction, 1 = to device */
#define SI_CSR_INTR_EN          0x0004  /* r/w: interrupt enable */
#define SI_CSR_FIFO_RES         0x0002  /* r/w: FIFO reset, active LOW */
#define SI_CSR_SCSI_RES         0x0001  /* r/w: 5380 + UDC reset, active LOW */
#define SI_CSR_RW_BITS          0x001f

/* Am9516 UDC registers the drivers touch (through UDC_ADDR/UDC_DATA) */
#define UDC_ADR_CAR_LOW         0x22    /* chain address, low word */
#define UDC_ADR_CAR_HIGH        0x26    /* chain address, bits 23..16 in 15..8 */
#define UDC_ADR_COMMAND         0x2e    /* master command register */
#define UDC_ADR_COUNT           0x32    /* current operation count (words) */
#define UDC_ADR_MODE            0x38    /* channel mode register */
#define UDC_CMD_RESET           0x00
#define UDC_CMD_CIE             0x32    /* channel interrupt enable */
#define UDC_CMD_START_CHN       0xa0    /* start chain: load the table below */

/* the six-word command table the UDC fetches from DVMA at start-chain */
#define UDC_TBL_RSEL            0       /* 0x182 receive / 0x282 send */
#define UDC_TBL_ADDRH           1       /* ((pa & 0xff0000) >> 8) | 0x40 */
#define UDC_TBL_ADDRL           2       /* pa & 0xffff */
#define UDC_TBL_COUNT           3       /* transfer length in words */
#define UDC_TBL_CMRH            4       /* 0x40 */
#define UDC_TBL_CMRL            5       /* 0xd2 receive / 0xc2 send */
#define UDC_TBL_WORDS           6

/*
 * Sun-3 DVMA: on-board DMA masters present a 24-bit address that the
 * MMU translates as virtual 0x0F000000 | addr in the *current* context
 * (which is why the kernels map their DVMA segments in all contexts).
 */
#define SUN3_DVMA_BASE          0x0F000000
#define SUN3_DVMA_MASK          0x00FFFFFF

/*
 * 3/60 on-board devices in the high type-0 (obmem) space, from the
 * monitor's device map table at ROM offset 0xD350:
 * 0xFF000000 video RAM (the monitor maps and clears a 256KB window at
 *            VA 0x0FE20000; the bwtwo carries 128KB, mirrored),
 * 0xFF1C0000 the bwtwo resolution ID byte: bit 7 set = 1152x900, clear
 *            = the 1600x1280 hi-res option.  The PROM's bwtwo probe
 *            (0xfef25a2) peeks it with a byte read and only stores its
 *            width/height variables when the peek succeeds -- with a
 *            bus error there they stay 0 and the console draws with a
 *            zero line stride (observed: screen_y advanced, VRAM empty),
 *            so the byte exists on every 3/60.  NetBSD bw2attach and
 *            SunOS bwtwoprobe peek the same byte, falling back to
 *            1152x900 on a bus error.
 * 0xFF600000 the boot EPROM again (128KB window, ROM mirrored).
 */
#define SUN3_OBMEM_VRAM         0xFF000000
#define SUN3_OBMEM_VRAM_SIZE    (128 * KiB)
#define SUN3_OBMEM_BW2_ID       0xFF1C0000
#define SUN3_BW2_ID_1152X900    0x80    /* only bit 7 is decoded */
#define SUN3_OBMEM_EPROM        0xFF600000
/*
 * Vertical retrace rate of the 1152x900 monitor.  Only the interrupt
 * register's VIDEO_ENAB bit gates it (SunOS bwtwointr acks by clearing
 * that bit; the PROM and NetBSD never enable it).
 */
#define SUN3_VIDEO_HZ           67

#define SUN3_ROM_SIZE           (64 * KiB)
#define SUN3_EEPROM_SIZE        (2 * KiB)
/* EEPROM configuration bytes the PROM honours */
#define SUN3_EEPROM_CONSOLE     0x1F
#define SUN3_EEPROM_CONS_BW     0x00
#define SUN3_EEPROM_CONS_TTYA   0x10
#define SUN3_EEPROM_CONS_TTYB   0x11
#define SUN3_RAM_MAX            (24 * MiB)
#define SUN3_INVALID_PMEG       0xFF

/*
 * -kernel (Linux/m68k sun3, no PROM image): the kernel is linked at
 * KERNBASE but loaded at physical 0 and entered at entry - KERNBASE
 * through the boot loader's low identity map (arch/m68k/kernel/sun3-head.S,
 * asm/sun3-head.h).  It finds the PROM vector table at VA 0x0FEF0000
 * (asm/openprom.h LINUX_OPPROM_BEGVM), which on the real machine is the
 * boot EPROM itself; the synthesised PROM below is a 64KB EPROM image
 * whose first 0xE0 bytes are that table, exactly like the genuine ROM.
 */
#define SUN3_KERNBASE           0x0E000000
#define SUN3_PROM_VA            0x0FEF0000
#define SUN3_IO_VA              0x0FE00000  /* the PROM's device page segment */
#define SUN3_PMEG_IO            254         /* config.c: "main I/O" */
#define SUN3_PMEG_ROM           247         /* config.c: "ROM mapping" */
#define SUN3_PROM_PTE_IO        (SUN3_PG_VALID | SUN3_PG_WRITE | \
                                 SUN3_PG_SYSTEM | SUN3_PG_NC | \
                                 (SUN3_TYPE_OBIO << SUN3_PG_TYPE_SHIFT))
#define SUN3_PROM_PTE_RAM       (SUN3_PG_VALID | SUN3_PG_WRITE | \
                                 SUN3_PG_SYSTEM)

/* interrupt register bits (PROM writes 0x21 = enable-all + clock-5) */
#define SUN3_IREG_ALL_ENAB      0x01
#define SUN3_IREG_SOFT_1        0x02
#define SUN3_IREG_SOFT_2        0x04
#define SUN3_IREG_SOFT_3        0x08
#define SUN3_IREG_VIDEO_ENAB    0x10    /* level-4 vertical retrace */
#define SUN3_IREG_CLOCK_ENAB_5  0x20
#define SUN3_IREG_CLOCK_ENAB_7  0x80    /* the monitor runs on 0x81 */

/* interrupt levels of the wired sources */
#define SUN3_IPL_SI             2
#define SUN3_IPL_LANCE          3
#define SUN3_IPL_VIDEO          4
#define SUN3_IPL_CLOCK          5
#define SUN3_IPL_ZS             6
#define SUN3_IPL_CLOCK_NMI      7
#define SUN3_IPL_MEMERR         7

/* interrupt sources (several share a level, so they get own lines) */
enum {
    SUN3_IRQ_SRC_ZS_TTY_B,
    SUN3_IRQ_SRC_ZS_TTY_A,
    SUN3_IRQ_SRC_ZS_KBD_B,
    SUN3_IRQ_SRC_ZS_KBD_A,
    SUN3_IRQ_SRC_SI,
    SUN3_IRQ_SRC_LANCE,
    SUN3_IRQ_SRC_MEMERR,
    SUN3_IRQ_NUM_SRCS,
};

static const uint8_t sun3_irq_src_level[SUN3_IRQ_NUM_SRCS] = {
    [SUN3_IRQ_SRC_ZS_TTY_B] = SUN3_IPL_ZS,
    [SUN3_IRQ_SRC_ZS_TTY_A] = SUN3_IPL_ZS,
    [SUN3_IRQ_SRC_ZS_KBD_B] = SUN3_IPL_ZS,
    [SUN3_IRQ_SRC_ZS_KBD_A] = SUN3_IPL_ZS,
    [SUN3_IRQ_SRC_SI]       = SUN3_IPL_SI,
    [SUN3_IRQ_SRC_LANCE]    = SUN3_IPL_LANCE,
    [SUN3_IRQ_SRC_MEMERR]   = SUN3_IPL_MEMERR,
};

/* IDPROM */
#define SUN3_IDPROM_SIZE        32
#define SUN3_IDPROM_FORMAT      0x01
#define SUN3_IDPROM_MACH_3_60   0x17
#define SUN3_ZS_FREQ            4915200

#define TYPE_SUN3_MACHINE MACHINE_TYPE_NAME("sun3-60")
OBJECT_DECLARE_SIMPLE_TYPE(Sun3MachineState, SUN3_MACHINE)

struct Sun3MachineState {
    MachineState parent_obj;

    M68kCPU *cpu;
    MemoryRegion type_bg[SUN3_NUM_TYPES];
    MemoryRegion rom;
    MemoryRegion rom_obmem[2];  /* the EPROM again at obmem 0xFF600000 */
    MemoryRegion eeprom;
    MemoryRegion intreg_mr;
    MemoryRegion memerr_mr;
    MemoryRegion memreg_mr;
    MemoryRegion si_mr;
    qemu_irq *irqs;             /* per-source device inputs */

    /* si SCSI */
    NCR5380State si_sbc;
    uint16_t si_csr;
    uint16_t si_fifo_count;
    uint16_t si_fifo_data;
    uint16_t si_scratch[SUN3_SI_SIZE / 2];  /* VME-only registers */
    bool si_sbc_irq;            /* 5380 IRQ output */
    uint8_t udc_addr;           /* UDC register select */
    uint16_t udc_car_hi, udc_car_lo, udc_mode;
    uint32_t udc_dma_addr;      /* active channel: 24-bit DVMA address */
    uint16_t udc_count;         /* active channel: words left */
    bool udc_active;
    bool udc_ip;                /* DMA interrupt pending */
    QEMUTimer si_dma_timer;     /* the transfer runs after the start */

    /* "le" LANCE ethernet */
    SysBusPCNetState *lance;
    MemoryRegion lance_mr;

    uint32_t reset_sp;
    uint32_t reset_pc;
    bool kernel_boot;           /* -kernel: synthesised PROM, maps preloaded */
    uint8_t idprom[SUN3_IDPROM_SIZE];

    /* MMU */
    uint8_t segmap[SUN3_NCONTEXTS][SUN3_NSEGS];
    uint32_t pgmap[SUN3_NPMEGS * SUN3_PAGES_PER_SEG];
    uint8_t context;
    uint8_t enable;
    uint8_t buserr;
    uint8_t diag;
    uint8_t udvma;

    /* interrupts */
    uint8_t intreg;
    uint16_t irq_sources;       /* asserted device lines, by source index */
    /*
     * The clock interrupt is latched on the board, once per enabled
     * level: the OS acknowledges a tick by momentarily dropping that
     * level's enable bit in the interrupt register.  The two latches are
     * separate -- SunOS runs with both the level-5 and the level-7 clock
     * enabled (intreg 0xA1) and acks the level-7 tick with 0xA1 -> 0x21
     * -> 0xA1, which with a single shared latch re-presented the stale
     * level 5 latch as a second, unexpected level-7 interrupt the moment
     * the enable came back (the PROM then reports "Exception 0x7C").
     */
    bool clock_latch[8];        /* board latches fed by the ICM7170 INT pin */

    /* on-board bwtwo */
    DeviceState *bwtwo;
    MemoryRegion bw2_id_mr;
    qemu_irq video_enable;      /* the bwtwo's display enable input */
    bool video_latch;           /* vertical retrace seen while enabled */
    QEMUTimer video_timer;
    bool console_fb;            /* "sunos-console": EEPROM byte 0x1F */

    /* memory error (parity) registers */
    uint8_t memerr_csr;
    GHashTable *parity_poison;  /* physical addresses holding bad parity */
    MemoryRegion parity_overlay;
    AddressSpace ram_as;        /* RAM below the parity overlay */
};

/* --- interrupt glue --- */

static void sun3_irq_update(Sun3MachineState *m)
{
    int level = 0;
    uint8_t pend = 0;
    int src;

    for (src = 0; src < SUN3_IRQ_NUM_SRCS; src++) {
        if (m->irq_sources & (1 << src)) {
            pend |= 1 << sun3_irq_src_level[src];
        }
    }

    /* soft interrupt request bits both enable and assert their level */
    if (m->intreg & SUN3_IREG_SOFT_1) {
        pend |= 1 << 1;
    }
    if (m->intreg & SUN3_IREG_SOFT_2) {
        pend |= 1 << 2;
    }
    if (m->intreg & SUN3_IREG_SOFT_3) {
        pend |= 1 << 3;
    }
    if (m->clock_latch[SUN3_IPL_CLOCK] &&
        (m->intreg & SUN3_IREG_CLOCK_ENAB_5)) {
        pend |= 1 << SUN3_IPL_CLOCK;
    }
    if (m->clock_latch[SUN3_IPL_CLOCK_NMI] &&
        (m->intreg & SUN3_IREG_CLOCK_ENAB_7)) {
        pend |= 1 << SUN3_IPL_CLOCK_NMI;
    }
    if (m->video_latch && (m->intreg & SUN3_IREG_VIDEO_ENAB)) {
        pend |= 1 << SUN3_IPL_VIDEO;
    }
    if (!(m->intreg & SUN3_IREG_ALL_ENAB)) {
        pend = 0;
    }
    if (pend) {
        level = 31 - clz32(pend);
    }
    m68k_set_irq_level(m->cpu, level,
                       level ? EXCP_INT_LEVEL_1 + level - 1 : 0);
}

static void sun3_set_irq(void *opaque, int n, int level)
{
    Sun3MachineState *m = opaque;

    if (level) {
        m->irq_sources |= 1 << n;
    } else {
        m->irq_sources &= ~(1 << n);
    }
    sun3_irq_update(m);
}

static void sun3_clock_irq(void *opaque, int n, int level)
{
    Sun3MachineState *m = opaque;

    /*
     * The board latches the ICM7170's interrupt output, separately for
     * each of the two clock levels; the OS clears a latch by momentarily
     * dropping that level's enable bit in the interrupt register (it
     * also reads the 7170's status register, which drops this line
     * again).
     */
    if (level) {
        m->clock_latch[SUN3_IPL_CLOCK] = true;
        m->clock_latch[SUN3_IPL_CLOCK_NMI] = true;
    }
    sun3_irq_update(m);
}

/*
 * Video (vertical retrace) interrupt, level 4: latched once per frame
 * while the interrupt register's VIDEO_ENAB bit is set, cleared by
 * clearing that bit (SunOS bwtwointr, locore 0xe004cf6).
 */
static void sun3_video_timer_cb(void *opaque)
{
    Sun3MachineState *m = opaque;

    if (!(m->intreg & SUN3_IREG_VIDEO_ENAB)) {
        return;
    }
    m->video_latch = true;
    sun3_irq_update(m);
    timer_mod(&m->video_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / SUN3_VIDEO_HZ);
}

static uint64_t sun3_intreg_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3MachineState *m = opaque;

    return m->intreg;
}

static void sun3_intreg_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Sun3MachineState *m = opaque;

    qemu_log_mask(CPU_LOG_INT, "sun3: intreg <- %02x\n", (uint8_t)val);
    if (!(val & SUN3_IREG_CLOCK_ENAB_5)) {
        m->clock_latch[SUN3_IPL_CLOCK] = false;
    }
    if (!(val & SUN3_IREG_CLOCK_ENAB_7)) {
        m->clock_latch[SUN3_IPL_CLOCK_NMI] = false;
    }
    if (!(val & SUN3_IREG_VIDEO_ENAB)) {
        m->video_latch = false;
        timer_del(&m->video_timer);
    } else if (!(m->intreg & SUN3_IREG_VIDEO_ENAB)) {
        timer_mod(&m->video_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / SUN3_VIDEO_HZ);
    }
    m->intreg = val;
    sun3_irq_update(m);
}

static const MemoryRegionOps sun3_intreg_ops = {
    .read = sun3_intreg_read,
    .write = sun3_intreg_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- memory error (parity) registers --- */

/*
 * Behaviour reconstructed from the PROM self-test: the CSR at +0 has
 * enable bits interrupt-enable 0x10, "test" 0x20 (writes store bad
 * parity) and "check" 0x40.  A parity error latches 0x80 plus the
 * failing byte lane in the low nibble (0x8 = byte 0 .. 0x1 = byte 3)
 * and, with 0x10 set, raises a level-7 interrupt.  Any register write
 * clears the error latch.  The register at +4 is written with zero by
 * the PROM around the test; it is kept as a dummy.
 *
 * Bad parity cannot be stored in QEMU RAM, so while test mode is on
 * (or poisoned bytes remain) an MMIO overlay shadows RAM and keeps a
 * set of poisoned addresses.
 */
#define SUN3_MEMERR_CSR         0x0
#define SUN3_MEMERR_REG2        0x4
#define SUN3_MEMERR_INTENA      0x10
#define SUN3_MEMERR_TEST        0x20
#define SUN3_MEMERR_CHECK       0x40
#define SUN3_MEMERR_IPEND       0x80
#define SUN3_MEMERR_ENABLES     (SUN3_MEMERR_INTENA | SUN3_MEMERR_TEST | \
                                 SUN3_MEMERR_CHECK)
#define SUN3_MEMERR_LANE(a)     (0x8 >> ((a) & 3))

static void sun3_parity_overlay_update(Sun3MachineState *m)
{
    bool need = (m->memerr_csr & SUN3_MEMERR_TEST) ||
                g_hash_table_size(m->parity_poison);

    memory_region_set_enabled(&m->parity_overlay, need);
}

static MemTxResult sun3_parity_read(void *opaque, hwaddr addr, uint64_t *val,
                                    unsigned size, MemTxAttrs attrs)
{
    Sun3MachineState *m = opaque;
    uint8_t buf[8];
    unsigned i;

    address_space_read(&m->ram_as, addr, attrs, buf, size);
    *val = ldn_be_p(buf, size);

    if (m->memerr_csr & SUN3_MEMERR_CHECK) {
        uint8_t lanes = 0;

        for (i = 0; i < size; i++) {
            if (g_hash_table_contains(m->parity_poison,
                                      GUINT_TO_POINTER(addr + i))) {
                lanes |= SUN3_MEMERR_LANE(addr + i);
            }
        }
        if (lanes) {
            m->memerr_csr |= SUN3_MEMERR_IPEND | lanes;
            if (m->memerr_csr & SUN3_MEMERR_INTENA) {
                qemu_set_irq(m->irqs[SUN3_IRQ_SRC_MEMERR], 1);
            }
        }
    }
    return MEMTX_OK;
}

static MemTxResult sun3_parity_write(void *opaque, hwaddr addr, uint64_t val,
                                     unsigned size, MemTxAttrs attrs)
{
    Sun3MachineState *m = opaque;
    uint8_t buf[8];
    unsigned i;

    stn_be_p(buf, size, val);
    address_space_write(&m->ram_as, addr, attrs, buf, size);

    for (i = 0; i < size; i++) {
        if (m->memerr_csr & SUN3_MEMERR_TEST) {
            g_hash_table_add(m->parity_poison, GUINT_TO_POINTER(addr + i));
        } else {
            g_hash_table_remove(m->parity_poison, GUINT_TO_POINTER(addr + i));
        }
    }
    return MEMTX_OK;
}

static const MemoryRegionOps sun3_parity_ops = {
    .read_with_attrs = sun3_parity_read,
    .write_with_attrs = sun3_parity_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static uint64_t sun3_memerr_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3MachineState *m = opaque;

    switch (addr) {
    case SUN3_MEMERR_CSR:
        return m->memerr_csr;
    default:
        return 0;
    }
}

static void sun3_memerr_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    Sun3MachineState *m = opaque;

    switch (addr) {
    case SUN3_MEMERR_CSR:
        m->memerr_csr = val & SUN3_MEMERR_ENABLES;
        break;
    default:
        /* any register write also clears the error latch */
        m->memerr_csr &= SUN3_MEMERR_ENABLES;
        break;
    }
    qemu_set_irq(m->irqs[SUN3_IRQ_SRC_MEMERR], 0);
    sun3_parity_overlay_update(m);
}

static const MemoryRegionOps sun3_memerr_ops = {
    .read = sun3_memerr_read,
    .write = sun3_memerr_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- other stub registers --- */

static uint64_t sun3_zero_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void sun3_zero_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
}

static const MemoryRegionOps sun3_stub_ops = {
    .read = sun3_zero_read,
    .write = sun3_zero_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- bus timeout backgrounds --- */

static MemTxResult sun3_bg_read(void *opaque, hwaddr addr, uint64_t *val,
                                unsigned size, MemTxAttrs attrs)
{
    Sun3MachineState *m = opaque;

    m->buserr |= SUN3_BUSERR_TIMEOUT;
    *val = 0;
    return MEMTX_DECODE_ERROR;
}

static MemTxResult sun3_bg_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size, MemTxAttrs attrs)
{
    Sun3MachineState *m = opaque;

    m->buserr |= SUN3_BUSERR_TIMEOUT;
    return MEMTX_DECODE_ERROR;
}

static const MemoryRegionOps sun3_bg_ops = {
    .read_with_attrs = sun3_bg_read,
    .write_with_attrs = sun3_bg_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- the MMU --- */

static unsigned sun3_pte_index(Sun3MachineState *m, uint32_t va)
{
    unsigned seg = (va & SUN3_VA_MASK) >> SUN3_SEG_BITS;
    unsigned pmeg = m->segmap[m->context][seg];

    return pmeg * SUN3_PAGES_PER_SEG +
           ((va >> SUN3_PAGE_BITS) & (SUN3_PAGES_PER_SEG - 1));
}

static hwaddr sun3_pte_phys(uint32_t pte, uint32_t va)
{
    unsigned type = (pte & SUN3_PG_TYPE_MASK) >> SUN3_PG_TYPE_SHIFT;

    return SUN3_TYPE_SPACE(type) |
           ((hwaddr)(pte & SUN3_PG_FRAME) << SUN3_PAGE_BITS) |
           (va & SUN3_PAGE_MASK);
}

static bool sun3_mmu_translate(Sun3MachineState *m, vaddr addr,
                               MMUAccessType access_type, bool super,
                               hwaddr *physical, int *prot)
{
    unsigned idx = sun3_pte_index(m, addr);
    uint32_t pte = m->pgmap[idx];

    if (!(pte & SUN3_PG_VALID)) {
        qemu_log_mask(CPU_LOG_INT,
                      "sun3: invalid pte va=%08x seg=%03x pmeg=%02x\n",
                      (uint32_t)addr, (uint32_t)(addr & SUN3_VA_MASK) >>
                      SUN3_SEG_BITS, m->segmap[m->context]
                      [(addr & SUN3_VA_MASK) >> SUN3_SEG_BITS]);
        m->buserr |= SUN3_BUSERR_INVALID;
        return false;
    }
    if (!super && (pte & SUN3_PG_SYSTEM)) {
        m->buserr |= SUN3_BUSERR_PROTERR;
        return false;
    }
    if (access_type == MMU_DATA_STORE && !(pte & SUN3_PG_WRITE)) {
        qemu_log_mask(CPU_LOG_INT,
                      "sun3: write prot va=%08x pte=%08x\n",
                      (uint32_t)addr, pte);
        m->buserr |= SUN3_BUSERR_PROTERR;
        return false;
    }

    pte |= SUN3_PG_REF;
    if (access_type == MMU_DATA_STORE) {
        pte |= SUN3_PG_MOD;
    }
    m->pgmap[idx] = pte;

    *physical = sun3_pte_phys(pte, addr);
    /* EPROM frames decode the CPU's untranslated low address bits */
    if (((pte & SUN3_PG_TYPE_MASK) >> SUN3_PG_TYPE_SHIFT) == SUN3_TYPE_OBIO) {
        hwaddr obio = *physical & 0xFFFFFFFF;

        if (obio >= SUN3_OBIO_EPROM && obio < SUN3_OBIO_EPROM_END) {
            *physical = SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) + SUN3_OBIO_EPROM +
                        (addr & (SUN3_ROM_SIZE - 1));
        }
    }
    /*
     * Writable pages start out read-only in the TLB until the first
     * store marks the PTE modified, so the MOD bit is kept exact.
     */
    *prot = PAGE_READ | PAGE_EXEC;
    if ((pte & SUN3_PG_WRITE) && (pte & SUN3_PG_MOD)) {
        *prot |= PAGE_WRITE;
    }
    return true;
}

static bool sun3_boot_state_fetch(Sun3MachineState *m, bool super,
                                  MMUAccessType access_type)
{
    return !(m->enable & SUN3_ENA_NOTBOOT) && super &&
           access_type == MMU_INST_FETCH;
}

static bool sun3_fc_translate(void *opaque, vaddr addr,
                              MMUAccessType access_type, int mmu_idx,
                              hwaddr *physical, int *prot)
{
    Sun3MachineState *m = opaque;
    bool super = mmu_idx == MMU_KERNEL_IDX;

    if (sun3_boot_state_fetch(m, super, access_type)) {
        /* boot state: all supervisor program fetches read the EPROM */
        *physical = SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) + SUN3_OBIO_EPROM +
                    (addr & (SUN3_ROM_SIZE - 1));
        *prot = PAGE_READ | PAGE_EXEC;
        return true;
    }
    return sun3_mmu_translate(m, addr, access_type, super, physical, prot);
}

/*
 * A page or segment map update changes what the QEMU TLB may have
 * cached for the affected virtual page.  The MMU only sees the low 28
 * address bits but the TLB is tagged with all 32 (the PROM reaches
 * its device pages through sign-extended absolute addresses), so
 * flush every alias of the page.
 */
static void sun3_flush_va_aliases(Sun3MachineState *m, uint32_t va)
{
    CPUState *cs = CPU(m->cpu);
    int nibble;

    va &= SUN3_VA_MASK & ~SUN3_PAGE_MASK;
    for (nibble = 0; nibble < 16; nibble++) {
        uint32_t alias = va | ((uint32_t)nibble << SUN3_VA_BITS);
        vaddr sub;

        /* an 8KB MMU page spans several QEMU target pages */
        for (sub = 0; sub < SUN3_PAGE_SIZE; sub += TARGET_PAGE_SIZE) {
            tlb_flush_page(cs, alias + sub);
        }
    }
}

/* byte lane insert/extract for sub-word control space accesses */
static uint32_t sun3_reg32_read(uint32_t reg, unsigned offset, unsigned size)
{
    return extract32(reg, (4 - size - (offset & 3)) * 8, size * 8);
}

static uint32_t sun3_reg32_write(uint32_t reg, unsigned offset, unsigned size,
                                 uint32_t val)
{
    return deposit32(reg, (4 - size - (offset & 3)) * 8, size * 8, val);
}

static bool sun3_control_space(Sun3MachineState *m, vaddr addr, unsigned size,
                               uint64_t *val, bool is_store)
{
    uint32_t va = addr & SUN3_VA_MASK;
    unsigned idx;

    switch ((uint32_t)addr >> SUN3_VA_BITS) {
    case SUN3_CTL_IDPROM:
        if (!is_store) {
            *val = m->idprom[va % SUN3_IDPROM_SIZE];
        }
        return true;
    case SUN3_CTL_PGMAP:
        idx = sun3_pte_index(m, va);
        if (is_store) {
            m->pgmap[idx] = sun3_reg32_write(m->pgmap[idx], va, size, *val);
            qemu_log_mask(CPU_LOG_INT,
                          "sun3: pgmap[va %08x idx %x] <- %08x\n",
                          va, idx, m->pgmap[idx]);
            sun3_flush_va_aliases(m, va);
        } else {
            *val = sun3_reg32_read(m->pgmap[idx], va, size);
            qemu_log_mask(CPU_LOG_INT,
                          "sun3: pgmap[va %08x idx %x] -> %08x\n",
                          va, idx, (uint32_t)*val);
        }
        return true;
    case SUN3_CTL_SEGMAP:
        idx = va >> SUN3_SEG_BITS;
        if (is_store) {
            m->segmap[m->context][idx] = *val;
            qemu_log_mask(CPU_LOG_INT,
                          "sun3: segmap[ctx %d seg %03x] <- %02x\n",
                          m->context, idx, (uint8_t)*val);
            tlb_flush(CPU(m->cpu));
        } else {
            *val = m->segmap[m->context][idx];
            qemu_log_mask(CPU_LOG_INT,
                          "sun3: segmap[ctx %d seg %03x] -> %02x\n",
                          m->context, idx, (uint8_t)*val);
        }
        return true;
    case SUN3_CTL_CONTEXT:
        if (is_store) {
            m->context = *val & SUN3_CONTEXT_MASK;
            tlb_flush(CPU(m->cpu));
        } else {
            *val = m->context;
        }
        return true;
    case SUN3_CTL_ENABLE:
        if (is_store) {
            qemu_log_mask(CPU_LOG_INT, "sun3: enable <- %02x\n",
                          (uint8_t)*val);
            m->enable = *val;
            tlb_flush(CPU(m->cpu));
            qemu_set_irq(m->video_enable, !!(m->enable & SUN3_ENA_VIDEO));
        } else {
            *val = m->enable;
        }
        return true;
    case SUN3_CTL_UDVMA:
        if (is_store) {
            m->udvma = *val;
        } else {
            *val = m->udvma;
        }
        return true;
    case SUN3_CTL_BUSERR:
        if (!is_store) {
            *val = m->buserr;
            m->buserr = 0;
        }
        return true;
    case SUN3_CTL_DIAG:
        if (is_store) {
            qemu_log_mask(CPU_LOG_INT, "sun3: diag LED <- %02x\n",
                          (uint8_t)*val);
            m->diag = *val;
        } else {
            *val = m->diag;
        }
        return true;
    default:
        return false;
    }
}

/*
 * A MOVES access that crosses an 8KB page boundary is split into two
 * bus cycles on hardware, each individually translated; mirror that
 * by chunking the access at page boundaries.
 */
static bool sun3_moves_data(Sun3MachineState *m, int fc, vaddr addr,
                            unsigned size, uint64_t *val, bool is_store)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
    MMUAccessType access_type = is_store ? MMU_DATA_STORE : MMU_DATA_LOAD;
    bool super = fc & 4;
    bool program = fc == 2 || fc == 6;
    uint8_t bytes[8];
    unsigned off = 0;

    if (is_store) {
        stn_be_p(bytes, size, *val);
    }
    while (off < size) {
        vaddr cur = addr + off;
        unsigned chunk = MIN(size - off,
                             SUN3_PAGE_SIZE - (cur & SUN3_PAGE_MASK));
        hwaddr pa;
        int prot;

        if (program && sun3_boot_state_fetch(m, super, MMU_INST_FETCH) &&
            !is_store) {
            pa = SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) + SUN3_OBIO_EPROM +
                 (cur & (SUN3_ROM_SIZE - 1));
        } else if (!sun3_mmu_translate(m, cur, access_type, super,
                                       &pa, &prot)) {
            return false;
        }
        if (address_space_rw(&address_space_memory, pa, attrs,
                             &bytes[off], chunk, is_store) != MEMTX_OK) {
            return false;
        }
        off += chunk;
    }
    if (!is_store) {
        *val = ldn_be_p(bytes, size);
    }
    return true;
}

static bool sun3_fc_moves(void *opaque, int fc, vaddr addr, unsigned size,
                          uint64_t *val, bool is_store)
{
    Sun3MachineState *m = opaque;

    switch (fc) {
    case 3:
        return sun3_control_space(m, addr, size, val, is_store);
    case 1:
    case 2:
    case 5:
    case 6:
        return sun3_moves_data(m, fc, addr, size, val, is_store);
    default:
        return false;
    }
}

static const M68kFCOps sun3_fc_ops = {
    .translate = sun3_fc_translate,
    .moves = sun3_fc_moves,
};

/* --- DVMA --- */

/*
 * Move a run of bytes between a device's 24-bit DVMA address and memory
 * through the Sun-3 MMU: the same segment/page-map walk the CPU uses, so
 * the REF/MOD bits are kept and an invalid or protected page faults the
 * transfer (the caller latches its bus-error status bit).  Chunked at
 * 8KB page boundaries like any other bus master.
 */
static bool sun3_dvma_rw(Sun3MachineState *m, uint32_t dva, uint8_t *buf,
                         unsigned len, bool is_write)
{
    MMUAccessType access = is_write ? MMU_DATA_STORE : MMU_DATA_LOAD;
    uint8_t saved_buserr = m->buserr;

    while (len) {
        uint32_t va = SUN3_DVMA_BASE | (dva & SUN3_DVMA_MASK);
        unsigned chunk = MIN(len, SUN3_PAGE_SIZE - (va & SUN3_PAGE_MASK));
        hwaddr pa;
        int prot;

        if (!sun3_mmu_translate(m, va, access, true, &pa, &prot)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "sun3: DVMA %s fault dva=%06x va=%08x ctx=%d\n",
                          is_write ? "write" : "read", dva & SUN3_DVMA_MASK,
                          va, m->context);
            /* a DVMA fault is the device's error, not a CPU bus error */
            m->buserr = saved_buserr;
            return false;
        }
        if (address_space_rw(&address_space_memory, pa,
                             MEMTXATTRS_UNSPECIFIED, buf, chunk,
                             is_write) != MEMTX_OK) {
            m->buserr = saved_buserr;
            return false;
        }
        buf += chunk;
        dva += chunk;
        len -= chunk;
    }
    return true;
}

/* --- "si" SCSI: NCR5380 + Am9516 UDC + FIFO --- */

static void sun3_si_update_irq(Sun3MachineState *m)
{
    bool level = (m->si_csr & SI_CSR_INTR_EN) && (m->si_sbc_irq || m->udc_ip);

    qemu_set_irq(m->irqs[SUN3_IRQ_SRC_SI], level);
}

static void sun3_si_sbc_irq(void *opaque, int n, int level)
{
    Sun3MachineState *m = opaque;

    m->si_sbc_irq = level;
    sun3_si_update_irq(m);
}

/*
 * Run the DMA engine: while the UDC channel has words left and the 5380
 * is armed with a byte to move, shuttle data between the SCSI data phase
 * and DVMA memory a word at a time.  The FIFO assembles bytes into
 * big-endian words; an odd trailing byte on a receive stays in the FIFO
 * data latch (high byte) for the driver to pick up, exactly as the PROM
 * and NetBSD do.  fifo_count is the driver-loaded byte counter, which
 * counts down per byte moved; the UDC count is in words.
 */
static void sun3_si_dma_run(Sun3MachineState *m)
{
    NCR5380State *n = &m->si_sbc;
    int dir = ncr5380_dma_direction(n);
    bool out;

    if (!m->udc_active || dir < 0) {
        return;
    }
    out = dir == 1;
    if (!!(m->si_csr & SI_CSR_SEND) != out) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sun3: si DMA direction mismatch: csr %04x, 5380 %s\n",
                      m->si_csr, out ? "send" : "receive");
    }
    while (m->udc_count > 0 && ncr5380_pdma_ready(n, out)) {
        uint8_t w[2];

        if (out) {
            if (!sun3_dvma_rw(m, m->udc_dma_addr, w, 2, false)) {
                m->si_csr |= SI_CSR_DMA_BUS_ERR;
                m->udc_active = false;
                break;
            }
            m->udc_dma_addr += 2;
            m->udc_count--;
            m->si_fifo_data = (w[0] << 8) | w[1];
            for (int i = 0; i < 2; i++) {
                /* the byte counter stops the pad byte of an odd send */
                if (m->si_fifo_count && ncr5380_pdma_ready(n, true)) {
                    ncr5380_pdma_write(n, w[i]);
                    m->si_fifo_count--;
                }
            }
        } else {
            w[0] = ncr5380_pdma_read(n);
            m->si_fifo_count--;
            if (!ncr5380_pdma_ready(n, false)) {
                /* odd trailing byte: left in the FIFO latch, high byte */
                m->si_fifo_data = w[0] << 8;
                break;
            }
            w[1] = ncr5380_pdma_read(n);
            m->si_fifo_count--;
            m->si_fifo_data = (w[0] << 8) | w[1];
            if (!sun3_dvma_rw(m, m->udc_dma_addr, w, 2, true)) {
                m->si_csr |= SI_CSR_DMA_BUS_ERR;
                m->udc_active = false;
                break;
            }
            m->udc_dma_addr += 2;
            m->udc_count--;
        }
    }
    if (m->udc_active && m->udc_count == 0) {
        /*
         * Chain complete: /EOP to the 5380 (which raises its own
         * interrupt -> SBC_IP when the target then changes phase).
         *
         * DMA_IP is NOT a normal-completion bit: both drivers treat it
         * as an error after a transfer.  SunOS's si_cmdwait waits for
         * SBC_IP (csr & 0x4200) and then fails the command outright if
         * csr & 0x4100 (DMA_CONFLICT | DMA_IP) is set -- munix_sun3
         * 0xe067660..0xe06766a, `bne` into its d7=3/si_reset path, which
         * is exactly the "resetting scsi bus" this model used to provoke
         * on every transfer -- and the PROM decodes bit 8 on its own as
         * "dma overrun" (ROM 0xfef8c7a).  So raise it only when the
         * transfer really did end early: the count ran out while the
         * target still had data to hand over.
         */
        m->udc_active = false;
        ncr5380_dma_eop(n);
        if (ncr5380_pdma_ready(n, out)) {
            m->udc_ip = true;
        }
    }
    sun3_si_update_irq(m);
}

/*
 * A DMA transfer takes time on the real board, and the drivers rely on
 * it: SunOS's si_sbc_dma_setup reads the 5380's reset-interrupt register
 * right after writing the DMA start register (MINIROOT kernel pcs
 * 0x0e0704be / 0x0e071eda), which on hardware happens while the first
 * bytes are still moving.  Completing the whole transfer inside the
 * start write would let that read clear the completion interrupt before
 * the driver's poll (sipoll: csr & SBC_IP) ever sees it.  So the engine
 * runs from a timer: a small setup latency plus ~100 ns per byte.
 */
#define SUN3_SI_DMA_LATENCY_NS  20000
#define SUN3_SI_DMA_NS_PER_BYTE 100

static void sun3_si_dma_schedule(Sun3MachineState *m)
{
    int64_t delay;

    if (!m->udc_active || ncr5380_dma_direction(&m->si_sbc) < 0 ||
        timer_pending(&m->si_dma_timer)) {
        return;
    }
    delay = SUN3_SI_DMA_LATENCY_NS +
            (int64_t)m->udc_count * 2 * SUN3_SI_DMA_NS_PER_BYTE;
    timer_mod(&m->si_dma_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay);
}

static void sun3_si_dma_timer_cb(void *opaque)
{
    sun3_si_dma_run(opaque);
}

/* the 5380's DRQ output: a byte is ready on the SCSI side */
static void sun3_si_drq(void *opaque, int n, int level)
{
    Sun3MachineState *m = opaque;

    if (level) {
        sun3_si_dma_schedule(m);
    }
}

static void sun3_si_udc_reset(Sun3MachineState *m)
{
    timer_del(&m->si_dma_timer);
    m->udc_active = false;
    m->udc_count = 0;
    m->udc_ip = false;
}

/* UDC start-chain: fetch the command table from DVMA and arm the channel */
static void sun3_si_udc_start(Sun3MachineState *m)
{
    uint32_t car = ((m->udc_car_hi & 0xff00) << 8) | m->udc_car_lo;
    uint8_t raw[UDC_TBL_WORDS * 2];
    uint16_t tbl[UDC_TBL_WORDS];
    int i;

    if (!sun3_dvma_rw(m, car, raw, sizeof(raw), false)) {
        m->si_csr |= SI_CSR_DMA_BUS_ERR;
        sun3_si_update_irq(m);
        return;
    }
    for (i = 0; i < UDC_TBL_WORDS; i++) {
        tbl[i] = lduw_be_p(&raw[i * 2]);
    }
    m->udc_dma_addr = ((tbl[UDC_TBL_ADDRH] & 0xff00) << 8) |
                      tbl[UDC_TBL_ADDRL];
    m->udc_count = tbl[UDC_TBL_COUNT];
    m->udc_active = true;
    m->udc_ip = false;
    qemu_log_mask(CPU_LOG_MMU,
                  "sun3: si UDC start: table@%06x rsel=%04x addr=%06x "
                  "count=%u words cmr=%02x%02x csr=%04x fifo_count=%u\n",
                  car, tbl[UDC_TBL_RSEL], m->udc_dma_addr, m->udc_count,
                  tbl[UDC_TBL_CMRH], tbl[UDC_TBL_CMRL], m->si_csr,
                  m->si_fifo_count);
    sun3_si_dma_schedule(m);
}

static void sun3_si_udc_write(Sun3MachineState *m, uint16_t val)
{
    switch (m->udc_addr) {
    case UDC_ADR_COMMAND:
        switch (val & 0xff) {
        case UDC_CMD_RESET:
            sun3_si_udc_reset(m);
            sun3_si_update_irq(m);
            break;
        case UDC_CMD_CIE:
            /* completion interrupts are always presented (DMA_IP) */
            break;
        case UDC_CMD_START_CHN:
            sun3_si_udc_start(m);
            break;
        default:
            qemu_log_mask(LOG_UNIMP, "sun3: si UDC command %02x\n",
                          (unsigned)val);
            break;
        }
        break;
    case UDC_ADR_MODE:
        m->udc_mode = val;
        break;
    case UDC_ADR_CAR_HIGH:
        m->udc_car_hi = val;
        break;
    case UDC_ADR_CAR_LOW:
        m->udc_car_lo = val;
        break;
    case UDC_ADR_COUNT:
        m->udc_count = val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "sun3: si UDC reg %02x <- %04x\n",
                      m->udc_addr, (unsigned)val);
        break;
    }
}

static uint16_t sun3_si_udc_read(Sun3MachineState *m)
{
    switch (m->udc_addr) {
    case UDC_ADR_COUNT:
        return m->udc_count;
    case UDC_ADR_MODE:
        return m->udc_mode;
    case UDC_ADR_CAR_HIGH:
        return m->udc_car_hi;
    case UDC_ADR_CAR_LOW:
        return m->udc_car_lo;
    default:
        return 0;
    }
}

static uint16_t sun3_si_csr_read(Sun3MachineState *m)
{
    uint16_t v = m->si_csr & (SI_CSR_RW_BITS | SI_CSR_DMA_BUS_ERR |
                              SI_CSR_DMA_CONFLICT);

    /* the FIFO drains synchronously: never full, always empty */
    v |= SI_CSR_FIFO_EMPTY;
    if (m->si_sbc_irq) {
        v |= SI_CSR_SBC_IP;
    }
    if (m->udc_ip) {
        v |= SI_CSR_DMA_IP;
    }
    if (m->udc_active) {
        v |= SI_CSR_DMA_ACTIVE;
    }
    return v;
}

static void sun3_si_csr_write(Sun3MachineState *m, uint16_t val)
{
    uint16_t old = m->si_csr;

    m->si_csr = (m->si_csr & ~SI_CSR_RW_BITS) | (val & SI_CSR_RW_BITS);
    if (!(val & SI_CSR_SCSI_RES)) {
        /* held in reset: the 5380, the UDC and the error latches */
        if (old & SI_CSR_SCSI_RES) {
            device_cold_reset(DEVICE(&m->si_sbc));
        }
        sun3_si_udc_reset(m);
        m->si_csr &= ~(SI_CSR_DMA_BUS_ERR | SI_CSR_DMA_CONFLICT);
    }
    if (!(val & SI_CSR_FIFO_RES)) {
        m->si_fifo_data = 0;
    }
    sun3_si_update_irq(m);
}

static uint64_t sun3_si_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3MachineState *m = opaque;
    unsigned reg = addr & ~1;
    uint16_t v;

    switch (reg) {
    case SI_UDC_DATA:
        v = sun3_si_udc_read(m);
        break;
    case SI_UDC_ADDR:
        v = m->udc_addr;
        break;
    case SI_FIFO_DATA:
        v = m->si_fifo_data;
        break;
    case SI_FIFO_COUNT:
        v = m->si_fifo_count;
        break;
    case SI_CSR:
        v = sun3_si_csr_read(m);
        break;
    default:
        v = m->si_scratch[reg / 2];
        break;
    }
    if (size == 1) {
        v = (addr & 1) ? v & 0xff : v >> 8;
    }
    qemu_log_mask(CPU_LOG_MMU, "sun3: si rd %02x.%u -> %04x\n",
                  (unsigned)addr, size, v);
    return v;
}

static void sun3_si_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    Sun3MachineState *m = opaque;
    unsigned reg = addr & ~1;
    uint16_t v = val;

    qemu_log_mask(CPU_LOG_MMU, "sun3: si wr %02x.%u <- %04x\n",
                  (unsigned)addr, size, (unsigned)val);
    if (size == 1) {
        uint16_t cur = sun3_si_read(m, reg, 2);

        v = (addr & 1) ? (cur & 0xff00) | (val & 0xff)
                       : (cur & 0x00ff) | ((val & 0xff) << 8);
    }
    switch (reg) {
    case SI_UDC_DATA:
        sun3_si_udc_write(m, v);
        break;
    case SI_UDC_ADDR:
        m->udc_addr = v;
        break;
    case SI_FIFO_DATA:
        m->si_fifo_data = v;
        break;
    case SI_FIFO_COUNT:
        m->si_fifo_count = v;
        break;
    case SI_CSR:
        sun3_si_csr_write(m, v);
        break;
    default:
        m->si_scratch[reg / 2] = v;
        break;
    }
}

/*
 * 16-bit register file; the SunOS si driver also does 32-bit stores to
 * the VME dma_addr/dma_count pair (si_sbc_dma_setup), split into halves.
 */
static const MemoryRegionOps sun3_si_ops = {
    .read = sun3_si_read,
    .write = sun3_si_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .endianness = DEVICE_BIG_ENDIAN,
};

static void sun3_si_reset(Sun3MachineState *m)
{
    m->si_csr = 0;
    m->si_fifo_count = 0;
    m->si_fifo_data = 0;
    memset(m->si_scratch, 0, sizeof(m->si_scratch));
    m->si_sbc_irq = false;
    m->udc_addr = 0;
    m->udc_car_hi = m->udc_car_lo = m->udc_mode = 0;
    m->udc_dma_addr = 0;
    sun3_si_udc_reset(m);
}

/* --- "le" ethernet: Am7990 LANCE at obio 0x120000 --- */

/*
 * The LANCE's two 16-bit registers, RDP at +0 and RAP at +2 (PROM le
 * driver at ROM 0xFEF75DA, NetBSD lewrcsr/lerdcsr, SunOS leinit: all
 * write the CSR number to +2 and the value to +0).  Both firmware and
 * SunOS also reach them with byte cycles: the PROM selects a CSR with
 * `andib #-4,(3,a4)` / `orib #n,(3,a4)` on RAP's low byte (ROM 0xFEF75F4
 * -- the access whose bus error aborted the auto-boot before this device
 * existed), and SunOS's leprobe() is a byte pokec() of 0 to RDP.  On the
 * 68020's dynamically sized bus a byte write to a 16-bit port is one word
 * cycle with the byte duplicated on both lanes, and a byte read a word
 * cycle of which the CPU keeps its lane; that is modelled here on top of
 * the pcnet core's word accessors instead of a read-modify-write, which
 * would write CSR0's write-1-to-clear bits back into the chip.
 */
static uint64_t sun3_lance_read(void *opaque, hwaddr addr, unsigned size)
{
    Sun3MachineState *m = opaque;
    uint32_t val = pcnet_ioport_readw(&m->lance->state, addr & ~1);

    if (size == 1) {
        return (addr & 1) ? val & 0xff : (val >> 8) & 0xff;
    }
    return val & 0xffff;
}

static void sun3_lance_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    Sun3MachineState *m = opaque;

    if (size == 1) {
        val = (val & 0xff) * 0x0101;
    }
    pcnet_ioport_writew(&m->lance->state, addr & ~1, val & 0xffff);
}

static const MemoryRegionOps sun3_lance_ops = {
    .read = sun3_lance_read,
    .write = sun3_lance_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

/*
 * hw/net/pcnet.c's DMA hooks.  The LANCE presents a 24-bit address which
 * the board translates exactly like the si's UDC above: VA 0x0F000000 |
 * addr through the CPU's MMU in the current context (SunOS's kernel heap
 * -- Sysbase 0x0F000000..Syslimit 0x0F806000, where le_alloc_buffers()
 * kmem_zalloc()s the init block, rings and buffers -- and NetBSD's DVMA
 * window 0x0FF00000.. both sit inside it; the PROM's le_xmit uses a
 * buffer in place only if (va & 0x0F000000) == 0x0F000000).  A fault is
 * the chip's error, CSR0 MERR, not a CPU bus error.
 *
 * Byte order: the Sun wiring puts the even (big-endian high) byte of a
 * memory word on DAL15-8, and every driver here sets CSR3 BSWP (= 4).
 * pcnet.c reads descriptors and the init block with do_bswap == 0 and
 * decodes them little-endian, so those get a 16-bit swap (the init
 * block's PADR is stored ea[1],ea[0],ea[3],ea[2].. by the PROM and SunOS
 * alike, which after the swap is the plain address pcnet expects); frame
 * bodies come with do_bswap == BSWP == 1 and pass through byte for byte.
 * Same contract as sun4m's ledma_memory_read/write and the sun3x le.
 */
static void sun3_lance_dma_read(void *opaque, hwaddr addr, uint8_t *buf,
                                int len, int do_bswap)
{
    Sun3MachineState *m = SUN3_MACHINE(qdev_get_machine());
    int i;

    if (!sun3_dvma_rw(m, addr, buf, len, false)) {
        memset(buf, 0, len);
        pcnet_dma_error(&m->lance->state);
        return;
    }
    if (!do_bswap) {
        for (i = 0; i + 1 < len; i += 2) {
            bswap16s((uint16_t *)(buf + i));
        }
    }
}

static void sun3_lance_dma_write(void *opaque, hwaddr addr, uint8_t *buf,
                                 int len, int do_bswap)
{
    Sun3MachineState *m = SUN3_MACHINE(qdev_get_machine());
    g_autofree uint8_t *tmp = NULL;
    int i;

    if (!do_bswap) {
        tmp = g_memdup2(buf, len);
        for (i = 0; i + 1 < len; i += 2) {
            bswap16s((uint16_t *)(tmp + i));
        }
        buf = tmp;
    }
    if (!sun3_dvma_rw(m, addr, buf, len, true)) {
        pcnet_dma_error(&m->lance->state);
    }
}

/* --- IDPROM --- */

static void sun3_build_idprom(Sun3MachineState *m, const MACAddr *mac)
{
    uint8_t *p = m->idprom;
    uint8_t sum;
    int i;

    memset(p, 0, SUN3_IDPROM_SIZE);
    p[0] = SUN3_IDPROM_FORMAT;
    p[1] = SUN3_IDPROM_MACH_3_60;
    memcpy(&p[2], mac->a, 6);
    /* bytes 8..11 date, 12..14 serial number: left zero */
    sum = 0;
    for (i = 0; i < 15; i++) {
        sum ^= p[i];
    }
    p[15] = sum;
}

/* --- on-board bwtwo resolution ID byte --- */

static uint64_t sun3_bw2_id_read(void *opaque, hwaddr addr, unsigned size)
{
    return SUN3_BW2_ID_1152X900;
}

static const MemoryRegionOps sun3_bw2_id_ops = {
    .read = sun3_bw2_id_read,
    .write = sun3_zero_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_BIG_ENDIAN,
};

/* --- machine --- */


/* --- -kernel: the synthesised PROM and the boot loader's maps --- */

/*
 * The Linux/sun3 kernel is booted on real hardware by a boot program the
 * PROM loaded: it runs with the MMU on, in context 0, through the low
 * identity map the monitor leaves (sun3-head.S copies the segment maps
 * from VA 0 up to the first invalid PMEG to KERNBASE and jumps high),
 * and it keeps calling into the resident PROM for the rest of its life:
 * config.c reads pv_sun3mem and pv_monid, hvc_sun3 and the early console
 * use pv_nbputchar/pv_nbgetchar, mmu_emu_init() copies context 0's
 * segment map into the other seven contexts through pv_setctxt, and
 * halt/reboot call pv_halt/pv_reboot.
 *
 * Without -bios the machine therefore stands in for the monitor's
 * power-on state: a 64KB EPROM image whose first 0xE0 bytes are the
 * romvec (as in the genuine ROM, whose vector table starts at offset 0 =
 * VA 0x0FEF0000) followed by hand-assembled stubs, and a segment/page map
 * preloaded at reset with the three things the kernel relies on: RAM
 * identity-mapped in PMEG n = physical segment n (the monitor maps 8MB,
 * here all of RAM), the PROM's device pages in PMEG 254 at VA 0x0FE00000
 * (the kernel addresses the clock at 0x0FE06000 and the interrupt
 * register at 0x0FE0A000 directly) and the EPROM in PMEG 247 at
 * 0x0FEF0000.  No RAM globals are needed: pv_sun3mem and pv_monid point
 * into the EPROM image.  All of it is data the PROM would have left
 * behind; the code path is the kernel's own, with the same entry (physical
 * 0x2000, enable register NOTBOOT|SDVMA) the monitor's `g` command gives.
 */
enum {
    RV_INITSP       = 0x00,
    RV_STARTMON     = 0x04,
    RV_MEMSIZE      = 0x10,     /* pv_sun3mem */
    RV_GETCHAR      = 0x14,
    RV_PUTCHAR      = 0x18,
    RV_NBGETCHAR    = 0x1C,
    RV_NBPUTCHAR    = 0x20,
    RV_ECHO         = 0x24,
    RV_INSOURCE     = 0x28,
    RV_OUTSINK      = 0x2C,
    RV_MONID        = 0x4C,
    RV_REBOOT       = 0x60,
    RV_ROMVERS      = 0xA4,
    RV_MEMORYAVAIL  = 0xB8,
    RV_HALT         = 0xC4,
    RV_SETCTXT      = 0xCC,     /* pv_setctxt / setcxsegmap */
    RV_SIZE         = 0xE0,
};

/* offsets of the stubs and data inside the synthesised EPROM image */
enum {
    PROM_PUTCHAR    = 0x0100,
    PROM_GETCHAR    = 0x0140,
    PROM_SETCTXT    = 0x0180,
    PROM_HALT       = 0x0240,
    PROM_MEMSIZE    = 0x0300,
    PROM_CONS       = 0x0304,   /* echo, insource, outsink bytes */
    PROM_MONID      = 0x0310,
};

/*
 * pv_putchar/pv_nbputchar(int ch): ttya is zs0 channel A, whose PROM page
 * is VA 0x0FE02000 (obio 0x20000): csr +4, data +6.  The ESCC transmits
 * nothing until WR5.TXEN is set, so enable it every time (the monitor's
 * zs init never ran), then wait for RR0.TXEMPTY (bit 2).  The genuine
 * routine (ROM 0x0ECE) returns -1 instead of waiting; Linux's
 * prom_putchar() retries either way.
 */
static const uint8_t sun3_prom_putchar[] = {
    0x13, 0xfc, 0x00, 0x05, 0x0f, 0xe0, 0x20, 0x04, /* move.b #5,csr */
    0x13, 0xfc, 0x00, 0x68, 0x0f, 0xe0, 0x20, 0x04, /* move.b #0x68,csr */
    0x12, 0x39, 0x0f, 0xe0, 0x20, 0x04,             /* 1: move.b csr,d1 */
    0x08, 0x01, 0x00, 0x02,                         /* btst #2,d1 */
    0x67, 0xf4,                                     /* beq.s 1b */
    0x12, 0x2f, 0x00, 0x07,                         /* move.b 7(sp),d1 */
    0x13, 0xc1, 0x0f, 0xe0, 0x20, 0x06,             /* move.b d1,data */
    0x70, 0x00,                                     /* moveq #0,d0 */
    0x4e, 0x75,                                     /* rts */
};

/* pv_getchar/pv_nbgetchar(): the character, or -1 if none is waiting */
static const uint8_t sun3_prom_getchar[] = {
    0x13, 0xfc, 0x00, 0x03, 0x0f, 0xe0, 0x20, 0x04, /* move.b #3,csr */
    0x13, 0xfc, 0x00, 0xc1, 0x0f, 0xe0, 0x20, 0x04, /* move.b #0xc1,csr */
    0x12, 0x39, 0x0f, 0xe0, 0x20, 0x04,             /* move.b csr,d1 */
    0x08, 0x01, 0x00, 0x00,                         /* btst #0,d1 */
    0x66, 0x04,                                     /* bne.s 1f */
    0x70, 0xff,                                     /* moveq #-1,d0 */
    0x4e, 0x75,                                     /* rts */
    0x10, 0x39, 0x0f, 0xe0, 0x20, 0x06,             /* 1: move.b data,d0 */
    0x02, 0x80, 0x00, 0x00, 0x00, 0xff,             /* andi.l #0xff,d0 */
    0x4e, 0x75,                                     /* rts */
};

/*
 * pv_setctxt(int ctxt, char *va, int pmeg): write one segment map entry
 * of another context.  Transcribed from the genuine setcxsegmap (ROM
 * 0x0AC8): DFC = 3, then clear the enable register's NOTBOOT bit so that
 * instruction fetches keep coming from the EPROM whatever the target
 * context maps (the redirect decodes the low 16 address bits, which are
 * the EPROM offset of this very routine), switch context, store the PMEG,
 * switch back, restore the enable register and DFC.  Nothing touches the
 * stack between the two context switches.
 */
static const uint8_t sun3_prom_setctxt[] = {
    0x2f, 0x02,                                     /* move.l d2,-(sp) */
    0x2f, 0x03,                                     /* move.l d3,-(sp) */
    0x4e, 0x7a, 0x90, 0x01,                         /* movec dfc,a1 */
    0x72, 0x03,                                     /* moveq #3,d1 */
    0x4e, 0x7b, 0x10, 0x01,                         /* movec d1,dfc */
    0x20, 0x6f, 0x00, 0x10,                         /* movea.l 16(sp),a0 */
    0xd1, 0xfc, 0x20, 0x00, 0x00, 0x00,             /* adda.l #AC_SEGMAP,a0 */
    0x20, 0x2f, 0x00, 0x14,                         /* move.l 20(sp),d0 */
    0x12, 0x2f, 0x00, 0x0f,                         /* move.b 15(sp),d1 */
    0x0e, 0x39, 0x20, 0x00, 0x40, 0x00, 0x00, 0x00, /* moves.b ENABLE,d2 */
    0x36, 0x3c, 0xff, 0x7f,                         /* move.w #0xff7f,d3 */
    0xc6, 0x02,                                     /* and.b d2,d3 */
    0x0e, 0x39, 0x38, 0x00, 0x40, 0x00, 0x00, 0x00, /* moves.b d3,ENABLE */
    0x0e, 0x39, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00, /* moves.b CONTEXT,d3 */
    0x0e, 0x39, 0x18, 0x00, 0x30, 0x00, 0x00, 0x00, /* moves.b d1,CONTEXT */
    0x0e, 0x10, 0x08, 0x00,                         /* moves.b d0,(a0) */
    0x0e, 0x39, 0x38, 0x00, 0x30, 0x00, 0x00, 0x00, /* moves.b d3,CONTEXT */
    0x0e, 0x39, 0x28, 0x00, 0x40, 0x00, 0x00, 0x00, /* moves.b d2,ENABLE */
    0x4e, 0x7b, 0x90, 0x01,                         /* movec a1,dfc */
    0x26, 0x1f,                                     /* move.l (sp)+,d3 */
    0x24, 0x1f,                                     /* move.l (sp)+,d2 */
    0x4e, 0x75,                                     /* rts */
};

/* pv_halt / pv_reboot */
static const uint8_t sun3_prom_halt[] = {
    0x60, 0xfe,                                     /* bra.s . */
};

static void sun3_build_prom(Sun3MachineState *m, uint32_t ram_size)
{
    uint8_t *p = memory_region_get_ram_ptr(&m->rom);

    memset(p, 0, SUN3_ROM_SIZE);

    /* the vector table, VA 0x0FEF0000 + offset */
    stl_be_p(p + RV_INITSP,      m->reset_sp);
    stl_be_p(p + RV_STARTMON,    m->reset_pc);
    stl_be_p(p + RV_MEMSIZE,     SUN3_PROM_VA + PROM_MEMSIZE);
    stl_be_p(p + RV_GETCHAR,     SUN3_PROM_VA + PROM_GETCHAR);
    stl_be_p(p + RV_PUTCHAR,     SUN3_PROM_VA + PROM_PUTCHAR);
    stl_be_p(p + RV_NBGETCHAR,   SUN3_PROM_VA + PROM_GETCHAR);
    stl_be_p(p + RV_NBPUTCHAR,   SUN3_PROM_VA + PROM_PUTCHAR);
    stl_be_p(p + RV_ECHO,        SUN3_PROM_VA + PROM_CONS + 0);
    stl_be_p(p + RV_INSOURCE,    SUN3_PROM_VA + PROM_CONS + 1);
    stl_be_p(p + RV_OUTSINK,     SUN3_PROM_VA + PROM_CONS + 2);
    stl_be_p(p + RV_MONID,       SUN3_PROM_VA + PROM_MONID);
    stl_be_p(p + RV_REBOOT,      SUN3_PROM_VA + PROM_HALT);
    stl_be_p(p + RV_ROMVERS,     2);
    stl_be_p(p + RV_MEMORYAVAIL, SUN3_PROM_VA + PROM_MEMSIZE);
    stl_be_p(p + RV_HALT,        SUN3_PROM_VA + PROM_HALT);
    stl_be_p(p + RV_SETCTXT,     SUN3_PROM_VA + PROM_SETCTXT);

    memcpy(p + PROM_PUTCHAR, sun3_prom_putchar, sizeof(sun3_prom_putchar));
    memcpy(p + PROM_GETCHAR, sun3_prom_getchar, sizeof(sun3_prom_getchar));
    memcpy(p + PROM_SETCTXT, sun3_prom_setctxt, sizeof(sun3_prom_setctxt));
    memcpy(p + PROM_HALT,    sun3_prom_halt,    sizeof(sun3_prom_halt));

    stl_be_p(p + PROM_MEMSIZE, ram_size);
    p[PROM_CONS + 0] = 0;       /* echo off */
    p[PROM_CONS + 1] = 1;       /* inSource: ttya */
    p[PROM_CONS + 2] = 1;       /* outSink: ttya */
    strcpy((char *)p + PROM_MONID, "QEMU");
}

/*
 * The maps the boot loader leaves for sun3-head.S (context 0; the
 * others are copied by the kernel through pv_setctxt): every segment
 * invalid (0xFF, which is what stops head.S's copy loop) except RAM
 * identity-mapped with PMEG n = physical segment n, the PROM's device
 * pages (its map table at ROM 0xD350: VA 0x0FE00000 + 2 * obio page
 * for the first seven on-board devices, si at 0x0FE12000) in PMEG 254
 * and the EPROM in PMEG 247.
 */
static void sun3_kernel_boot_maps(Sun3MachineState *m, uint32_t ram_size)
{
    static const uint32_t io_pages[][2] = {
        { 0x0FE00000, SUN3_OBIO_ZS_KBD_MS },
        { 0x0FE02000, SUN3_OBIO_ZS_TTY },
        { 0x0FE04000, SUN3_OBIO_EEPROM },
        { 0x0FE06000, SUN3_OBIO_CLOCK },
        { 0x0FE08000, SUN3_OBIO_MEMERR },
        { 0x0FE0A000, SUN3_OBIO_INTREG },
        { 0x0FE10000, SUN3_OBIO_LANCE },
        { 0x0FE12000, SUN3_OBIO_SI },
        { 0x0FE16000, SUN3_OBIO_MEMREG },
    };
    unsigned nseg = ram_size >> SUN3_SEG_BITS;
    unsigned seg, i;

    memset(m->segmap, SUN3_INVALID_PMEG, sizeof(m->segmap));
    memset(m->pgmap, 0, sizeof(m->pgmap));

    for (seg = 0; seg < nseg; seg++) {
        m->segmap[0][seg] = seg;
        for (i = 0; i < SUN3_PAGES_PER_SEG; i++) {
            m->pgmap[seg * SUN3_PAGES_PER_SEG + i] =
                SUN3_PROM_PTE_RAM | (seg * SUN3_PAGES_PER_SEG + i);
        }
    }

    m->segmap[0][SUN3_IO_VA >> SUN3_SEG_BITS] = SUN3_PMEG_IO;
    for (i = 0; i < ARRAY_SIZE(io_pages); i++) {
        unsigned page = (io_pages[i][0] >> SUN3_PAGE_BITS) &
                        (SUN3_PAGES_PER_SEG - 1);

        m->pgmap[SUN3_PMEG_IO * SUN3_PAGES_PER_SEG + page] =
            SUN3_PROM_PTE_IO | (io_pages[i][1] >> SUN3_PAGE_BITS);
    }

    m->segmap[0][SUN3_PROM_VA >> SUN3_SEG_BITS] = SUN3_PMEG_ROM;
    for (i = 0; i < SUN3_ROM_SIZE / SUN3_PAGE_SIZE; i++) {
        unsigned page = ((SUN3_PROM_VA >> SUN3_PAGE_BITS) + i) &
                        (SUN3_PAGES_PER_SEG - 1);

        m->pgmap[SUN3_PMEG_ROM * SUN3_PAGES_PER_SEG + page] =
            SUN3_PROM_PTE_IO | ((SUN3_OBIO_EPROM >> SUN3_PAGE_BITS) + i);
    }
}

static uint64_t sun3_kernel_translate(void *opaque, uint64_t addr)
{
    return addr - SUN3_KERNBASE;
}

/*
 * Load the ELF like sun3-tools/boot-linux.py does: every PT_LOAD at
 * p_vaddr - KERNBASE (load_elf zero-fills the BSS tail, which
 * sun3-head.S never does), the bootinfo at _end, entry at
 * e_entry - KERNBASE.  Only BI_MACHTYPE is consumed by the sun3 kernel:
 * config_sun3() takes the memory size from pv_sun3mem and overwrites
 * the BI_MEMCHUNK record; BI_COMMAND_LINE is replaced by
 * CONFIG_BOOTPARAM_STRING when that is configured (sun3-linux.config sets
 * "console=hvc0"); BI_RAMDISK is generic.
 */
static void sun3_load_kernel(Sun3MachineState *m, MachineState *machine)
{
    CPUState *cs = CPU(m->cpu);
    uint64_t elf_entry, high;
    ssize_t kernel_size;
    hwaddr top = machine->ram_size - 2 * SUN3_PAGE_SIZE; /* PROM pages */
    hwaddr bootinfo_base;
    void *blob, *ptr;

    kernel_size = load_elf(machine->kernel_filename, NULL,
                           sun3_kernel_translate, NULL, &elf_entry, NULL,
                           &high, NULL, ELFDATA2MSB, EM_68K, 0, 0);
    if (kernel_size < 0) {
        error_report("sun3-60: could not load kernel '%s'",
                     machine->kernel_filename);
        exit(1);
    }
    if (high > top) {
        error_report("sun3-60: kernel '%s' does not fit in %u MiB of RAM",
                     machine->kernel_filename,
                     (unsigned)(machine->ram_size / MiB));
        exit(1);
    }
    m->reset_pc = elf_entry - SUN3_KERNBASE;
    m->reset_sp = m->reset_pc;      /* unused: head.S loads its own */

    bootinfo_base = QEMU_ALIGN_UP(high, 4);
    blob = g_malloc(1024 + (machine->kernel_cmdline ?
                            strlen(machine->kernel_cmdline) : 0));
    ptr = blob;
    BOOTINFO1(ptr, BI_MACHTYPE, MACH_SUN3);
    BOOTINFO1(ptr, BI_CPUTYPE, CPU_68020);
    BOOTINFO1(ptr, BI_FPUTYPE, FPU_68881);
    BOOTINFO1(ptr, BI_MMUTYPE, MMU_SUN3);
    if (machine->kernel_cmdline) {
        BOOTINFOSTR(ptr, BI_COMMAND_LINE, machine->kernel_cmdline);
    }
    if (machine->initrd_filename) {
        int64_t initrd_size = get_image_size(machine->initrd_filename, NULL);
        hwaddr initrd_base;

        if (initrd_size < 0) {
            error_report("sun3-60: could not load initial ram disk '%s'",
                         machine->initrd_filename);
            exit(1);
        }
        initrd_base = (top - initrd_size) & ~(hwaddr)SUN3_PAGE_MASK;
        if (initrd_base < bootinfo_base + 1024) {
            error_report("sun3-60: initial ram disk '%s' does not fit",
                         machine->initrd_filename);
            exit(1);
        }
        load_image_targphys(machine->initrd_filename, initrd_base,
                            top - initrd_base, &error_fatal);
        BOOTINFO2(ptr, BI_RAMDISK, initrd_base, initrd_size);
    }
    BOOTINFO0(ptr, BI_LAST);
    rom_add_blob_fixed_as("bootinfo", blob, ptr - blob, bootinfo_base,
                          cs->as);
    g_free(blob);
}

static void sun3_machine_reset(void *opaque)
{
    Sun3MachineState *m = opaque;
    CPUState *cs = CPU(m->cpu);

    memset(m->segmap, 0, sizeof(m->segmap));
    memset(m->pgmap, 0, sizeof(m->pgmap));
    m->context = 0;
    m->enable = 0;              /* boot state */
    m->buserr = 0;
    m->diag = 0;
    m->udvma = 0;
    m->intreg = 0;
    m->irq_sources = 0;
    memset(m->clock_latch, 0, sizeof(m->clock_latch));
    m->video_latch = false;
    timer_del(&m->video_timer);
    qemu_set_irq(m->video_enable, 0);
    m->memerr_csr = 0;
    if (m->parity_poison) {
        g_hash_table_remove_all(m->parity_poison);
        sun3_parity_overlay_update(m);
    }
    sun3_si_reset(m);
    if (m->kernel_boot) {
        /* the state the monitor's `g` command would hand the kernel */
        sun3_kernel_boot_maps(m, MACHINE(m)->ram_size);
        m->enable = SUN3_ENA_NOTBOOT | SUN3_ENA_SDVMA;
    }

    cpu_reset(cs);
    /*
     * In the boot state the reset vector fetch is redirected to the
     * EPROM like every other supervisor program access.
     */
    m->cpu->env.aregs[7] = m->reset_sp;
    m->cpu->env.pc = m->reset_pc;
}

static void sun3_init(MachineState *machine)
{
    Sun3MachineState *m = SUN3_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    const char *bios_name = machine->firmware ?: "3.60_v3.0.1_rom";
    g_autofree char *filename = NULL;
    MACAddr mac = { .a = { 0x08, 0x00, 0x20, 0x36, 0x00, 0x60 } };
    DeviceState *dev;
    SysBusDevice *sbd;
    uint8_t rom_header[8];
    int i;

    if (machine->ram_size > SUN3_RAM_MAX) {
        error_report("sun3-60: maximum RAM size is 24MB");
        exit(1);
    }

    m->cpu = M68K_CPU(cpu_create(machine->cpu_type));
    m68k_set_fc_ops(m->cpu, &sun3_fc_ops, m);
    qemu_register_reset(sun3_machine_reset, m);

    /* bus-timeout backgrounds for the four PTE type spaces */
    for (i = 0; i < SUN3_NUM_TYPES; i++) {
        static const char *names[SUN3_NUM_TYPES] = {
            "sun3.obmem-bg", "sun3.obio-bg", "sun3.vme16-bg", "sun3.vme32-bg"
        };
        memory_region_init_io(&m->type_bg[i], OBJECT(machine), &sun3_bg_ops,
                              m, names[i], 4 * GiB);
        memory_region_add_subregion_overlap(sysmem, SUN3_TYPE_SPACE(i),
                                            &m->type_bg[i], -1);
    }

    /* RAM */
    memory_region_add_subregion(sysmem, SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM),
                                machine->ram);

    /* boot EPROM */
    memory_region_init_rom(&m->rom, NULL, "sun3.eprom",
                           SUN3_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                SUN3_OBIO_EPROM, &m->rom);
    if (machine->kernel_filename && machine->firmware) {
        error_report("sun3-60: -kernel boots without the PROM; "
                     "use either -kernel or -bios");
        exit(1);
    }
    m->kernel_boot = machine->kernel_filename != NULL;
    if (m->kernel_boot) {
        sun3_load_kernel(m, machine);
        sun3_build_prom(m, machine->ram_size);
    } else {
        filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
        if (!filename ||
            load_image_size(filename, memory_region_get_ram_ptr(&m->rom),
                            SUN3_ROM_SIZE) <= 0) {
            error_report("sun3-60: cannot load boot PROM '%s'", bios_name);
            exit(1);
        }
        memcpy(rom_header, memory_region_get_ram_ptr(&m->rom),
               sizeof(rom_header));
        m->reset_sp = ldl_be_p(&rom_header[0]);
        m->reset_pc = ldl_be_p(&rom_header[4]);
    }
    for (i = 0; i < ARRAY_SIZE(m->rom_obmem); i++) {
        memory_region_init_alias(&m->rom_obmem[i], NULL, "sun3.eprom-obmem",
                                 &m->rom, 0, SUN3_ROM_SIZE);
        memory_region_add_subregion(sysmem,
                                    SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM) +
                                    SUN3_OBMEM_EPROM + i * SUN3_ROM_SIZE,
                                    &m->rom_obmem[i]);
    }

    /*
     * On-board bwtwo: 128KB of video RAM at obmem 0xFF000000, mirrored
     * to fill the 256KB window the PROM maps (and clears); the display
     * enable is driven from the system enable register's VIDEO bit
     * (see hw/display/sun-bwtwo.c, "the on-board Sun-3/60 variant").
     */
    m->bwtwo = qdev_new("sun-bwtwo");
    qdev_prop_set_bit(m->bwtwo, "p4", false);
    sbd = SYS_BUS_DEVICE(m->bwtwo);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM) +
                            SUN3_OBMEM_VRAM);
    sysbus_mmio_map(sbd, 1, SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM) +
                            SUN3_OBMEM_VRAM + SUN3_OBMEM_VRAM_SIZE);
    m->video_enable = qdev_get_gpio_in_named(m->bwtwo, "video-enable", 0);
    timer_init_ns(&m->video_timer, QEMU_CLOCK_VIRTUAL,
                  sun3_video_timer_cb, m);
    memory_region_init_io(&m->bw2_id_mr, OBJECT(machine), &sun3_bw2_id_ops,
                          m, "sun3.bw2-id", 1);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM) +
                                SUN3_OBMEM_BW2_ID, &m->bw2_id_mr);

    /* EEPROM (configuration store; zeroed => PROM falls back to defaults) */
    memory_region_init_ram(&m->eeprom, NULL, "sun3.eeprom",
                           SUN3_EEPROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                SUN3_OBIO_EEPROM, &m->eeprom);
    /*
     * Byte 0x1F selects the console: 0x00 = screen + keyboard (the PROM
     * then probes the bwtwo and the keyboard and sets its romvec
     * inSource/outSink to 0, which SunOS's consconfig() follows), 0x10 =
     * ttya.  The "sunos-console" machine property chooses; ttya is the
     * default so serial-driven runs are unchanged.
     */
    ((uint8_t *)memory_region_get_ram_ptr(&m->eeprom))[SUN3_EEPROM_CONSOLE] =
        m->console_fb ? SUN3_EEPROM_CONS_BW : SUN3_EEPROM_CONS_TTYA;

    /* interrupt register and glue */
    m->irqs = qemu_allocate_irqs(sun3_set_irq, m, SUN3_IRQ_NUM_SRCS);
    memory_region_init_io(&m->intreg_mr, OBJECT(machine), &sun3_intreg_ops,
                          m, "sun3.intreg", SUN3_PAGE_SIZE);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                SUN3_OBIO_INTREG, &m->intreg_mr);

    /* memory error registers and the bad-parity RAM overlay */
    memory_region_init_io(&m->memerr_mr, OBJECT(machine), &sun3_memerr_ops,
                          m, "sun3.memerr", SUN3_PAGE_SIZE);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                SUN3_OBIO_MEMERR, &m->memerr_mr);
    m->parity_poison = g_hash_table_new(NULL, NULL);
    address_space_init(&m->ram_as, machine->ram, "sun3.ram-as");
    memory_region_init_io(&m->parity_overlay, OBJECT(machine),
                          &sun3_parity_ops, m, "sun3.parity-overlay",
                          machine->ram_size);
    memory_region_set_enabled(&m->parity_overlay, false);
    memory_region_add_subregion_overlap(sysmem,
                                        SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM),
                                        &m->parity_overlay, 1);
    memory_region_init_io(&m->memreg_mr, OBJECT(machine), &sun3_stub_ops,
                          m, "sun3.memreg", SUN3_PAGE_SIZE);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                SUN3_OBIO_MEMREG, &m->memreg_mr);

    /* zs0: ttya/ttyb */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", SUN3_ZS_FREQ);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    qdev_prop_set_uint32(dev, "chnAtype", escc_serial);
    qdev_prop_set_uint32(dev, "chnBtype", escc_serial);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                            SUN3_OBIO_ZS_TTY);
    sysbus_connect_irq(sbd, 0, m->irqs[SUN3_IRQ_SRC_ZS_TTY_B]);
    sysbus_connect_irq(sbd, 1, m->irqs[SUN3_IRQ_SRC_ZS_TTY_A]);
    /*
     * Carrier is hardwired on the Sun-3 serial ports; without it the
     * NetBSD console open sleeps in the carrier wait forever (the
     * chardev backend may look disconnected, e.g. a listening socket).
     */
    qemu_irq_raise(qdev_get_gpio_in_named(dev, "dcd", 0));
    qemu_irq_raise(qdev_get_gpio_in_named(dev, "dcd", 1));

    /*
     * zs1: Sun keyboard on channel A (csr +4, data +6; the PROM's
     * keyboard init at 0xfef3fb2 programs it for 1200 baud and sends
     * the reset command 0x01 until the 0xFF <id> reply arrives, then
     * records the id in its romvec keyBid), mouse on channel B.  Same
     * escc_kbd/escc_mouse channel types and PCLK as sun4m and sun3x;
     * the keyboard answers as a Type 4 with layout 0 (US), which is
     * what SunOS 4.1.1's OpenWindows 2.0 accepts.  Idle under
     * -nographic, like sun3x.
     */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", !machine->enable_graphics);
    qdev_prop_set_uint32(dev, "frequency", SUN3_ZS_FREQ);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_chr(dev, "chrA", NULL);
    qdev_prop_set_chr(dev, "chrB", NULL);
    qdev_prop_set_uint32(dev, "chnAtype", escc_kbd);
    qdev_prop_set_uint32(dev, "chnBtype", escc_mouse);
    qdev_prop_set_string(dev, "chnA-sunkbd-layout", "0");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                            SUN3_OBIO_ZS_KBD_MS);
    sysbus_connect_irq(sbd, 0, m->irqs[SUN3_IRQ_SRC_ZS_KBD_B]);
    sysbus_connect_irq(sbd, 1, m->irqs[SUN3_IRQ_SRC_ZS_KBD_A]);

    /* Intersil ICM7170 clock */
    dev = qdev_new(TYPE_ICM7170);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                            SUN3_OBIO_CLOCK);
    sysbus_connect_irq(sbd, 0,
                       qemu_allocate_irq(sun3_clock_irq, m, 0));

    /*
     * si SCSI: the NCR5380 (byte-spaced registers) on top of the DMA/CSR
     * block, interrupt level 2 through the machine's per-source line.
     */
    object_initialize_child(OBJECT(machine), "si", &m->si_sbc, TYPE_NCR5380);
    qdev_prop_set_uint8(DEVICE(&m->si_sbc), "reg-shift", 0);
    sysbus_realize(SYS_BUS_DEVICE(&m->si_sbc), &error_fatal);
    memory_region_init_io(&m->si_mr, OBJECT(machine), &sun3_si_ops, m,
                          "sun3.si", SUN3_SI_SIZE);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                SUN3_OBIO_SI, &m->si_mr);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&m->si_sbc), 0,
                            SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) + SUN3_OBIO_SI,
                            1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->si_sbc), 0,
                       qemu_allocate_irq(sun3_si_sbc_irq, m, 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&m->si_sbc), 1,
                       qemu_allocate_irq(sun3_si_drq, m, 0));
    timer_init_ns(&m->si_dma_timer, QEMU_CLOCK_VIRTUAL,
                  sun3_si_dma_timer_cb, m);
    scsi_bus_legacy_handle_cmdline(&m->si_sbc.bus);

    /*
     * "le" LANCE ethernet at obio 0x120000, interrupt level 3 through its
     * own per-source line (see sun3_lance_ops / sun3_lance_dma_read above
     * for the register and DVMA contracts).  Modelled only when a NIC of
     * model "lance" is configured (-nic user,model=lance): without one
     * the PROM's auto-boot keeps bus-erroring on its le probe and falling
     * into the monitor as before, which the kernel-injection tooling
     * relies on -- with the chip present a diskless auto-boot instead
     * RARPs forever ("Requesting Internet address for ...").  An
     * implicit default NIC has no model and so does not count; neither
     * does a plain "-nic user".  The guest never asks the chip for its
     * address: the PROM, SunOS (localetheraddr) and NetBSD all take it
     * from the IDPROM and program it into the init block themselves, so
     * the qdev "mac" is forced to the IDPROM address to keep the two (and
     * `info network`) consistent.
     */
    {
        NICInfo *nd = qemu_find_nic_info(TYPE_LANCE, false, NULL);

        if (nd) {
            dev = qdev_new(TYPE_LANCE);
            qdev_set_nic_properties(dev, nd);
            qdev_prop_set_macaddr(dev, "mac", mac.a);
            sbd = SYS_BUS_DEVICE(dev);
            sysbus_realize_and_unref(sbd, &error_fatal);
            m->lance = SYSBUS_PCNET(dev);
            m->lance->state.phys_mem_read = sun3_lance_dma_read;
            m->lance->state.phys_mem_write = sun3_lance_dma_write;
            memory_region_init_io(&m->lance_mr, OBJECT(machine),
                                  &sun3_lance_ops, m, "sun3.lance", 4);
            memory_region_add_subregion(sysmem,
                                        SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                        SUN3_OBIO_LANCE, &m->lance_mr);
            sysbus_connect_irq(sbd, 0, m->irqs[SUN3_IRQ_SRC_LANCE]);
        }
    }

    sun3_build_idprom(m, &mac);
}

static char *sun3_get_sunos_console(Object *obj, Error **errp)
{
    Sun3MachineState *m = SUN3_MACHINE(obj);

    return g_strdup(m->console_fb ? "fb" : "ttya");
}

static void sun3_set_sunos_console(Object *obj, const char *value,
                                   Error **errp)
{
    Sun3MachineState *m = SUN3_MACHINE(obj);

    if (!strcmp(value, "ttya")) {
        m->console_fb = false;
    } else if (!strcmp(value, "fb")) {
        m->console_fb = true;
    } else {
        error_setg(errp,
                   "Invalid sunos-console value '%s': must be 'ttya' or 'fb'",
                   value);
    }
}

static void sun3_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Sun-3/60 (Ferrari)";
    mc->init = sun3_init;
    mc->default_cpu_type = M68K_CPU_TYPE_NAME("m68020");
    mc->default_ram_size = 16 * MiB;
    mc->default_ram_id = "sun3.ram";
    mc->max_cpus = 1;

    object_class_property_add_str(oc, "sunos-console",
                                  sun3_get_sunos_console,
                                  sun3_set_sunos_console);
    object_class_property_set_description(oc, "sunos-console",
        "Console selected by EEPROM byte 0x1F: 'ttya' (default, serial) "
        "or 'fb' (on-board bwtwo screen + Sun keyboard/mouse).");
}

static const TypeInfo sun3_machine_typeinfo = {
    .name = TYPE_SUN3_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Sun3MachineState),
    .class_init = sun3_machine_class_init,
};

static void sun3_machine_register_types(void)
{
    type_register_static(&sun3_machine_typeinfo);
}

type_init(sun3_machine_register_types)
