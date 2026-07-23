/*
 * Intersil ICM7170 real-time clock.
 *
 * Eight 8-bit counters (1/100s, hours, minutes, seconds, month, day,
 * year, day-of-week), eight bytes of alarm-compare RAM, an interrupt
 * mask/status register and a command register.  Reading the 1/100s
 * counter latches all eight counters so a torn multi-register read
 * cannot happen; the other counter reads return the latch.  The chip
 * can raise a periodic interrupt at 100Hz/10Hz/1Hz/min/hour/day rates.
 * The Sun-3 uses it as the 100Hz system clock at interrupt level 5/7.
 *
 * Alarm compare interrupts are not implemented (the RAM is plain
 * storage); none of the known guests use them.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rtc/icm7170.h"
#include "system/rtc.h"

#define ICM7170_REG_MASK        0x1f
#define ICM7170_CSEC_PERIOD_NS  (NANOSECONDS_PER_SECOND / 100)

/* rate of the fastest enabled periodic source, in nanoseconds */
static int64_t icm7170_periodic_interval(ICM7170State *s)
{
    static const struct {
        uint8_t bit;
        int64_t period;
    } sources[] = {
        { ICM7170_INT_CSEC, ICM7170_CSEC_PERIOD_NS },
        { ICM7170_INT_DSEC, NANOSECONDS_PER_SECOND / 10 },
        { ICM7170_INT_SEC,  NANOSECONDS_PER_SECOND },
        { ICM7170_INT_MIN,  60 * NANOSECONDS_PER_SECOND },
        { ICM7170_INT_HOUR, 60 * 60 * NANOSECONDS_PER_SECOND },
        { ICM7170_INT_DAY,  24 * 60 * 60 * NANOSECONDS_PER_SECOND },
    };
    int i;

    for (i = 0; i < ARRAY_SIZE(sources); i++) {
        if (s->int_mask & sources[i].bit) {
            return sources[i].period;
        }
    }
    return 0;
}

static void icm7170_update_irq(ICM7170State *s)
{
    bool level = (s->cmd & ICM7170_CMD_INT_ENABLE) &&
                 (s->int_status & s->int_mask);

    if (level) {
        s->int_status |= ICM7170_INT_PENDING;
    }
    qemu_set_irq(s->irq, level);
}

static void icm7170_rearm(ICM7170State *s)
{
    int64_t interval = icm7170_periodic_interval(s);

    if (!interval || !(s->cmd & ICM7170_CMD_RUN)) {
        timer_del(s->periodic_timer);
        return;
    }
    if (s->next_periodic_ns <= qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) {
        s->next_periodic_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + interval;
    }
    timer_mod(s->periodic_timer, s->next_periodic_ns);
}

static void icm7170_periodic(void *opaque)
{
    ICM7170State *s = opaque;
    int64_t interval = icm7170_periodic_interval(s);

    if (!interval) {
        return;
    }
    /* every faster source has also expired by now */
    s->int_status |= s->int_mask & ~ICM7170_INT_ALARM;
    s->next_periodic_ns += interval;
    icm7170_update_irq(s);
    icm7170_rearm(s);
}

/* materialise the counters at the current time into time_regs */
static void icm7170_snap(ICM7170State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed, csec;
    struct tm tm;
    time_t secs;

    if (!(s->cmd & ICM7170_CMD_RUN)) {
        s->snap_ns = now;
        return;
    }
    elapsed = now - s->snap_ns;
    csec = s->time_regs[ICM7170_REG_CSEC] +
           elapsed / (ICM7170_CSEC_PERIOD_NS);
    s->snap_ns = now - elapsed % ICM7170_CSEC_PERIOD_NS;
    if (csec < 100) {
        s->time_regs[ICM7170_REG_CSEC] = csec;
        return;
    }

    tm = (struct tm) {
        .tm_sec = s->time_regs[ICM7170_REG_SEC],
        .tm_min = s->time_regs[ICM7170_REG_MIN],
        .tm_hour = s->time_regs[ICM7170_REG_HOUR],
        .tm_mday = s->time_regs[ICM7170_REG_DAY],
        .tm_mon = s->time_regs[ICM7170_REG_MONTH] - 1,
        /* the year counter is two digits; keep a fixed century */
        .tm_year = s->time_regs[ICM7170_REG_YEAR] + 100,
    };
    secs = mktimegm(&tm) + csec / 100;
    gmtime_r(&secs, &tm);

    s->time_regs[ICM7170_REG_CSEC] = csec % 100;
    s->time_regs[ICM7170_REG_SEC] = tm.tm_sec;
    s->time_regs[ICM7170_REG_MIN] = tm.tm_min;
    s->time_regs[ICM7170_REG_HOUR] = tm.tm_hour;
    s->time_regs[ICM7170_REG_DAY] = tm.tm_mday;
    s->time_regs[ICM7170_REG_MONTH] = tm.tm_mon + 1;
    s->time_regs[ICM7170_REG_YEAR] = tm.tm_year % 100;
    s->time_regs[ICM7170_REG_DOW] = tm.tm_wday;
}

static uint64_t icm7170_read(void *opaque, hwaddr addr, unsigned size)
{
    ICM7170State *s = opaque;
    unsigned reg = addr & ICM7170_REG_MASK;
    uint64_t ret;

    switch (reg) {
    case ICM7170_REG_CSEC:
        icm7170_snap(s);
        memcpy(s->latch, s->time_regs, sizeof(s->latch));
        ret = s->latch[reg];
        break;
    case ICM7170_REG_HOUR ... ICM7170_REG_DOW:
        ret = s->latch[reg];
        break;
    case ICM7170_REG_RAM_BASE ...
         ICM7170_REG_RAM_BASE + ICM7170_NUM_TIME_REGS - 1:
        ret = s->ram[reg - ICM7170_REG_RAM_BASE];
        break;
    case ICM7170_REG_INT:
        ret = s->int_status;
        s->int_status = 0;
        icm7170_update_irq(s);
        break;
    case ICM7170_REG_CMD:
        ret = s->cmd;
        break;
    default:
        ret = 0;
        break;
    }
    return ret;
}

static void icm7170_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    ICM7170State *s = opaque;
    unsigned reg = addr & ICM7170_REG_MASK;

    qemu_log_mask(CPU_LOG_INT, "icm7170: [%02x] <- %02x\n", reg,
                  (uint8_t)val);

    switch (reg) {
    case ICM7170_REG_CSEC ... ICM7170_REG_DOW:
        icm7170_snap(s);
        s->time_regs[reg] = val;
        break;
    case ICM7170_REG_RAM_BASE ...
         ICM7170_REG_RAM_BASE + ICM7170_NUM_TIME_REGS - 1:
        s->ram[reg - ICM7170_REG_RAM_BASE] = val;
        break;
    case ICM7170_REG_INT:
        s->int_mask = val & ~ICM7170_INT_PENDING;
        s->next_periodic_ns = 0;
        icm7170_update_irq(s);
        icm7170_rearm(s);
        break;
    case ICM7170_REG_CMD:
        icm7170_snap(s);        /* freeze/thaw the counters at "now" */
        s->cmd = val;
        icm7170_update_irq(s);
        icm7170_rearm(s);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps icm7170_ops = {
    .read = icm7170_read,
    .write = icm7170_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
    .endianness = DEVICE_BIG_ENDIAN,
};

static void icm7170_reset_hold(Object *obj, ResetType type)
{
    ICM7170State *s = ICM7170(obj);
    struct tm tm;

    /* start out running with the host's time of day */
    qemu_get_timedate(&tm, 0);
    s->time_regs[ICM7170_REG_CSEC] = 0;
    s->time_regs[ICM7170_REG_SEC] = tm.tm_sec;
    s->time_regs[ICM7170_REG_MIN] = tm.tm_min;
    s->time_regs[ICM7170_REG_HOUR] = tm.tm_hour;
    s->time_regs[ICM7170_REG_DAY] = tm.tm_mday;
    s->time_regs[ICM7170_REG_MONTH] = tm.tm_mon + 1;
    s->time_regs[ICM7170_REG_YEAR] = tm.tm_year % 100;
    s->time_regs[ICM7170_REG_DOW] = tm.tm_wday;
    memcpy(s->latch, s->time_regs, sizeof(s->latch));
    s->snap_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->int_status = 0;
    s->int_mask = 0;
    s->cmd = ICM7170_CMD_24HR | ICM7170_CMD_RUN;
    s->next_periodic_ns = 0;
    timer_del(s->periodic_timer);
    qemu_set_irq(s->irq, 0);
}

static void icm7170_realize(DeviceState *dev, Error **errp)
{
    ICM7170State *s = ICM7170(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &icm7170_ops, s,
                          TYPE_ICM7170, ICM7170_REG_MASK + 1);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->periodic_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, icm7170_periodic, s);
}

static const VMStateDescription vmstate_icm7170 = {
    .name = TYPE_ICM7170,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(time_regs, ICM7170State, ICM7170_NUM_TIME_REGS),
        VMSTATE_INT64(snap_ns, ICM7170State),
        VMSTATE_UINT8_ARRAY(latch, ICM7170State, ICM7170_NUM_TIME_REGS),
        VMSTATE_UINT8_ARRAY(ram, ICM7170State, ICM7170_NUM_TIME_REGS),
        VMSTATE_UINT8(int_status, ICM7170State),
        VMSTATE_UINT8(int_mask, ICM7170State),
        VMSTATE_UINT8(cmd, ICM7170State),
        VMSTATE_TIMER_PTR(periodic_timer, ICM7170State),
        VMSTATE_INT64(next_periodic_ns, ICM7170State),
        VMSTATE_END_OF_LIST()
    }
};

static void icm7170_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = icm7170_realize;
    dc->vmsd = &vmstate_icm7170;
    rc->phases.hold = icm7170_reset_hold;
}

static const TypeInfo icm7170_info = {
    .name = TYPE_ICM7170,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ICM7170State),
    .class_init = icm7170_class_init,
};

static void icm7170_register_types(void)
{
    type_register_static(&icm7170_info);
}

type_init(icm7170_register_types)
