/*
 * MStar/SigmaStar ARMv7 SoC base class
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_ARM_MSTARV7_H
#define HW_ARM_MSTARV7_H

#include "qemu/units.h"
#include "system/memory.h"
#include "hw/adc/mstar_sar.h"
#include "hw/audio/mstar_bach.h"
#include "hw/cpu/a15mpcore.h"
#include "hw/display/mstar_disp.h"
#include "hw/display/mstar_dphy.h"
#include "hw/display/mstar_dsi.h"
#include "hw/display/mstar_ge.h"
#include "hw/dma/mstar_bdma.h"
#include "hw/gpio/mstar_gpio.h"
#include "hw/i2c/mstar_i2c.h"
#include "hw/intc/mstar_intc.h"
#include "hw/misc/mstar_cpupll.h"
#include "hw/misc/mstar_pwm.h"
#include "hw/misc/mstar_regbank.h"
#include "hw/rtc/mstar_rtcpwc.h"
#include "hw/sd/mstar_fcie.h"
#include "hw/ssi/mstar_fsp.h"
#include "hw/timer/mstar_timer.h"
#include "hw/watchdog/mstar_wdt.h"
#include "target/arm/cpu.h"
#include "qom/object.h"

#define TYPE_MSTARV7_SOC "mstarv7-soc"
OBJECT_DECLARE_TYPE(MStarV7SoCState, MStarV7SoCClass, MSTARV7_SOC)

/* The largest core count of any MStar ARMv7 SoC (dual Cortex-A7) */
#define MSTARV7_SOC_MAX_CPUS 2

/*
 * Physical addresses shared by every MStar ARMv7 SoC.
 *
 * Source: mainline Linux arch/arm/boot/dts/mstar-v7.dtsi
 */
/*
 * Start of DRAM, the MIU0 address space. The window decodes 512 MiB;
 * smaller DRAM wraps within it, which is how the IPL sizes memory.
 */
#define MSTARV7_MIU0_BASE       0x20000000
#define MSTARV7_MIU0_WINDOW     (512 * MiB)
/* The RIU register bus, where almost all peripherals live */
#define MSTARV7_RIU_BASE        0x1f000000
#define MSTARV7_RIU_SIZE        0x00400000
/* Cortex-A7 PERIPHBASE (read back via CBAR); the GIC lives in here */
#define MSTARV7_PERIPHBASE      0x16000000
/* SCU/GIC/timer private region size, from the Cortex-A7 MPCore TRM */
#define MSTARV7_PERIPHBASE_SIZE 0x8000
/*
 * ARM generic timer frequency. The vendor device tree pins the
 * arm,armv7-timer at 6MHz (mstar-v7.dtsi: clock-frequency = <6000000>),
 * which is what the kernel uses to convert counter ticks to time. The
 * model must advance the counter at the same rate.
 */
#define MSTARV7_CNTFRQ_HZ       6000000
/* GIC-400 SPI count, from the previous branch */
#define MSTARV7_GIC_NUM_SPI     128
/* IMI SRAM; the size varies between SoCs */
#define MSTARV7_IMI_BASE        0xa0000000
/*
 * The "DID" block. The boot ROM reads the boot-media strap from
 * DID_KEY bits[5:2] to decide where to load the IPL from.
 */
#define MSTARV7_DID_BASE        (MSTARV7_RIU_BASE + 0x7000)
#define MSTARV7_DID_SIZE        0x200
#define MSTARV7_DID_KEY         0x1c0
#define MSTARV7_BOOT_MEDIA_SPI_NOR      0x20
#define MSTARV7_BOOT_MEDIA_NAND         0x10
#define MSTARV7_BOOT_MEDIA_SPI_NAND     0x08
/*
 * The two mst-intc instances between the peripherals and the GIC:
 * "IRQ" lines land on GIC SPI 32 up, "FIQ" lines on SPI 96 up.
 */
#define MSTARV7_INTC_IRQ_BASE   (MSTARV7_RIU_BASE + 0x201350)
#define MSTARV7_INTC_IRQ_NUM    64
#define MSTARV7_INTC_IRQ_START  32
#define MSTARV7_INTC_FIQ_BASE   (MSTARV7_RIU_BASE + 0x201310)
#define MSTARV7_INTC_FIQ_NUM    32
#define MSTARV7_INTC_FIQ_START  96
/* The PM UART's line on the "IRQ" mst-intc */
#define MSTARV7_PM_UART_INTC_IRQ 34
/*
 * The clkgen bank. Software programs clock gates/muxes here and
 * derives clock rates from reading them back, so it must retain what
 * is written; modelled as plain storage.
 */
#define MSTARV7_CLKGEN_BASE     (MSTARV7_RIU_BASE + 0x207000)
/*
 * The PM-domain clkgen bank holds the always-on clock gates - CLK_pwm,
 * CLK_spi, CLK_ir, CLK_rtc, CLK_sar and CLK_pm_sleep - plus the SAR
 * conversion trigger the temperature read path pulses. Another plain
 * readback bank: the busiest one after the flash write-protect, since
 * every temperature sample pokes it.
 */
#define MSTARV7_PM_CLKGEN_BASE  (MSTARV7_RIU_BASE + 0x1c00)
/*
 * The "chiptop" block: the pin-mux (pinctrl) pad control together with
 * the chip straps. The package "bond" strap at +0x120 identifies the
 * package/DRAM variant (0x1d SSD201/64 MiB, 0x1e SSD202D/128 MiB).
 * Software programs the pad-mux registers and reads them back, so the
 * block is modelled as readback storage plus the read-only bond strap.
 */
#define MSTARV7_CHIPTOP_BASE    (MSTARV7_RIU_BASE + 0x203c00)
#define MSTARV7_CHIPTOP_SIZE    0x200
#define MSTARV7_CHIPTOP_NUM_REGS (MSTARV7_CHIPTOP_SIZE / 4)
#define MSTARV7_CHIPTOP_BOND    0x120
/*
 * The chip-version bank (RIU bank 0x1e, 0x1f003c00). The vendor GOP code
 * reads the chip version from +0x19c (0x1f003d9c); bit 8 distinguishes the
 * revision. Captured as 0x0100 on a real SSD202D (see contrib/mstarpoker).
 */
#define MSTARV7_CHIPVER_BASE    (MSTARV7_RIU_BASE + 0x3c00)
#define MSTARV7_CHIPVER_REG     0x19c
#define MSTARV7_CHIPVER_VALUE   0x0100
#define MSTARV7_CHIPVER_ID      0x00f0  /* +0x00: chip ID byte (0xf0 = SSD20XD) */
/*
 * The efuse / OTP config-readback bank (RIU bank 0x20, 0x1f004000). The boot
 * ROM and vendor IPL read it (read_config_word) for the DDR/PHY calibration and
 * per-board USB/ETH/MIPI/HDMI analog trims. Captured from a real SSD202D via
 * contrib/mstarpoker; without it these regs read 0 and every trim is skipped.
 */
#define MSTARV7_EFUSE_BASE      (MSTARV7_RIU_BASE + 0x4000)
/* The two HWI2C masters (i2c@223000 and i2c@223200) */
#define MSTARV7_NUM_I2C         2
#define MSTARV7_I2C_BASE        (MSTARV7_RIU_BASE + 0x223000)
#define MSTARV7_I2C_STRIDE      0x200
/* Their lines on the "IRQ" mst-intc (36 and 37, as in the mainline DT) */
#define MSTARV7_I2C_INTC_IRQ    36
/* The SAR ADC; boards hang keypads off its input channels */
#define MSTARV7_SAR_BASE        (MSTARV7_RIU_BASE + 0x2800)
/* The analog CPU PLL; the Cortex-A7 clock (and cpufreq) derive from it */
#define MSTARV7_CPUPLL_BASE     (MSTARV7_RIU_BASE + 0x206400)
/* The PWM controller; on the Miyoo Mini one channel is the LCD backlight */
#define MSTARV7_PWM_BASE        (MSTARV7_RIU_BASE + 0x3400)
/* The watchdog timer (sstar,infinity-wdt) */
#define MSTARV7_WDT_BASE        (MSTARV7_RIU_BASE + 0x6000)
/* The FCIE SD/MMC host controller */
#define MSTARV7_FCIE_BASE       (MSTARV7_RIU_BASE + 0x282000)
/*
 * Its "MIE" (MStar interrupt-event) line, on which the sdmmc driver
 * waits for command/data completion. The vendor kernel names it
 * ms_sdmmc_mie and routes it through the main-intc as hwirq 51, i.e.
 * "IRQ" mst-intc line 51 - INTC_IRQ_START.
 */
#define MSTARV7_FCIE_INTC_IRQ   (51 - MSTARV7_INTC_IRQ_START)
/*
 * The display output path to the panel: the MIPI DSI controller and
 * the analog D-PHY it drives. The rest of the display pipe (GOP, the
 * display top and MOP overlay) is not modelled yet.
 */
#define MSTARV7_DSI_BASE        (MSTARV7_RIU_BASE + 0x345200)
#define MSTARV7_DPHY_BASE       (MSTARV7_RIU_BASE + 0x2a5000)
/*
 * The display controller front end: the GOP framebuffer scanout and
 * the display top (frame timing and vsync). Their vsync interrupts
 * land on the "IRQ" mst-intc.
 */
#define MSTARV7_DISP_GOP_BASE   (MSTARV7_RIU_BASE + 0x246800)
#define MSTARV7_DISP_TOP_BASE   (MSTARV7_RIU_BASE + 0x225000)
#define MSTARV7_DISP_MOP_BASE   (MSTARV7_RIU_BASE + 0x280a00)
#define MSTARV7_DISP_TOP_INTC_IRQ 50
#define MSTARV7_DISP_GOP_INTC_IRQ 20
#define MSTARV7_GE_INTC_IRQ 25
/*
 * The GE 2D graphics engine. MI_GFX/MainUI composites the on-screen UI
 * through it; its bitblts must run for anything to appear on the panel.
 */
#define MSTARV7_DISP_GE_BASE    (MSTARV7_RIU_BASE + 0x281200)
/*
 * Display-pipeline configuration/timing banks that the panel bring-up
 * programs and reads back but which do not feed the functional scanout
 * (the disp model reads the framebuffer state directly), plus the USB
 * UTMI PHY banks (0x283e00-0x286200) that the vendor IPL powers up and
 * that were formerly mislabelled display "scalers". Modelled as plain
 * readback banks; see docs/system/arm/mstarv7/display.rst for the
 * per-bank register maps. The base of each is in mstarv7_disp_cfg_base[].
 */
#define MSTARV7_NUM_DISP_CFG    19
/*
 * The "bach" audio controller and its "audiotop" codec syscon. The
 * vendor MI_AO audio path (the chime the boot flow plays, MainUI's
 * SDL audio) drives the reader DMA sub-channel here and waits on its
 * underrun interrupt.
 */
#define MSTARV7_BACH_BASE       (MSTARV7_RIU_BASE + 0x2a0400)
#define MSTARV7_AUDIOTOP_BASE   (MSTARV7_RIU_BASE + 0x206800)
#define MSTARV7_BACH_INTC_IRQ   42
/*
 * The GPIO pad banks: the main bank (gpio@207800 in the mainline
 * device trees) and the PM bank in the always-on domain. Boards hang
 * buttons off both.
 */
#define MSTARV7_GPIO_BASE       (MSTARV7_RIU_BASE + 0x207800)
#define MSTARV7_PM_GPIO_BASE    (MSTARV7_RIU_BASE + 0x1e00)
/*
 * The RTC power/wake controller (device tree "sstar,infinity-rtcpwc"). The
 * vendor RTC driver reads the always-on RTC counter through it, and the
 * firmware's power management reads its power-source status; without it the
 * RTC bring-up fails and MainUI reboots thinking the system is unpowered.
 */
#define MSTARV7_RTCPWC_BASE     (MSTARV7_RIU_BASE + 0x006800)
/*
 * The ISP SPI NOR controller: the FSP command sequencer bank and the
 * memory mapped XIP read window the flash contents appear in.
 */
#define MSTARV7_FSP_BASE        (MSTARV7_RIU_BASE + 0x2c00)
#define MSTARV7_ISP_BASE        (MSTARV7_RIU_BASE + 0x1000)
#define MSTARV7_ISP_XIP_BASE    0x14000000
/* The BDMA engine; the boot ROM DMAs the IPL from flash into IMI */
#define MSTARV7_BDMA_BASE       (MSTARV7_RIU_BASE + 0x200400)
/*
 * The MIU DDR controller: an analog block (PLL/PHY), an arbiter and
 * a digital block. Modelled just enough for the IPL's DDR bring-up:
 * training and the memory BIST complete instantly.
 */
#define MSTARV7_MIU_BASE        (MSTARV7_RIU_BASE + 0x202000)
#define MSTARV7_MIU_SIZE        0x1000
#define MSTARV7_MIU_ANA_DDFSET_L        0x060
#define MSTARV7_MIU_ANA_DDFSET_H        0x064
#define MSTARV7_MIU_DIG_CNTRL0          0x400
#define MSTARV7_MIU_DIG_CNTRL0_INITDONE (1 << 15)
#define MSTARV7_MIU_DIG_BIST_CTRL       0x5c0
#define MSTARV7_MIU_DIG_BIST_DONE       (1 << 15)
/*
 * What the vendor IPL programs into the DDR PLL frequency-set pair;
 * reported back so the kernel's MIU clock driver computes a sane
 * DDR rate instead of dividing by zero.
 */
#define MSTARV7_MIU_DDFSET_L_VALUE      0x8000
#define MSTARV7_MIU_DDFSET_H_VALUE      0x0029
/*
 * The smpctrl secondary-core boot mailbox. On real hardware CPU1
 * runs the mask ROM and parks polling UNLOCK for the magic, then
 * jumps to the address in BOOT_LOW/HIGH; mainline and the vendor
 * kernel both post the entry address this way. In the model CPU1 is
 * powered off instead and the unlock powers it on at that address.
 * Software also spins reading back what it wrote here, so the bank
 * is a plain register file.
 */
#define MSTARV7_SMPCTRL_BASE        (MSTARV7_RIU_BASE + 0x204000)
#define MSTARV7_SMPCTRL_SIZE        0x200
#define MSTARV7_SMPCTRL_NUM_REGS    (MSTARV7_SMPCTRL_SIZE / 4)
#define MSTARV7_SMPCTRL_BOOT_HIGH   0x4c
#define MSTARV7_SMPCTRL_BOOT_LOW    0x50
#define MSTARV7_SMPCTRL_UNLOCK      0x58
#define MSTARV7_SMPCTRL_UNLOCK_MAGIC 0xbabe
/*
 * The l3bridge, used as a write barrier: software pokes the trigger
 * and polls the flush done bit, per mainline's mstarv7 barrier code.
 */
#define MSTARV7_L3BRIDGE_BASE   (MSTARV7_RIU_BASE + 0x204400)
#define MSTARV7_L3BRIDGE_SIZE   0x200
#define MSTARV7_L3BRIDGE_STATUS 0x40
#define MSTARV7_L3BRIDGE_STATUS_DONE (1 << 12)
/*
 * The three timers (timer@6040/6080/60c0 in the device trees). The
 * boot ROM uses the first to time its flash operations.
 */
#define MSTARV7_NUM_TIMERS      3
#define MSTARV7_TIMER_BASE      (MSTARV7_RIU_BASE + 0x6040)
#define MSTARV7_TIMER_STRIDE    0x40
/*
 * The PM UART, a 16550 with the registers on an 8 byte stride. The
 * boot ROM prints its messages here.
 */
#define MSTARV7_PM_UART_BASE    (MSTARV7_RIU_BASE + 0x221000)
#define MSTARV7_PM_UART_REGSHIFT 3
/* The real UART clock is unknown; this only affects the reported baud */
#define MSTARV7_PM_UART_BAUDBASE 115200

struct MStarV7SoCState {
    /*< private >*/
    DeviceState parent_obj;
    /*< public >*/

    ARMCPU cpus[MSTARV7_SOC_MAX_CPUS];
    A15MPPrivState a7mpcore;
    MStarIntcState intc_irq;
    MStarIntcState intc_fiq;
    MemoryRegion imi;
    MemoryRegion did;
    MemoryRegion miu;
    MemoryRegion l3bridge;
    MemoryRegion smpctrl_mr;
    uint16_t smpctrl[MSTARV7_SMPCTRL_NUM_REGS];
    MStarBdmaState bdma;
    MStarFspState fsp;
    MStarSarState sar;
    MStarI2cState i2c[MSTARV7_NUM_I2C];
    MStarRegbankState clkgen;
    MStarRegbankState pm_clkgen;
    MStarRegbankState chipver;
    MStarRegbankState efuse;
    MStarRegbankState disp_cfg[MSTARV7_NUM_DISP_CFG];
    MStarCpupllState cpupll;
    MStarPwmState pwm;
    MStarWdtState wdt;
    MemoryRegion chiptop;
    uint16_t chiptop_regs[MSTARV7_CHIPTOP_NUM_REGS];
    MStarDsiState dsi;
    MStarDphyState dphy;
    MStarDispState disp;
    MStarGeState ge;
    MStarBachState bach;
    MStarGpioState gpio;
    MStarRtcpwcState rtcpwc;
    MStarFcieState fcie;
    MStarTimerState timer[MSTARV7_NUM_TIMERS];

    /* DID_KEY value, i.e. the boot-media strap ("did-key" property) */
    uint16_t did_key;
};

struct MStarV7SoCClass {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/

    /* Number of Cortex-A7 cores in this SoC */
    unsigned num_cpus;
    /* Size of the IMI SRAM */
    uint64_t imi_size;
    /* Input clock of the timers in Hz */
    uint32_t timer_freq;
    /* Package "bond" strap value read back from the chiptop block */
    uint16_t bond;
};

#endif /* HW_ARM_MSTARV7_H */
