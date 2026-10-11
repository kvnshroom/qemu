/*
 * MStar/SigmaStar hardware random number generator
 *
 * A small block next to the crypto engines (rng@224400 in the mainline
 * device tree). Registers as used by the mainline msc313-rng driver:
 *
 *   0x00 CTRL    bit 7 enables the generator
 *   0x08 VALUE   16 random bits
 *   0x0c STATUS  bit 0 set when VALUE holds fresh bits
 *
 * Without this block the driver busy-polls an unimplemented register for
 * a second per read, which slows the whole emulated boot down. Here a new
 * value is always ready, taken from the host's random source.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "hw/misc/mstar_rng.h"

#define RNG_CTRL            0x00
#define RNG_CTRL_ENABLE     (1 << 7)
#define RNG_VALUE           0x08
#define RNG_STATUS          0x0c
#define RNG_STATUS_READY    (1 << 0)

static uint64_t mstar_rng_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarRngState *s = MSTAR_RNG(opaque);
    uint16_t value;

    switch (addr) {
    case RNG_CTRL:
        return s->ctrl;
    case RNG_VALUE:
        qemu_guest_getrandom_nofail(&value, sizeof(value));
        return value;
    case RNG_STATUS:
        return (s->ctrl & RNG_CTRL_ENABLE) ? RNG_STATUS_READY : 0;
    default:
        qemu_log_mask(LOG_UNIMP, "mstar-rng: read 0x%02" HWADDR_PRIx "\n",
                      addr);
        return 0;
    }
}

static void mstar_rng_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarRngState *s = MSTAR_RNG(opaque);

    if (addr == RNG_CTRL) {
        s->ctrl = val;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "mstar-rng: write 0x%02" HWADDR_PRIx
                  " = 0x%04" PRIx64 "\n", addr, val);
}

static const MemoryRegionOps mstar_rng_ops = {
    .read = mstar_rng_read,
    .write = mstar_rng_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void mstar_rng_reset(DeviceState *dev)
{
    MSTAR_RNG(dev)->ctrl = 0;
}

static void mstar_rng_init(Object *obj)
{
    MStarRngState *s = MSTAR_RNG(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_rng_ops, s, TYPE_MSTAR_RNG,
                          MSTAR_RNG_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mstar_rng_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), mstar_rng_reset);
}

static const TypeInfo mstar_rng_types[] = {
    {
        .name           = TYPE_MSTAR_RNG,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarRngState),
        .instance_init  = mstar_rng_init,
        .class_init     = mstar_rng_class_init,
    },
};

DEFINE_TYPES(mstar_rng_types)
