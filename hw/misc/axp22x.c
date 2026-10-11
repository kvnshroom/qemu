/*
 * X-Powers AXP221/AXP223 PMIC (I2C interface)
 *
 * The Miyoo Mini Plus carries an AXP223 on SSD202D i2c bus 1 at 0x34.
 * The vendor firmware does not bind a kernel driver to it; the userspace
 * tool /customer/app/axp_test talks to it through /dev/i2c-1 to power the
 * board up, switch the Wi-Fi rail and read the battery state. It touches
 * registers 0x00, 0x01, 0x03, 0x0f, 0x10, 0x12, 0x21, 0x30-0x33, 0x4a,
 * 0x78/0x79, 0x82, 0xb8 and 0xb9.
 *
 * This is a register file with the status registers derived from the
 * board state (battery level and voltage, USB power present). Register
 * meanings follow the AXP221/AXP223 datasheets. The IC type, the status
 * register layout and the battery ADC scaling were checked against a real
 * Mini Plus; the control register defaults below are not from hardware.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/axp22x.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define AXP22X_PWR_INPUT_STATUS 0x00    /* bit7 ACIN present, bit6 usable,
                                           bit5 VBUS present, bit4 usable,
                                           bit2 battery current direction */
#define AXP22X_PWR_OP_MODE      0x01    /* bit6 charging, bit5 battery
                                           present */
#define AXP22X_IC_TYPE          0x03
#define AXP22X_IRQ_STATUS_FIRST 0x48    /* 0x48-0x4c, write 1 to clear */
#define AXP22X_IRQ_STATUS_LAST  0x4c
#define AXP22X_BATT_V_H         0x78    /* 12-bit battery voltage, 1.1 mV */
#define AXP22X_BATT_V_L         0x79
#define AXP22X_BATT_CHRG_I_H    0x7a    /* 12-bit charge current, 0.5 mA */
#define AXP22X_BATT_CHRG_I_L    0x7b
#define AXP22X_FG_RES           0xb9    /* bit7 valid, bits6:0 percent */

/* Read back as 0x06 on a Mini Plus */
#define AXP22X_IC_TYPE_VALUE    0x06
#define AXP22X_CHARGE_MA        500

static bool axp22x_charging(AXP22xState *s)
{
    return s->vbus && s->battery_percent < 100;
}

static uint8_t axp22x_read_reg(AXP22xState *s, uint8_t reg)
{
    unsigned adc;

    switch (reg) {
    case AXP22X_PWR_INPUT_STATUS:
        /*
         * The Mini Plus wires USB-C power to ACIN: a real unit on a charger
         * with a full battery reads 0xc4 here and 0x30 in PWR_OP_MODE.
         */
        return (s->vbus ? 0xc4 : 0) | (axp22x_charging(s) ? 0x04 : 0);
    case AXP22X_PWR_OP_MODE:
        return 0x20 | (axp22x_charging(s) ? 0x40 : 0);
    case AXP22X_IC_TYPE:
        return AXP22X_IC_TYPE_VALUE;
    case AXP22X_BATT_V_H:
    case AXP22X_BATT_V_L:
        adc = s->battery_mv * 10 / 11;
        return reg == AXP22X_BATT_V_H ? adc >> 4 : adc & 0xf;
    case AXP22X_BATT_CHRG_I_H:
    case AXP22X_BATT_CHRG_I_L:
        adc = axp22x_charging(s) ? AXP22X_CHARGE_MA * 2 : 0;
        return reg == AXP22X_BATT_CHRG_I_H ? adc >> 4 : adc & 0xf;
    case AXP22X_FG_RES:
        return 0x80 | MIN(s->battery_percent, 100);
    default:
        return s->regs[reg];
    }
}

static void axp22x_write_reg(AXP22xState *s, uint8_t reg, uint8_t val)
{
    if (reg >= AXP22X_IRQ_STATUS_FIRST && reg <= AXP22X_IRQ_STATUS_LAST) {
        s->regs[reg] &= ~val;
        return;
    }
    s->regs[reg] = val;
}

static int axp22x_event(I2CSlave *i2c, enum i2c_event event)
{
    AXP22xState *s = AXP22X(i2c);

    if (event == I2C_START_SEND) {
        s->ptr_set = false;
    }
    return 0;
}

static uint8_t axp22x_recv(I2CSlave *i2c)
{
    AXP22xState *s = AXP22X(i2c);

    return axp22x_read_reg(s, s->ptr++);
}

static int axp22x_send(I2CSlave *i2c, uint8_t data)
{
    AXP22xState *s = AXP22X(i2c);

    if (!s->ptr_set) {
        s->ptr = data;
        s->ptr_set = true;
    } else {
        axp22x_write_reg(s, s->ptr++, data);
    }
    return 0;
}

static void axp22x_reset(DeviceState *dev)
{
    AXP22xState *s = AXP22X(dev);

    /*
     * Control register reset values as read from an AXP221s (taken from
     * the axp221_pmu model in hw/misc/axp2xx.c); the status registers are
     * computed from the board state in axp22x_read_reg().
     */
    static const uint8_t defaults[][2] = {
        { 0x10, 0xbf }, { 0x13, 0x01 }, { 0x30, 0x60 }, { 0x31, 0x03 },
        { 0x32, 0x43 }, { 0x33, 0xc6 }, { 0x34, 0x45 }, { 0x35, 0x0e },
        { 0x36, 0x5d }, { 0x37, 0x08 }, { 0x38, 0xa5 }, { 0x39, 0x1f },
        { 0x3c, 0xfc }, { 0x3d, 0x16 }, { 0x40, 0xd8 }, { 0x41, 0xff },
        { 0x42, 0x03 }, { 0x43, 0x03 }, { 0x80, 0x80 }, { 0x82, 0xe0 },
        { 0x84, 0x32 }, { 0x8f, 0x01 }, { 0x90, 0x07 }, { 0x91, 0x1f },
        { 0x92, 0x07 }, { 0x93, 0x1f }, { 0xb8, 0xc0 }, { 0xe6, 0xa0 },
    };
    unsigned i;

    memset(s->regs, 0, sizeof(s->regs));
    for (i = 0; i < ARRAY_SIZE(defaults); i++) {
        s->regs[defaults[i][0]] = defaults[i][1];
    }
    s->ptr = 0;
    s->ptr_set = false;
}

static const Property axp22x_properties[] = {
    DEFINE_PROP_UINT8("battery-percent", AXP22xState, battery_percent, 80),
    DEFINE_PROP_UINT16("battery-mv", AXP22xState, battery_mv, 3900),
    DEFINE_PROP_BOOL("vbus", AXP22xState, vbus, true),
};

static const VMStateDescription vmstate_axp22x = {
    .name = TYPE_AXP22X,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, AXP22xState),
        VMSTATE_UINT8_ARRAY(regs, AXP22xState, AXP22X_NUM_REGS),
        VMSTATE_UINT8(ptr, AXP22xState),
        VMSTATE_BOOL(ptr_set, AXP22xState),
        VMSTATE_END_OF_LIST()
    }
};

static void axp22x_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, axp22x_reset);
    device_class_set_props(dc, axp22x_properties);
    dc->vmsd = &vmstate_axp22x;
    k->event = axp22x_event;
    k->recv = axp22x_recv;
    k->send = axp22x_send;
}

static const TypeInfo axp22x_types[] = {
    {
        .name          = TYPE_AXP22X,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(AXP22xState),
        .class_init    = axp22x_class_init,
    },
};

DEFINE_TYPES(axp22x_types)
