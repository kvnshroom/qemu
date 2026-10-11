/*
 * Miyoo Mini handheld game console
 *
 * A handheld game console built around the SigmaStar SSD202D.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/block-backend-global-state.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/ssd202d.h"
#include "hw/display/dsi.h"
#include "hw/i2c/alpu.h"
#include "hw/misc/axp22x.h"
#include "hw/core/irq.h"
#include "hw/sd/sd.h"
#include "ui/input.h"
#include "standard-headers/linux/input-event-codes.h"

#define TYPE_MIYOOMINI_MACHINE MACHINE_TYPE_NAME("miyoomini")
#define TYPE_MIYOOMINIPLUS_MACHINE MACHINE_TYPE_NAME("miyoomini-plus")
OBJECT_DECLARE_SIMPLE_TYPE(MiyooMiniMachineState, MIYOOMINI_MACHINE)

/*
 * The battery voltage divider feeds SAR ADC channel 0. Give it a
 * mid-scale reading so MainUI's battery gauge shows a roughly half-full
 * level rather than the empty reading a bare mid-rail produces. This is
 * provisional: the ADC-to-voltage curve the vendor firmware applies is
 * not calibrated yet.
 */
#define MIYOOMINI_BATTERY_SAR_CH0   460

/*
 * HACK (temporary): keep the UI up. The vendor keymon daemon powers the
 * board off (via reboot) after an idle timeout when it is running on
 * battery, and MainUI shows the charging screen instead of the menu
 * unless it thinks it was powered on with the power button. Until the
 * charger/OTG and power-on-reason are modelled properly we fake:
 *   - the charger-detect (gpio59, PM pad 0x50) reads plugged in, so
 *     keymon stays awake instead of idling off; and
 *   - the power button (gpio86, main pad 0x104) is held at power-on and
 *     released once the kernel has latched its pwrkeyval boot reason, so
 *     MainUI comes up on the menu rather than the charging screen.
 * This needs revisiting: model the real charger and power-on reason.
 */
#define MIYOOMINI_CHARGER_PM_PAD    0x050
#define MIYOOMINI_PWRKEY_MAIN_PAD   0x104
#define MIYOOMINI_PWRKEY_RELEASE_NS (8 * NANOSECONDS_PER_SECOND)

/*
 * The board buttons, wired to SoC GPIO pads (from the mainline 6.5
 * device tree's gpio-keys node and the vendor kernel's GPIO HAL pad
 * table, which the polling gpio-keys driver reads at these offsets).
 * Each is driven by one key on the QEMU keyboard. Every button idles
 * high behind its pull-up and is pulled low when pressed. The dpad's
 * down/left are on the PM pad bank, the rest on the main bank.
 */
typedef struct {
    unsigned int key;       /* host key (Linux input-event code) */
    bool pm_bank;           /* pad is in the PM bank, not the main bank */
    uint16_t offset;        /* pad register offset within its bank */
    const char *name;
} MiyooMiniButton;

static const MiyooMiniButton miyoomini_buttons[] = {
    { KEY_UP,         false, 0x004, "up" },
    { KEY_DOWN,       true,  0x128, "down" },
    { KEY_LEFT,       true,  0x12c, "left" },
    { KEY_RIGHT,      false, 0x014, "right" },
    { KEY_A,          false, 0x01c, "a" },
    { KEY_B,          false, 0x018, "b" },
    { KEY_X,          false, 0x024, "x" },
    { KEY_Y,          false, 0x020, "y" },
    { KEY_ENTER,      false, 0x028, "start" },
    { KEY_RIGHTSHIFT, false, 0x02c, "select" },
    { KEY_ESC,        false, 0x030, "menu" },
    { KEY_Q,          false, 0x038, "l1" },
    { KEY_W,          false, 0x034, "l2" },
    { KEY_O,          false, 0x060, "r1" },
    { KEY_P,          false, 0x114, "r2" },
};

#define TYPE_MIYOOMINI_KEYS "miyoomini-keys"
OBJECT_DECLARE_SIMPLE_TYPE(MiyooMiniKeysState, MIYOOMINI_KEYS)

struct MiyooMiniKeysState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    QemuInputHandlerState *hs;
    qemu_irq pads[ARRAY_SIZE(miyoomini_buttons)];
};

struct MiyooMiniMachineState {
    /*< private >*/
    MachineState parent_obj;
    /*< public >*/

    SSD202DSoCState soc;
    MiyooMiniKeysState keys;
    qemu_irq pwrkey;            /* HACK: the power-button pad, held at boot */
    QEMUTimer pwrkey_release;   /* HACK: lets go of it once booted */
    struct arm_boot_info binfo;
};

static void miyoomini_pwrkey_release(void *opaque)
{
    MiyooMiniMachineState *s = MIYOOMINI_MACHINE(opaque);

    qemu_set_irq(s->pwrkey, 0);
}

static void miyoomini_keys_event(DeviceState *dev, QemuConsole *src,
                                 QemuInputEvent *evt)
{
    MiyooMiniKeysState *s = MIYOOMINI_KEYS(dev);
    int i;

    /* HACK: input trace to help map keys to pads; remove with the hack */
    fprintf(stderr, "INPUT key=%d down=%d\n", evt->key.key, evt->key.down);
    for (i = 0; i < ARRAY_SIZE(miyoomini_buttons); i++) {
        const MiyooMiniButton *b = &miyoomini_buttons[i];

        if (b->key == evt->key.key) {
            /* active low: pressed pulls the pad to 0, released to 1 */
            fprintf(stderr, "INPUT   -> %s %s\n", b->name,
                    evt->key.down ? "press" : "release");
            qemu_set_irq(s->pads[i], !evt->key.down);
            break;
        }
    }
}

static const QemuInputHandler miyoomini_keys_handler = {
    .name = "miyoomini-keys",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = miyoomini_keys_event,
};

static void miyoomini_keys_init(Object *obj)
{
    MiyooMiniKeysState *s = MIYOOMINI_KEYS(obj);

    qdev_init_gpio_out(DEVICE(obj), s->pads, ARRAY_SIZE(miyoomini_buttons));
}

static void miyoomini_keys_realize(DeviceState *dev, Error **errp)
{
    MiyooMiniKeysState *s = MIYOOMINI_KEYS(dev);

    s->hs = qemu_input_handler_register(dev, &miyoomini_keys_handler);
}

static void miyoomini_keys_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = miyoomini_keys_realize;
    /* Created by the machine, not -device */
    dc->user_creatable = false;
}

static void miyoomini_load_bootrom(MachineState *machine, SSD202DSoCState *soc)
{
    const char *bios_name = machine->firmware ?: SSD202D_BOOTROM_FILENAME;
    g_autofree char *filename = NULL;

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (!filename) {
        error_report("Could not find boot ROM image '%s'", bios_name);
        if (!machine->kernel_filename) {
            /* Nothing to run without a boot ROM or a kernel */
            exit(1);
        }
        return;
    }
    if (load_image_mr(filename, &soc->bootrom) < 0) {
        error_report("Failed to load boot ROM image '%s'", filename);
        exit(1);
    }
}

static void miyoomini_init(MachineState *machine)
{
    MiyooMiniMachineState *s = MIYOOMINI_MACHINE(machine);
    MStarV7SoCState *soc;
    DriveInfo *dinfo;
    DeviceState *panel;
    uint64_t offset;
    int i;

    /* The DRAM is inside the SoC package so its size is fixed */
    if (machine->ram_size != SSD202D_DRAM_SIZE) {
        char *size_str = size_to_str(SSD202D_DRAM_SIZE);
        error_report("Invalid RAM size, should be %s", size_str);
        g_free(size_str);
        exit(1);
    }

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_SSD202D_SOC);

    /*
     * The SPI NOR image (-drive if=mtd) is picked up by the SoC's FSP/ISP
     * controller itself: it attaches the flash to its SSI bus and mirrors it
     * into the XIP window (see hw/ssi/mstar_fsp.c).
     */

    /*
     * The panel is board specific: it hangs off the SoC's DSI
     * controller on the point-to-point MIPI link. Create it and link
     * it to the controller before the SoC (and its DSI) is realized.
     */
    panel = qdev_new(TYPE_DSI_DCS_PANEL);
    object_property_add_child(OBJECT(machine), "panel", OBJECT(panel));
    qdev_realize_and_unref(panel, NULL, &error_fatal);
    object_property_set_link(OBJECT(&MSTARV7_SOC(&s->soc)->dsi), "panel",
                             OBJECT(panel), &error_fatal);

    /* The panel is mounted upside down on this board */
    object_property_set_bool(OBJECT(&MSTARV7_SOC(&s->soc)->disp), "flip",
                             true, &error_fatal);

    /* HACK: the battery hangs off SAR ADC channel 0 (provisional level) */
    object_property_set_uint(OBJECT(&MSTARV7_SOC(&s->soc)->sar), "channel0",
                             MIYOOMINI_BATTERY_SAR_CH0, &error_fatal);

    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /*
     * The Miyoo Mini carries a Neowine ALPU-FA copy-protection chip on
     * i2c bus 1 at 0x3d; the vendor kernel and MainUI refuse to run
     * without it. It is board specific, so it is wired here rather
     * than in the SoC.
     */
    i2c_slave_create_simple(MSTARV7_SOC(&s->soc)->i2c[1].bus, TYPE_ALPU, 0x3d);

    /*
     * The board buttons: each drives its SoC GPIO pad from a key on
     * the QEMU keyboard, and every active-low button's pad idles high
     * behind its pull-up.
     */
    object_initialize_child(OBJECT(machine), "keys", &s->keys,
                            TYPE_MIYOOMINI_KEYS);
    qdev_realize(DEVICE(&s->keys), NULL, &error_fatal);
    for (i = 0; i < ARRAY_SIZE(miyoomini_buttons); i++) {
        const MiyooMiniButton *b = &miyoomini_buttons[i];
        qemu_irq pad = qdev_get_gpio_in_named(DEVICE(&MSTARV7_SOC(&s->soc)->gpio),
                                              b->pm_bank ? MSTAR_GPIO_PM_PAD
                                                         : MSTAR_GPIO_MAIN_PAD,
                                              b->offset / 4);

        qdev_connect_gpio_out(DEVICE(&s->keys), i, pad);
        /* Idle high behind the pull-up, until a press pulls it low */
        qemu_irq_raise(pad);
    }

    /*
     * HACK (see the comment on MIYOOMINI_CHARGER_PM_PAD): keep the UI up
     * by faking "charger plugged in" so keymon does not idle-off, and by
     * holding the power button at boot (released shortly after) so MainUI
     * shows the menu rather than the charging screen.
     */
    qemu_irq_raise(qdev_get_gpio_in_named(DEVICE(&MSTARV7_SOC(&s->soc)->gpio),
                                          MSTAR_GPIO_PM_PAD,
                                          MIYOOMINI_CHARGER_PM_PAD / 4));
    s->pwrkey = qdev_get_gpio_in_named(DEVICE(&MSTARV7_SOC(&s->soc)->gpio),
                                       MSTAR_GPIO_MAIN_PAD,
                                       MIYOOMINI_PWRKEY_MAIN_PAD / 4);
    qemu_irq_raise(s->pwrkey);
    timer_init_ns(&s->pwrkey_release, QEMU_CLOCK_VIRTUAL,
                  miyoomini_pwrkey_release, s);
    timer_mod(&s->pwrkey_release, MIYOOMINI_PWRKEY_RELEASE_NS);

    /* An SD card in the slot, if the user supplied one with -drive if=sd */
    dinfo = drive_get(IF_SD, 0, 0);
    if (dinfo) {
        BusState *sdbus = qdev_get_child_bus(DEVICE(&MSTARV7_SOC(&s->soc)->fcie),
                                             "sd-bus");
        DeviceState *card = qdev_new(TYPE_SD_CARD);

        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
        qdev_realize_and_unref(card, sdbus, &error_fatal);
    }

    memory_region_add_subregion(get_system_memory(), MSTARV7_MIU0_BASE,
                                machine->ram);

    /*
     * The DRAM wraps in the MIU window; the IPL sizes memory by
     * writing markers and looking for where they alias back.
     */
    for (offset = machine->ram_size;
         offset + machine->ram_size <= MSTARV7_MIU0_WINDOW;
         offset += machine->ram_size) {
        MemoryRegion *mirror = g_new(MemoryRegion, 1);

        memory_region_init_alias(mirror, OBJECT(machine), "miyoomini.mirror",
                                 machine->ram, 0, machine->ram_size);
        memory_region_add_subregion(get_system_memory(),
                                    MSTARV7_MIU0_BASE + offset, mirror);
    }

    miyoomini_load_bootrom(machine, &s->soc);

    soc = MSTARV7_SOC(&s->soc);
    s->binfo.loader_start = MSTARV7_MIU0_BASE;
    s->binfo.ram_size = machine->ram_size;
    arm_load_kernel(&soc->cpus[0], machine, &s->binfo);
}

static void miyoomini_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a7"),
        NULL
    };

    mc->desc = "Miyoo Mini (SigmaStar SSD202D)";
    mc->init = miyoomini_init;
    mc->min_cpus = INFINITY2M_NUM_CPUS;
    mc->max_cpus = INFINITY2M_NUM_CPUS;
    mc->default_cpus = INFINITY2M_NUM_CPUS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = SSD202D_DRAM_SIZE;
    mc->default_ram_id = "miyoomini.ram";
    mc->no_parallel = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
}

/*
 * Miyoo Mini Plus: same SoC, ALPU and boot flow as the Mini, plus an
 * X-Powers AXP223 PMIC on i2c bus 1 at 0x34 that the vendor axp_test
 * tool drives from userspace. The Plus panel, button pads and Wi-Fi are
 * not modelled separately yet.
 */
static void miyoominiplus_init(MachineState *machine)
{
    MiyooMiniMachineState *s = MIYOOMINI_MACHINE(machine);

    miyoomini_init(machine);
    i2c_slave_create_simple(MSTARV7_SOC(&s->soc)->i2c[1].bus, TYPE_AXP22X,
                            0x34);
}

static void miyoominiplus_machine_class_init(ObjectClass *oc,
                                             const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Miyoo Mini Plus (SigmaStar SSD202D, AXP223)";
    mc->init = miyoominiplus_init;
}

static const TypeInfo miyoomini_machine_types[] = {
    {
        .name           = TYPE_MIYOOMINI_MACHINE,
        .parent         = TYPE_MACHINE,
        .instance_size  = sizeof(MiyooMiniMachineState),
        .class_init     = miyoomini_machine_class_init,
        .interfaces     = arm_machine_interfaces,
    },
    {
        .name           = TYPE_MIYOOMINIPLUS_MACHINE,
        .parent         = TYPE_MIYOOMINI_MACHINE,
        .class_init     = miyoominiplus_machine_class_init,
        .interfaces     = arm_machine_interfaces,
    },
    {
        .name           = TYPE_MIYOOMINI_KEYS,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(MiyooMiniKeysState),
        .instance_init  = miyoomini_keys_init,
        .class_init     = miyoomini_keys_class_init,
    },
};

DEFINE_TYPES(miyoomini_machine_types)
