/*
 * X-Powers AXP221/AXP223 PMIC (I2C interface)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_AXP22X_H
#define HW_MISC_AXP22X_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_AXP22X "axp22x"
OBJECT_DECLARE_SIMPLE_TYPE(AXP22xState, AXP22X)

#define AXP22X_NUM_REGS 256

struct AXP22xState {
    /*< private >*/
    I2CSlave parent_obj;
    /*< public >*/

    uint8_t regs[AXP22X_NUM_REGS];
    uint8_t ptr;            /* register pointer */
    bool ptr_set;           /* first byte of this write was the pointer */

    /* Board/battery state, settable with -global or qom-set at runtime */
    uint8_t battery_percent;
    uint16_t battery_mv;
    bool vbus;              /* USB power present */
};

#endif /* HW_MISC_AXP22X_H */
