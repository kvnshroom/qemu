/*
 * MStar/SigmaStar generic readback register bank
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MSTAR_REGBANK_H
#define HW_MISC_MSTAR_REGBANK_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MSTAR_REGBANK "mstar-regbank"
OBJECT_DECLARE_SIMPLE_TYPE(MStarRegbankState, MSTAR_REGBANK)

#define MSTAR_REGBANK_SIZE     0x200
#define MSTAR_REGBANK_NUM_REGS (MSTAR_REGBANK_SIZE / 4)

/*
 * A sparse reset-default: the value a register in the bank powers up
 * holding. Captured from real silicon; see contrib/mstarpoker.
 */
typedef struct MStarRegDefault {
    uint16_t offset;    /* byte offset within the bank */
    uint16_t value;
} MStarRegDefault;

/*
 * A name for a register whose meaning we have worked out. Banks whose
 * registers are mostly still unknown (clkgen, ...) can supply the few they
 * understand here; the rest are traced by offset. Terminate with a NULL name.
 */
typedef struct MStarRegName {
    uint16_t offset;
    const char *name;
} MStarRegName;

struct MStarRegbankState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    uint16_t regs[MSTAR_REGBANK_NUM_REGS];

    /* Optional reset defaults, set by the instantiator before realize. */
    const MStarRegDefault *defaults;
    unsigned num_defaults;

    /*
     * Optional status bits that always read back as set (OR-ed into the
     * stored value), for hardware "done" flags the vendor code polls, e.g.
     * the USB UTMI PHY calibration-done bit. Set before realize.
     */
    const MStarRegDefault *read_set;
    unsigned num_read_set;

    /*
     * Tracing support, set by the instantiator before realize. `base` is the
     * bank's absolute RIU address and `bankname` a short label, both used in
     * the trace output; `regnames` optionally names the registers we have
     * decoded (see the mstar_regbank_read/write trace events). Enable with
     * e.g. -trace enable=mstar_regbank_write.
     */
    uint32_t base;
    const char *bankname;
    const MStarRegName *regnames;
};

#endif /* HW_MISC_MSTAR_REGBANK_H */
