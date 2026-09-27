/*
 * QEMU model of the Sun "bwtwo" 1152x900x1 monochrome frame buffer, in
 * the two forms the Sun-3 workstations have it.  The register/address
 * facts below come from the SunOS 4.1.1 GENERIC kernels' bwtwoprobe()/
 * p4probe()/bwtwommap()/bwtwoioctl()/bwtwointr() (unstripped vmunix,
 * symbols via sun3x-tools/sunos-symbols.py), from NetBSD 1.5.2's
 * bw2attach()/fb_pfour_id() and from the genuine 3/80 v3.0.3 and 3/60
 * v3.0.1 boot PROMs (see hw/m68k/sun3x-NOTES.md, hw/m68k/sun3-NOTES.md).
 *
 * THE P4-BUS VARIANT (property p4=true, the default; the Sun 3/80)
 * -----------------------------------------------------------------
 * A P4-bus frame buffer exposes one 32-bit register at its "P4 base"
 * (obmem 0x50300000 on the 3/80: the address autoconfig prints, "bwtwo0
 * at obmem 0x50300000 pri 4", and the literal address p4probe() peeks):
 *
 *   - Top byte: READ-ONLY.  High nibble = board ID (0x0 bwtwo, 0x4
 *     cgfour, 0x6 cgeight); low nibble = resolution code (0 1600x1280,
 *     1 1152x900, 2 1024x1024, 3 1280x1024, 4 1440x1440, 5 640x480,
 *     NetBSD sys/dev/sun/p4reg.h).  The on-board bwtwo is a fixed
 *     1152x900 board, so this byte always reads 0x01.
 *   - Low byte: read/write control bits -- 0x80 DIAG, 0x40 READBACK_CLR,
 *     0x20 VIDEO (display enable), 0x10 SYNC, 0x08 VTRACE (vertical
 *     retrace status), 0x04 INTCLR, 0x02 INTEN, 0x01 FIRSTHALF.
 *   - Middle two bytes: unused, read as 0.
 *
 * p4probe() (0xf80a9482) pokes `copy ^ 0x7f000000` back and re-reads:
 * if the top bits stuck (ordinary RAM) there is no card; if they are
 * read-only it decodes the ID byte, and for bwtwo/res 1 bwtwoprobe()
 * (0xf80a2f8a) sets softc.p4regs = reg, softc.addr = reg + 0x2000 and
 * width/height from the p4reg.h table at 0xf8109708 (1152x900, size
 * 144 * 900 = 129600 bytes); bwtwommap() maps softc.addr + offset;
 * bwtwoioctl() clears 0x24 and ORs 0x20 for video on/off; bwtwointr()
 * tests and re-writes bit 0x04 but nothing in open/attach/ioctl ever
 * sleeps on the interrupt, and it is only invoked by a level-4 CPU
 * interrupt this model does not raise -- VTRACE is a free-running
 * status bit at ~67 Hz with no interrupt delivery.
 *
 * The PROM's local-console geometry probe (0xfefe6bde..0xfefe6d1a)
 * peeks the same top byte (0 -> 1600x1280, nonzero -> 1152x900), then
 * does `bset #5,reg+3` (VIDEO), `bclr #2,reg+3` (INTCLR) and `bset
 * #3,reg+3` (VTRACE) -- byte read-modify-writes of the control byte --
 * and its scan helper (0xfefe0f3a, table 0xfeff5eb0) probes 32 pages of
 * 0x2000 from reg + 0x100000 ("VIDEOMEM_BASE", 0x50400000), a 256 KiB
 * span its font blitter later writes through.
 *
 * So two physical conventions for "where is the pixel memory" are both
 * real: the kernel's reg + 0x2000 and the PROM's reg + 0x100000 across
 * 256 KiB.  The model is one 128 KiB VRAM backing store aliased (true
 * memory-region aliases, phase 0) at both places, the PROM span being
 * two consecutive 128 KiB aliases, as an 18-bit address decode would
 * mirror.  Nothing else in OBMEM is mapped by this device: the PROM's
 * bwtwo dispatch path touches only the register and these windows.
 *
 * THE ON-BOARD SUN-3/60 VARIANT (property p4=false)
 * --------------------------------------------------
 * The Sun-3/60's bwtwo is not a P4 device: it is 128 KiB of plain video
 * RAM at obmem 0xFF000000 with no register at all.
 *
 *   - PROM bwtwo probe (0xfef2586): peeks VA 0x0FE2E000 (frame buffer
 *     + 0xE000, the PROM maps 32 pages = 256 KiB at VA 0x0FE20000 ->
 *     obmem 0xFF000000), then peeks the hi-res ID byte (obmem
 *     0xFF1C0000: bit 7 set = 1152x900, clear = 1600x1280, bus error =
 *     default 1152x900), clears 256 KiB with `clrw' from VA 0x0FE20000,
 *     then turns the video on by setting bit 0x08 (ENA_VIDEO) in the
 *     *system enable register* (control-space 0x40000000, 0xfef233a) --
 *     there is no P4 register to write.  The PROM's screen flash
 *     (0xfef2d58) clears and sets that same bit.
 *   - NetBSD bw2attach (0xe10d188): fb_pfour_id()'s write-back probe on
 *     the first VRAM word sees ordinary memory (-1 = no P4), so the
 *     frame buffer is at the probe address itself (no +0x100000), size
 *     0x20000; bw2svideo() -> enable_video() (0xe0f1360) ORs/ANDs 0x08
 *     into the enable register; no interrupt is registered.
 *   - SunOS bwtwoprobe (0xe09e89a): p4probe() < 0 -> softc.p4regs = 0,
 *     softc.addr = reg, geometry from the EEPROM screen-size byte 0x16
 *     (0 = 1152x900, size 0x1FA40) plus the same 0xFF1C0000 hi-res
 *     peek; FBIOSVIDEO/FBIOGVIDEO use enable-register bit 0x08
 *     (locore 0xe004c92 / shadow 0xe0e85d4); bwtwointr() only ever
 *     *clears* the interrupt register's VIDEO_ENAB bit 0x10.
 *
 * So in this mode the device is just the VRAM (mmio 0) plus a mirror
 * (mmio 1, the second 128 KiB of the 256 KiB window the PROM clears),
 * and the display-enable is an input line, "video-enable", that the
 * board drives from its enable register.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_SUN_BWTWO "sun-bwtwo"
OBJECT_DECLARE_SIMPLE_TYPE(SunBwTwoState, SUN_BWTWO)

#define BWTWO_WIDTH     1152
#define BWTWO_HEIGHT    900
#define BWTWO_STRIDE    (BWTWO_WIDTH / 8)               /* 144 bytes/row */
#define BWTWO_VRAM_SIZE (BWTWO_STRIDE * BWTWO_HEIGHT)   /* 129600 bytes  */

/*
 * Real bwtwo/P4 boards carry a full 128 KiB (0x20000) VRAM chip even
 * though the visible 1152x900x1 image only occupies the first 129600
 * (0x1fa40) bytes of it -- see the file header.  Only the first
 * BWTWO_VRAM_SIZE bytes are ever read by the display refresh below; the
 * remainder is inert backing store real hardware also wastes.
 */
#define BWTWO_VRAM_ALLOC_SIZE   0x20000                 /* 128 KiB, real HW */

/* P4 register top-byte encoding: ID nibble 0x0 (bwtwo) | resolution
 * nibble 0x1 (1152x900) -- see file header. */
#define BWTWO_P4_IDRES          0x01

/* P4 register control-byte (low byte) bits -- see file header. */
#define BWTWO_CTRL_DIAG         0x80
#define BWTWO_CTRL_READBACK_CLR 0x40
#define BWTWO_CTRL_VIDEO        0x20
#define BWTWO_CTRL_SYNC         0x10
#define BWTWO_CTRL_VTRACE       0x08
#define BWTWO_CTRL_INTCLR       0x04
#define BWTWO_CTRL_INTEN        0x02
#define BWTWO_CTRL_FIRSTHALF    0x01

/* Free-running VTRACE toggle rate -- "fine as a periodic flip", not a
 * precise duty cycle (see file header, "Interrupt line"). */
#define BWTWO_VTRACE_HZ         67

struct SunBwTwoState {
    SysBusDevice parent_obj;

    QemuConsole *con;
    MemoryRegion reg_mem;           /* the P4 register itself, 4 bytes   */
    MemoryRegion vram_mem;          /* real 128 KiB VRAM backing store   */
    MemoryRegion vram_kernel_alias; /* P4ID+0x2000, phase 0 (bwtwoprobe) */
    MemoryRegion vram_prom_alias0;  /* P4ID+0x100000, phase 0 (PROM)     */
    MemoryRegion vram_prom_alias1;  /* P4ID+0x120000, phase 0 (mirror)  */
    MemoryRegion vram_mirror;       /* on-board variant: 2nd 128 KiB     */
    uint8_t ctrl;
    bool full_update;
    bool p4;                        /* P4-bus device (sun3x) or on-board */
};

static bool bwtwo_update_display(void *opaque)
{
    SunBwTwoState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    DirtyBitmapSnapshot *snap = NULL;
    const uint8_t *pix;
    uint32_t *data;
    int x, y, y_start;

    if (surface_bits_per_pixel(surface) != 32) {
        return true;
    }

    pix = memory_region_get_ram_ptr(&s->vram_mem);
    data = (uint32_t *)surface_data(surface);
    y_start = -1;

    if (!s->full_update) {
        snap = memory_region_snapshot_and_clear_dirty(
            &s->vram_mem, 0, BWTWO_VRAM_SIZE, DIRTY_MEMORY_VGA);
    }

    for (y = 0; y < BWTWO_HEIGHT; y++) {
        bool update = s->full_update;
        const uint8_t *row = pix + (ptrdiff_t)y * BWTWO_STRIDE;
        uint32_t *rowout = data + (ptrdiff_t)y * BWTWO_WIDTH;

        if (!update) {
            update = memory_region_snapshot_get_dirty(&s->vram_mem, snap,
                                                       (ram_addr_t)y * BWTWO_STRIDE,
                                                       BWTWO_STRIDE);
        }

        if (!update) {
            if (y_start >= 0) {
                qemu_console_update(s->con, 0, y_start, BWTWO_WIDTH,
                                    y - y_start);
                y_start = -1;
            }
            continue;
        }

        if (y_start < 0) {
            y_start = y;
        }

        if (!(s->ctrl & BWTWO_CTRL_VIDEO)) {
            /* Video disabled: blank (white) screen, like real hardware
             * suppresses the CRT beam. */
            for (x = 0; x < BWTWO_WIDTH; x++) {
                rowout[x] = 0x00ffffff;
            }
            continue;
        }

        for (x = 0; x < BWTWO_WIDTH; x += 8) {
            uint8_t byte = row[x / 8];
            int b;

            for (b = 0; b < 8; b++) {
                /* MSB = leftmost pixel; 1 = black. */
                rowout[x + b] = (byte & (0x80 >> b)) ? 0x00000000 : 0x00ffffff;
            }
        }
    }

    s->full_update = false;
    if (y_start >= 0) {
        qemu_console_update(s->con, 0, y_start, BWTWO_WIDTH,
                            BWTWO_HEIGHT - y_start);
    }

    g_free(snap);
    return true;
}

static void bwtwo_invalidate_display(void *opaque)
{
    SunBwTwoState *s = opaque;

    memory_region_set_dirty(&s->vram_mem, 0, BWTWO_VRAM_SIZE);
    s->full_update = true;
}

/* Live VTRACE status bit: a free-running toggle, no stored state, no
 * interrupt delivery -- see file header "Interrupt line". */
static bool bwtwo_vtrace_bit(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t half_period_ns = NANOSECONDS_PER_SECOND / (2 * BWTWO_VTRACE_HZ);

    return (now / half_period_ns) & 1;
}

static uint32_t bwtwo_reg_full(SunBwTwoState *s)
{
    uint8_t ctrl = s->ctrl & ~BWTWO_CTRL_VTRACE;

    if (bwtwo_vtrace_bit()) {
        ctrl |= BWTWO_CTRL_VTRACE;
    }
    return ((uint32_t)BWTWO_P4_IDRES << 24) | ctrl;
}

/*
 * The P4 register: top byte (ID | resolution) is fixed/read-only; the low
 * (control) byte is the only writable part.  See the file header for the
 * byte-exact kernel/PROM disassembly this implements.
 */
static uint64_t bwtwo_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    SunBwTwoState *s = opaque;
    uint32_t full = bwtwo_reg_full(s);
    unsigned shift = (4 - size - addr) * 8;
    uint64_t mask = (size == 4) ? 0xffffffffULL : ((1ULL << (size * 8)) - 1);

    return (full >> shift) & mask;
}

static void bwtwo_reg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    SunBwTwoState *s = opaque;
    hwaddr ctrl_pos = 3; /* byte offset of the control byte in the 4-byte
                          * big-endian register */

    if (addr <= ctrl_pos && ctrl_pos < addr + size) {
        unsigned byte_shift = (size - 1 - (ctrl_pos - addr)) * 8;

        s->ctrl = (val >> byte_shift) & 0xff;
        s->full_update = true;
    }
    /* Writes to the ID/resolution byte(s) are silently discarded -- a
     * real read-only ID register ignores them too (that's exactly what
     * p4probe()'s write-back probe relies on). */
}

static const MemoryRegionOps bwtwo_reg_ops = {
    .read = bwtwo_reg_read,
    .write = bwtwo_reg_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static const GraphicHwOps bwtwo_ops = {
    .invalidate = bwtwo_invalidate_display,
    .gfx_update = bwtwo_update_display,
};

/*
 * "video-enable" input: the on-board 3/60 variant has no register, its
 * display enable is bit 0x08 of the machine's system enable register
 * (see the file header).
 */
static void bwtwo_video_enable(void *opaque, int n, int level)
{
    SunBwTwoState *s = opaque;
    uint8_t ctrl = level ? (s->ctrl | BWTWO_CTRL_VIDEO)
                         : (s->ctrl & ~BWTWO_CTRL_VIDEO);

    if (ctrl != s->ctrl) {
        s->ctrl = ctrl;
        s->full_update = true;
    }
}

static void sun_bwtwo_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    Object *obj = OBJECT(dev);
    SunBwTwoState *s = SUN_BWTWO(dev);

    if (!s->p4) {
        /* on-board Sun-3/60: 128 KiB of plain VRAM, mirrored once */
        memory_region_init_ram(&s->vram_mem, obj, "sun-bwtwo.vram",
                               BWTWO_VRAM_ALLOC_SIZE, &error_fatal);
        memory_region_set_log(&s->vram_mem, true, DIRTY_MEMORY_VGA);
        sysbus_init_mmio(sbd, &s->vram_mem);
        memory_region_init_alias(&s->vram_mirror, obj,
                                 "sun-bwtwo.vram-mirror", &s->vram_mem, 0,
                                 BWTWO_VRAM_ALLOC_SIZE);
        sysbus_init_mmio(sbd, &s->vram_mirror);
        qdev_init_gpio_in_named(dev, bwtwo_video_enable, "video-enable", 1);

        s->con = qemu_graphic_console_create(dev, 0, &bwtwo_ops, s);
        qemu_console_resize(s->con, BWTWO_WIDTH, BWTWO_HEIGHT);
        return;
    }

    memory_region_init_io(&s->reg_mem, obj, &bwtwo_reg_ops, s,
                          "sun-bwtwo.p4reg", 4);
    sysbus_init_mmio(sbd, &s->reg_mem);

    memory_region_init_ram(&s->vram_mem, obj, "sun-bwtwo.vram",
                           BWTWO_VRAM_ALLOC_SIZE, &error_fatal);
    memory_region_set_log(&s->vram_mem, true, DIRTY_MEMORY_VGA);

    /* Kernel convention: P4ID + 0x2000, phase 0 (bwtwoprobe()/bwtwommap()
     * -- see file header). */
    memory_region_init_alias(&s->vram_kernel_alias, obj,
                             "sun-bwtwo.vram-kernel", &s->vram_mem, 0,
                             BWTWO_VRAM_ALLOC_SIZE);
    sysbus_init_mmio(sbd, &s->vram_kernel_alias);

    /* PROM convention: P4ID + 0x100000, mirrored twice to cover the full
     * 256 KiB span the PROM's VIDEOMEM_BASE scan touches -- see file
     * header. */
    memory_region_init_alias(&s->vram_prom_alias0, obj,
                             "sun-bwtwo.vram-prom0", &s->vram_mem, 0,
                             BWTWO_VRAM_ALLOC_SIZE);
    sysbus_init_mmio(sbd, &s->vram_prom_alias0);

    memory_region_init_alias(&s->vram_prom_alias1, obj,
                             "sun-bwtwo.vram-prom1", &s->vram_mem, 0,
                             BWTWO_VRAM_ALLOC_SIZE);
    sysbus_init_mmio(sbd, &s->vram_prom_alias1);

    s->con = qemu_graphic_console_create(dev, 0, &bwtwo_ops, s);
    qemu_console_resize(s->con, BWTWO_WIDTH, BWTWO_HEIGHT);
}

static void sun_bwtwo_reset(DeviceState *d)
{
    SunBwTwoState *s = SUN_BWTWO(d);

    /* VIDEO enabled by default, like real hardware: the PROM's own P4/
     * self-test banner should be visible even before SunOS or the PROM's
     * own console-init helper (0xfefe6ab2, see file header) ever touches
     * this register.  The on-board 3/60 variant instead follows the
     * machine's enable register, which resets to 0 (video off until the
     * PROM or the kernel turns it on). */
    s->ctrl = s->p4 ? BWTWO_CTRL_VIDEO : 0;
    s->full_update = true;
}

static int sun_bwtwo_post_load(void *opaque, int version_id)
{
    bwtwo_invalidate_display(opaque);
    return 0;
}

static const VMStateDescription vmstate_sun_bwtwo = {
    .name = "sun-bwtwo",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sun_bwtwo_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(ctrl, SunBwTwoState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sun_bwtwo_properties[] = {
    /* true: P4-bus device (Sun 3/80); false: on-board Sun-3/60 VRAM */
    DEFINE_PROP_BOOL("p4", SunBwTwoState, p4, true),
};

static void sun_bwtwo_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = sun_bwtwo_realize;
    device_class_set_legacy_reset(dc, sun_bwtwo_reset);
    dc->vmsd = &vmstate_sun_bwtwo;
    device_class_set_props(dc, sun_bwtwo_properties);
}

static const TypeInfo sun_bwtwo_info = {
    .name          = TYPE_SUN_BWTWO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SunBwTwoState),
    .class_init    = sun_bwtwo_class_init,
};

static void sun_bwtwo_register_types(void)
{
    type_register_static(&sun_bwtwo_info);
}

type_init(sun_bwtwo_register_types)
