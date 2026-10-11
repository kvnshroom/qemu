/*
 * MIPI DSI panels
 *
 * The abstract DSI panel base and a generic DCS panel that interprets
 * the standard MIPI DCS command set. A DSI-attached LCD is configured
 * by the host sending it a sequence of DCS commands (sleep out,
 * display on, address windows, pixel format, ...) before pixel data
 * streams over the link. This models a panel that receives those
 * commands and tracks the state they set, so the host's bring-up
 * sequence has something real to talk to and can be observed.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/display/dsi.h"
#include "trace.h"

/* MIPI DCS commands (MIPI Alliance DCS spec) */
#define DCS_NOP                 0x00
#define DCS_SOFT_RESET          0x01
#define DCS_ENTER_SLEEP_MODE    0x10
#define DCS_EXIT_SLEEP_MODE     0x11
#define DCS_SET_DISPLAY_OFF     0x28
#define DCS_SET_DISPLAY_ON      0x29
#define DCS_SET_COLUMN_ADDRESS  0x2a
#define DCS_SET_PAGE_ADDRESS    0x2b
#define DCS_WRITE_MEMORY_START  0x2c
#define DCS_SET_TEAR_OFF        0x34
#define DCS_SET_TEAR_ON         0x35
#define DCS_SET_ADDRESS_MODE    0x36
#define DCS_SET_PIXEL_FORMAT    0x3a
#define DCS_SET_TEAR_SCANLINE   0x44

OBJECT_DECLARE_SIMPLE_TYPE(DsiDcsPanel, DSI_DCS_PANEL)

struct DsiDcsPanel {
    /*< private >*/
    DsiPanel parent_obj;
    /*< public >*/

    bool awake;             /* exit_sleep_mode seen */
    bool display_on;        /* set_display_on seen */
    bool tear_on;
    uint16_t col_start, col_end;
    uint16_t page_start, page_end;
    uint8_t address_mode;   /* MADCTL */
    uint8_t pixel_format;
};

static uint16_t dcs_pair(const uint8_t *p, unsigned int i, uint32_t len)
{
    uint8_t hi = (2 * i) < len ? p[2 * i] : 0;
    uint8_t lo = (2 * i + 1) < len ? p[2 * i + 1] : 0;

    return (hi << 8) | lo;
}

static void dsi_dcs_panel_dcs(DsiDcsPanel *s, uint8_t cmd,
                              const uint8_t *param, uint32_t plen)
{
    switch (cmd) {
    case DCS_NOP:
        break;
    case DCS_SOFT_RESET:
        s->awake = false;
        s->display_on = false;
        break;
    case DCS_EXIT_SLEEP_MODE:
        s->awake = true;
        break;
    case DCS_ENTER_SLEEP_MODE:
        s->awake = false;
        break;
    case DCS_SET_DISPLAY_ON:
        s->display_on = true;
        break;
    case DCS_SET_DISPLAY_OFF:
        s->display_on = false;
        break;
    case DCS_SET_COLUMN_ADDRESS:
        s->col_start = dcs_pair(param, 0, plen);
        s->col_end = dcs_pair(param, 1, plen);
        break;
    case DCS_SET_PAGE_ADDRESS:
        s->page_start = dcs_pair(param, 0, plen);
        s->page_end = dcs_pair(param, 1, plen);
        break;
    case DCS_SET_TEAR_ON:
        s->tear_on = true;
        break;
    case DCS_SET_TEAR_OFF:
        s->tear_on = false;
        break;
    case DCS_SET_ADDRESS_MODE:
        s->address_mode = plen ? param[0] : 0;
        break;
    case DCS_SET_PIXEL_FORMAT:
        s->pixel_format = plen ? param[0] : 0;
        break;
    case DCS_SET_TEAR_SCANLINE:
    case DCS_WRITE_MEMORY_START:
        break;
    default:
        /* Panel-specific init command; tracked only in the trace */
        break;
    }
}

static void dsi_dcs_panel_receive(DsiPanel *panel, uint8_t data_type,
                                  const uint8_t *payload, uint32_t len)
{
    DsiDcsPanel *s = DSI_DCS_PANEL(panel);
    uint8_t cmd = len ? payload[0] : 0;

    trace_dsi_dcs_panel_cmd(data_type, cmd, len);
    if (trace_event_get_state_backends(TRACE_DSI_DCS_PANEL_PAYLOAD)) {
        /* The whole packet, so a panel's init sequence can be recorded */
        GString *str = g_string_new(NULL);
        uint32_t i;

        for (i = 0; i < len; i++) {
            g_string_append_printf(str, "%s%02x", i ? " " : "", payload[i]);
        }
        trace_dsi_dcs_panel_payload(data_type, str->str);
        g_string_free(str, true);
    }

    switch (data_type) {
    case MIPI_DSI_DCS_SHORT_WRITE:
        dsi_dcs_panel_dcs(s, cmd, NULL, 0);
        break;
    case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
        dsi_dcs_panel_dcs(s, cmd, len > 1 ? &payload[1] : NULL, len > 1);
        break;
    case MIPI_DSI_DCS_LONG_WRITE:
        dsi_dcs_panel_dcs(s, cmd, &payload[1], len > 0 ? len - 1 : 0);
        break;
    case MIPI_DSI_GENERIC_SHORT_WRITE_0:
    case MIPI_DSI_GENERIC_SHORT_WRITE_1:
    case MIPI_DSI_GENERIC_SHORT_WRITE_2:
    case MIPI_DSI_GENERIC_LONG_WRITE:
        /* Generic writes are panel-vendor-specific; nothing to track */
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "dsi-dcs-panel: data type 0x%02x\n",
                      data_type);
        break;
    }
}

static void dsi_dcs_panel_reset(DeviceState *dev)
{
    DsiDcsPanel *s = DSI_DCS_PANEL(dev);

    s->awake = false;
    s->display_on = false;
    s->tear_on = false;
    s->col_start = s->col_end = 0;
    s->page_start = s->page_end = 0;
    s->address_mode = 0;
    s->pixel_format = 0;
}

static void dsi_dcs_panel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    DsiPanelClass *pc = DSI_PANEL_CLASS(oc);

    device_class_set_legacy_reset(dc, dsi_dcs_panel_reset);
    pc->receive = dsi_dcs_panel_receive;
}

static const TypeInfo dsi_panel_types[] = {
    {
        .name           = TYPE_DSI_PANEL,
        .parent         = TYPE_DEVICE,
        .instance_size  = sizeof(DsiPanel),
        .class_size     = sizeof(DsiPanelClass),
        .abstract       = true,
    },
    {
        .name           = TYPE_DSI_DCS_PANEL,
        .parent         = TYPE_DSI_PANEL,
        .instance_size  = sizeof(DsiDcsPanel),
        .class_init     = dsi_dcs_panel_class_init,
    },
};

DEFINE_TYPES(dsi_panel_types)
