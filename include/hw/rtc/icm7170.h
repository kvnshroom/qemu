/*
 * Intersil ICM7170 real-time clock.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef HW_RTC_ICM7170_H
#define HW_RTC_ICM7170_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_ICM7170 "icm7170"
OBJECT_DECLARE_SIMPLE_TYPE(ICM7170State, ICM7170)

/* eight counters, eight alarm-compare RAM bytes, interrupt, command */
#define ICM7170_NUM_TIME_REGS   8
#define ICM7170_REG_CSEC        0x00
#define ICM7170_REG_HOUR        0x01
#define ICM7170_REG_MIN         0x02
#define ICM7170_REG_SEC         0x03
#define ICM7170_REG_MONTH       0x04
#define ICM7170_REG_DAY         0x05
#define ICM7170_REG_YEAR        0x06
#define ICM7170_REG_DOW         0x07
#define ICM7170_REG_RAM_BASE    0x08
#define ICM7170_REG_INT         0x10
#define ICM7170_REG_CMD         0x11

/* interrupt status (read) / mask (write) bits */
#define ICM7170_INT_ALARM       0x01
#define ICM7170_INT_CSEC        0x02    /* 100Hz */
#define ICM7170_INT_DSEC        0x04    /* 10Hz */
#define ICM7170_INT_SEC         0x08
#define ICM7170_INT_MIN         0x10
#define ICM7170_INT_HOUR        0x20
#define ICM7170_INT_DAY         0x40
#define ICM7170_INT_PENDING     0x80    /* any enabled source fired */

/* command register bits */
#define ICM7170_CMD_FREQ_MASK   0x03    /* crystal frequency select */
#define ICM7170_CMD_24HR        0x04
#define ICM7170_CMD_RUN         0x08
#define ICM7170_CMD_INT_ENABLE  0x10
#define ICM7170_CMD_TEST        0x20

struct ICM7170State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* counter snapshot, valid at snap_ns (virtual clock) */
    uint8_t time_regs[ICM7170_NUM_TIME_REGS];
    int64_t snap_ns;
    uint8_t latch[ICM7170_NUM_TIME_REGS];
    uint8_t ram[ICM7170_NUM_TIME_REGS];

    uint8_t int_status;
    uint8_t int_mask;
    uint8_t cmd;

    QEMUTimer *periodic_timer;
    int64_t next_periodic_ns;
};

#endif
