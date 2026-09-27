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
#include "hw/intc/m68k_irqc.h"
#include "chardev/char.h"
#include "qemu/timer.h"
#include "target/m68k/cpu.h"
#include "elf.h"
#include "standard-headers/asm-m68k/bootinfo.h"
#include "bootinfo.h"

/* --- physical memory map (see sun3x-NOTES.md) --- */
#define SUN3X_RAM_BASE          0x00000000
#define SUN3X_RAM_DEFAULT       (16 * MiB)
#define SUN3X_RAM_MAX           (64 * MiB)

#define SUN3X_IOMMU             0x60000000
#define SUN3X_IOMMU_SIZE        0x1000
#define SUN3X_ENAREG            0x61000000  /* enable/copro block base */
#define SUN3X_ENA_SIZE          0x2000      /* covers INTREG @+0x1400, DIAG @+0x1800 */
#define SUN3X_INTREG            0x61001400  /* offset 0x1400 within the block */
#define SUN3X_ZS1               0x62000000  /* console Z8530 */
#define SUN3X_ZS2               0x62002000  /* keyboard/mouse Z8530 (idle) */
#define SUN3X_ZS_SIZE           0x2000
#define SUN3X_EEPROM            0x64000000  /* NVRAM: idprom @+0x7d8, clock @+0xf8 */
#define SUN3X_EEPROM_SIZE       0x800
#define SUN3X_IDPROM_OFF        0x7d8
#define SUN3X_CLOCK_OFF         0xf8

/* PROM window: identity-mapped, holds romvec + page table + stubs */
#define SUN3X_PROM_PHYS         0xfee00000
#define SUN3X_PROM_SIZE         0x200000
#define SUN3X_PROM_BASE         0xfefe0000  /* romvec (SUN3X_PROM_BASE) */

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
#define SUN3X_INT_ENABLE_5      0x20

/* clock tick rate (Linux/m68k CONFIG_HZ default) */
#define SUN3X_HZ                100

/* Z8530 (ESCC) channel-A register byte offsets for it_shift==1:
 *   channel = (addr >> 2) & 1 (A == 1), reg = (addr >> 1) & 1 (ctrl 0/data 1) */
#define SUN3X_ZS_FREQ           4915200

typedef struct Sun3xState {
    M68kCPU *cpu;
    DeviceState *irqc;

    MemoryRegion bg;            /* absorb/zero background */
    MemoryRegion iommu;         /* DVMA page table scratch */
    MemoryRegion enareg;        /* enable/copro + LED block (absorbed) */
    MemoryRegion intreg_mr;     /* the interrupt/enable register */
    MemoryRegion eeprom;        /* NVRAM (idprom + clock) */
    MemoryRegion prom;          /* synthesised boot PROM window */

    QEMUTimer *tick;
    uint8_t intreg;
    bool tick_latch;

    uint32_t reset_pc;
    uint32_t reset_sp;
} Sun3xState;

/* --- interrupt / timer --- */

static void sun3x_update_irq(Sun3xState *s)
{
    bool level5 = s->tick_latch &&
                  (s->intreg & SUN3X_INT_ENABLE_ALL) &&
                  (s->intreg & SUN3X_INT_ENABLE_5);

    /* level 5 autovector -> IRQC gpio index 4 (LEVEL_1..7 are 0-based) */
    qemu_set_irq(qdev_get_gpio_in(s->irqc, 5 - 1), level5);
}

static void sun3x_tick(void *opaque)
{
    Sun3xState *s = opaque;

    s->tick_latch = true;
    sun3x_update_irq(s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / SUN3X_HZ);
}

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
        s->tick_latch = false;
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

/* --- IDPROM + Mostek clock (in the NVRAM) --- */

static void sun3x_build_nvram(Sun3xState *s)
{
    uint8_t *p = memory_region_get_ram_ptr(&s->eeprom);
    uint8_t *id = p + SUN3X_IDPROM_OFF;
    uint8_t *clk = p + SUN3X_CLOCK_OFF;
    struct tm tm;
    uint8_t sum = 0;
    int i;

    memset(p, 0, SUN3X_EEPROM_SIZE);

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

    /* Mostek-style clock: csr, sec, min, hour, wday, mday, month, year (BCD) */
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

    s->cpu = M68K_CPU(cpu_create(machine->cpu_type));

    /* autovector interrupt controller */
    s->irqc = qdev_new(TYPE_M68K_IRQC);
    object_property_set_link(OBJECT(s->irqc), "m68k-cpu", OBJECT(s->cpu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->irqc), &error_fatal);

    /* zero/absorb background over the whole space (sun3x does no bus-error
     * probing: RAM comes from BI_MEMCHUNK and devices are at fixed addrs). */
    memory_region_init_io(&s->bg, NULL, &sun3x_absorb_ops, s,
                          "sun3x.bg", UINT64_MAX);
    memory_region_add_subregion_overlap(sysmem, 0, &s->bg, -1);

    /* main RAM */
    memory_region_add_subregion(sysmem, SUN3X_RAM_BASE, machine->ram);

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

    /* console Z8530 (channel A driven by the PROM stubs) */
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
    sysbus_mmio_map(sbd, 0, SUN3X_ZS1);

    /* second Z8530 (keyboard/mouse), left idle */
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
    sysbus_mmio_map(sbd, 0, SUN3X_ZS2);

    /* synthesised boot PROM window (identity mapped) */
    memory_region_init_ram(&s->prom, NULL, "sun3x.prom",
                           SUN3X_PROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, SUN3X_PROM_PHYS, &s->prom);
    sun3x_build_prom(s);

    /* periodic clock tick -> autovector level 5 */
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, sun3x_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / SUN3X_HZ);

    if (machine->kernel_filename) {
        sun3x_load_kernel(s, machine);
    } else {
        error_report("sun3x currently only supports -kernel boot");
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
