/*
 * MStar/SigmaStar HWI2C master
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_I2C_MSTAR_I2C_H
#define HW_I2C_MSTAR_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_MSTAR_I2C "mstar-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(MStarI2cState, MSTAR_I2C)

#define MSTAR_I2C_REGION_SIZE 0x200
#define MSTAR_I2C_NUM_REGS    (MSTAR_I2C_REGION_SIZE / 4)

struct MStarI2cState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    I2CBus *bus;
    qemu_irq irq;

    uint16_t regs[MSTAR_I2C_NUM_REGS];
    bool start_pending;     /* next WDATA byte is the address */
    bool active;            /* a transfer is open on the bus */
    bool nak;               /* last address/data byte was not acked */
    bool int_pending;
    uint16_t int_stat;      /* INT_STAT event bits 13:8 since last clear */
    uint8_t rdata;
};

#endif /* HW_I2C_MSTAR_I2C_H */
