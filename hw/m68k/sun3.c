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
 *   0xFF000000 P4 bwtwo framebuffer
 *
 * Accesses that decode to nothing latch the TIMEOUT bit in the bus
 * error register and terminate in a bus error, which is how the PROM
 * sizes memory and probes for optional devices.
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
#include "system/system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/char/escc.h"
#include "hw/rtc/icm7170.h"
#include "target/m68k/cpu.h"

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
 * 3/60 on-board devices in the high type-0 (obmem) space, from the
 * monitor's device map table at ROM offset 0xD350:
 * 0xFF000000 video RAM (the monitor also runs its early stack here),
 * 0xFF600000 the boot EPROM again (128KB window, ROM mirrored).
 */
#define SUN3_OBMEM_VRAM         0xFF000000
#define SUN3_OBMEM_VRAM_SIZE    (256 * KiB)
#define SUN3_OBMEM_EPROM        0xFF600000

#define SUN3_ROM_SIZE           (64 * KiB)
#define SUN3_EEPROM_SIZE        (2 * KiB)
/* EEPROM configuration bytes the PROM honours */
#define SUN3_EEPROM_CONSOLE     0x1F
#define SUN3_EEPROM_CONS_BW     0x00
#define SUN3_EEPROM_CONS_TTYA   0x10
#define SUN3_EEPROM_CONS_TTYB   0x11
#define SUN3_RAM_MAX            (24 * MiB)

/* interrupt register bits (PROM writes 0x21 = enable-all + clock-5) */
#define SUN3_IREG_ALL_ENAB      0x01
#define SUN3_IREG_SOFT_1        0x02
#define SUN3_IREG_SOFT_2        0x04
#define SUN3_IREG_SOFT_3        0x08
#define SUN3_IREG_VIDEO_ENAB    0x10
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
    MemoryRegion vram;
    MemoryRegion eeprom;
    MemoryRegion intreg_mr;
    MemoryRegion memerr_mr;
    MemoryRegion memreg_mr;
    qemu_irq *irqs;             /* per-source device inputs */

    uint32_t reset_sp;
    uint32_t reset_pc;
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
    bool clock_latch;           /* board latch fed by the ICM7170 INT pin */

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
    if (m->clock_latch && (m->intreg & SUN3_IREG_CLOCK_ENAB_5)) {
        pend |= 1 << SUN3_IPL_CLOCK;
    }
    if (m->clock_latch && (m->intreg & SUN3_IREG_CLOCK_ENAB_7)) {
        pend |= 1 << SUN3_IPL_CLOCK_NMI;
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
     * The board latches the ICM7170's interrupt output; the OS clears
     * the latch by momentarily dropping the clock enable bit in the
     * interrupt register (it also reads the 7170's status register,
     * which drops this line again).
     */
    if (level) {
        m->clock_latch = true;
    }
    sun3_irq_update(m);
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
    if (!(val & SUN3_IREG_CLOCK_ENAB_5) && !(val & SUN3_IREG_CLOCK_ENAB_7)) {
        m->clock_latch = false;
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

/* --- machine --- */

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
    m->clock_latch = false;
    m->memerr_csr = 0;
    if (m->parity_poison) {
        g_hash_table_remove_all(m->parity_poison);
        sun3_parity_overlay_update(m);
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
    for (i = 0; i < ARRAY_SIZE(m->rom_obmem); i++) {
        memory_region_init_alias(&m->rom_obmem[i], NULL, "sun3.eprom-obmem",
                                 &m->rom, 0, SUN3_ROM_SIZE);
        memory_region_add_subregion(sysmem,
                                    SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM) +
                                    SUN3_OBMEM_EPROM + i * SUN3_ROM_SIZE,
                                    &m->rom_obmem[i]);
    }

    /* on-board video RAM (also the monitor's early work RAM/stack) */
    memory_region_init_ram(&m->vram, NULL, "sun3.vram",
                           SUN3_OBMEM_VRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBMEM) +
                                SUN3_OBMEM_VRAM, &m->vram);

    /* EEPROM (configuration store; zeroed => PROM falls back to defaults) */
    memory_region_init_ram(&m->eeprom, NULL, "sun3.eeprom",
                           SUN3_EEPROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem,
                                SUN3_TYPE_SPACE(SUN3_TYPE_OBIO) +
                                SUN3_OBIO_EEPROM, &m->eeprom);
    /* console on ttya until there is a display+keyboard to talk to */
    ((uint8_t *)memory_region_get_ram_ptr(&m->eeprom))[SUN3_EEPROM_CONSOLE] =
        SUN3_EEPROM_CONS_TTYA;

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
     * zs1: keyboard/mouse.  Left as plain unconnected serial channels
     * for now: the PROM finds no keyboard and selects the ttya
     * console.  (Switch to escc_kbd/escc_mouse along with the bwtwo.)
     */
    dev = qdev_new(TYPE_ESCC);
    qdev_prop_set_uint32(dev, "disabled", 0);
    qdev_prop_set_uint32(dev, "frequency", SUN3_ZS_FREQ);
    qdev_prop_set_uint32(dev, "it_shift", 1);
    qdev_prop_set_chr(dev, "chrA", NULL);
    qdev_prop_set_chr(dev, "chrB", NULL);
    qdev_prop_set_uint32(dev, "chnAtype", escc_serial);
    qdev_prop_set_uint32(dev, "chnBtype", escc_serial);
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

    sun3_build_idprom(m, &mac);
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
