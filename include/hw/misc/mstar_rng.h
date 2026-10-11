/*
 * MStar/SigmaStar hardware random number generator
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MSTAR_RNG_H
#define HW_MISC_MSTAR_RNG_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_RNG "mstar-rng"
OBJECT_DECLARE_SIMPLE_TYPE(MStarRngState, MSTAR_RNG)

#define MSTAR_RNG_REGION_SIZE 0x10

struct MStarRngState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint16_t ctrl;
};

#endif /* HW_MISC_MSTAR_RNG_H */
