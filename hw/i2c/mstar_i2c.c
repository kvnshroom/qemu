/*
 * MStar/SigmaStar HWI2C master
 *
 * The register file is 16-bit registers at the usual 4 byte RIU
 * stride, but the vendor HAL writes them a byte at a time and puts
 * separate commands in the two byte lanes of a word, so several
 * commands live at odd offsets and the region must accept 1 byte
 * accesses - with a minimum access size of 2 every START write is
 * silently dropped and the driver busy-polls forever.
 *
 * A transfer is: write CMD_START, write the address byte to WDATA
 * and check the NAK bit, then more WDATA bytes (or trigger reads via
 * RDATA_CFG and collect them from RDATA), and finally CMD_STOP. The
 * driver polls INT_CTL for completion after each step. With no slave
 * at the address the address byte NAKs, exactly like a real bus with
 * the device absent.
 *
 * The mainline msc313e-i2c driver does the same steps with 16-bit
 * writes (STOP as bit 8 of 0x04, read trigger as bit 8 of 0x0c) and
 * sleeps on the completion interrupt instead of polling. It reads the
 * event that completed the step from INT_STAT bits 13:8 and clears it
 * through INT_CTL. The interrupt is raised while a step is complete and
 * CTRL.enint is set.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/i2c/mstar_i2c.h"

/* Byte offsets; 16-bit registers with commands in single byte lanes */
#define I2C_CTRL            0x00    /* bit 0 reset, bit 2 interrupt enable */
#define I2C_CTRL_RST        (1 << 0)
#define I2C_CTRL_ENINT      (1 << 2)
#define I2C_CMD_START       0x04    /* byte: bit 0 generates START */
#define I2C_CMD_STOP        0x05    /* byte: bit 0 generates STOP */
#define I2C_CMD_BIT         (1 << 0)
#define I2C_STOP_HIBIT      (1 << 8) /* STOP via a wide write to 0x04 */
#define I2C_WDATA           0x08    /* W: byte to send; R: bit 8 = NAK */
#define I2C_WDATA_NAK       (1 << 8)
#define I2C_RDATA           0x0c    /* R: last received byte;
                                       W: bit 8 trigger read, bit 9 ACK */
#define I2C_RDATA_TRIG      (1 << 8)
#define I2C_RDATA_CFG       0x0d    /* byte: bit 0 trigger read, bit 1 ACK */
#define I2C_RDATA_CFG_TRIG  (1 << 0)
#define I2C_INT_CTL         0x10    /* R: bit 0 step complete; W: clear */
#define I2C_CUR_STATE       0x14    /* R: bits 4:0 state machine (0 =
                                       idle), bits 13:8 completed events */
#define I2C_EV_START_DET    (1 << 8)
#define I2C_EV_STOP_DET     (1 << 9)
#define I2C_EV_RX_DONE      (1 << 10)
#define I2C_EV_TX_DONE      (1 << 11)

static void mstar_i2c_update_irq(MStarI2cState *s)
{
    qemu_set_irq(s->irq, s->int_pending &&
                 (s->regs[I2C_CTRL / 4] & I2C_CTRL_ENINT));
}

/* A command step has finished with the given event */
static void mstar_i2c_step_done(MStarI2cState *s, uint16_t event)
{
    s->int_pending = true;
    s->int_stat |= event;
    mstar_i2c_update_irq(s);
}

static void mstar_i2c_recv(MStarI2cState *s)
{
    s->rdata = s->active ? i2c_recv(s->bus) : 0xff;
    mstar_i2c_step_done(s, I2C_EV_RX_DONE);
}

static void mstar_i2c_end(MStarI2cState *s)
{
    if (s->active) {
        i2c_end_transfer(s->bus);
        s->active = false;
    }
}

static void mstar_i2c_wdata(MStarI2cState *s, uint8_t byte)
{
    if (s->start_pending) {
        s->start_pending = false;
        s->nak = i2c_start_transfer(s->bus, byte >> 1, byte & 1) != 0;
        s->active = !s->nak;
    } else if (s->active) {
        s->nak = i2c_send(s->bus, byte) != 0;
    } else {
        s->nak = true;
    }
    mstar_i2c_step_done(s, I2C_EV_TX_DONE);
}

static uint64_t mstar_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    MStarI2cState *s = MSTAR_I2C(opaque);

    switch (addr) {
    case I2C_WDATA:
        return s->nak ? I2C_WDATA_NAK : 0;
    case I2C_RDATA:
        return s->rdata;
    case I2C_INT_CTL:
        return s->int_pending ? 1 : 0;
    case I2C_CUR_STATE:
        return s->int_stat;     /* state machine always idle */
    default:
        return s->regs[addr / 4];
    }
}

static void mstar_i2c_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    MStarI2cState *s = MSTAR_I2C(opaque);

    switch (addr) {
    case I2C_CTRL:
        s->regs[I2C_CTRL / 4] = val;
        if (val & I2C_CTRL_RST) {
            /* The mainline driver resets before every repeated start */
            mstar_i2c_end(s);
            s->start_pending = false;
            s->int_pending = false;
            s->int_stat = 0;
        }
        mstar_i2c_update_irq(s);
        return;
    case I2C_CMD_START:
        if (val & I2C_CMD_BIT) {
            /* A START while active is a repeated start */
            mstar_i2c_end(s);
            s->start_pending = true;
            mstar_i2c_step_done(s, I2C_EV_START_DET);
        }
        if ((size > 1) && (val & I2C_STOP_HIBIT)) {
            mstar_i2c_end(s);
            mstar_i2c_step_done(s, I2C_EV_STOP_DET);
        }
        return;
    case I2C_CMD_STOP:
        if (val & I2C_CMD_BIT) {
            mstar_i2c_end(s);
            mstar_i2c_step_done(s, I2C_EV_STOP_DET);
        }
        return;
    case I2C_WDATA:
        mstar_i2c_wdata(s, val);
        return;
    case I2C_RDATA:
        if ((size > 1) && (val & I2C_RDATA_TRIG)) {
            mstar_i2c_recv(s);
        }
        return;
    case I2C_RDATA_CFG:
        if (val & I2C_RDATA_CFG_TRIG) {
            mstar_i2c_recv(s);
        }
        return;
    case I2C_INT_CTL:
        if (val & 1) {
            s->int_pending = false;
            s->int_stat = 0;
            mstar_i2c_update_irq(s);
        }
        return;
    default:
        s->regs[addr / 4] = val;
        return;
    }
}

static const MemoryRegionOps mstar_i2c_ops = {
    .read = mstar_i2c_read,
    .write = mstar_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void mstar_i2c_reset(DeviceState *dev)
{
    MStarI2cState *s = MSTAR_I2C(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->start_pending = false;
    s->active = false;
    s->nak = false;
    s->int_pending = false;
    s->int_stat = 0;
    s->rdata = 0;
    qemu_set_irq(s->irq, 0);
}

static void mstar_i2c_init(Object *obj)
{
    MStarI2cState *s = MSTAR_I2C(obj);

    memory_region_init_io(&s->iomem, obj, &mstar_i2c_ops, s, TYPE_MSTAR_I2C,
                          MSTAR_I2C_REGION_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
}

static void mstar_i2c_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_i2c_reset);
}

static const TypeInfo mstar_i2c_types[] = {
    {
        .name           = TYPE_MSTAR_I2C,
        .parent         = TYPE_SYS_BUS_DEVICE,
        .instance_size  = sizeof(MStarI2cState),
        .instance_init  = mstar_i2c_init,
        .class_init     = mstar_i2c_class_init,
    },
};

DEFINE_TYPES(mstar_i2c_types)
