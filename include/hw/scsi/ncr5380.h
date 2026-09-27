/*
 * NCR 5380 SCSI controller
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef HW_SCSI_NCR5380_H
#define HW_SCSI_NCR5380_H

#include "hw/core/sysbus.h"
#include "hw/scsi/scsi.h"
#include "qom/object.h"

#define TYPE_NCR5380 "ncr5380"
OBJECT_DECLARE_SIMPLE_TYPE(NCR5380State, NCR5380)

struct NCR5380State {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    SCSIBus bus;
    SCSIDevice *dev;
    SCSIRequest *req;

    uint8_t reg_shift;
    bool sun_mode;      /* Sun si: arm selection on MR ARBITRATE (target=MR&7) */
    uint8_t sun_fifo[16];       /* Sun si: CDB staged via reg0 writes pre-select */
    uint8_t sun_fifo_count;
    /*
     * Sun si data-in DMA sub-state.  The PROM's driver reads the live SCSI
     * phase from reg2 (low 3 bits, standard SCSI encoding) and drives a
     * three-step data-in handshake: it first sees the DATA phase (reg2==1)
     * and *arms* the si DMA (writes dma_addr + si_csr|=SBC_IP); the si then
     * moves the bytes and the driver, seeing the transfer done (reg2==3),
     * finalises it by reading back the auto-incremented dma_addr (the byte
     * count it validates); finally it reads STATUS (reg2==7).  sun_dma_done
     * gates the reg2 1->3 transition: false while data is pending in the
     * data phase, true once the si DMA engine has drained it.
     */
    bool sun_dma_done;
    /*
     * Sun si: one-shot latch bridging the gap between the si DMA engine
     * draining a data phase and the driver's own subsequent register
     * writes finalising it.  Real Sun 3/80 hardware re-asserts /REQ the
     * instant the si drains a data phase (the target has moved on to the
     * next phase); this model's pseudo-DMA burst (ncr5380_pdma_read())
     * correctly clears CSB_REQ per byte but nothing then re-asserts it,
     * so the kernel "sm" driver's very next reg3 (TCR_R) poll -- which
     * keys its whole dispatch on CSB_REQ (0x08 = phase pending, 0x20 =
     * state/BSR dispatch) -- mis-routes into the state dispatcher and
     * declares "Lost Busy".  Re-asserting CSB_REQ itself at drain time
     * does not survive: the driver's own DMA-arm-strobe ICR pulse
     * (ICR=0x90, bit7 sun-mode arm-strobe + bit4 ASSERT_ACK) fires
     * ncr5380_ack() immediately afterward, whose unconditional top-of-
     * function `csb &= ~CSB_REQ` clobbers it again before the driver's
     * poll ever observes it.  This latch lives entirely outside
     * ncr5380_ack()/ncr5380_ack_release() (touched nowhere else) so it
     * cannot regress either function's other, unrelated callers (notably
     * this board's own PROM self-test, which drives the raw 5380
     * registers directly and depends on ncr5380_ack_release()'s generic,
     * unconditional STATUS transition).  Set by ncr5380_sun_dma_drained()
     * at drain time; consumed by exactly the one reg3 read that would
     * otherwise mis-route.
     */
    bool sun_dma_drained_pending;
    /*
     * Sun si: set once a *blind* (ufsboot / sd-open) command has been torn
     * down to bus-free by the blind reg0 STATUS/MESSAGE read path.  While set
     * (and dev==NULL) sun_phase_bits() presents reg2==7 (MESSAGE IN) so the
     * driver's state machine, called a fixed number of trailing times after
     * completion, re-takes its benign success branch (phase 7) instead of the
     * phase-0 retry-exhausted path (err5) that made the sd-open TUR return -1
     * -> "bdevvp: bad open".  Cleared on the next selection (MR ARBITRATE).
     * Only the blind path sets it; the PROM probe's ACK-handshake teardown
     * leaves it clear, so the PROM path's trailing calls stay benign (phase 0).
     */
    bool sun_cmd_complete;
    /*
     * Sun si trailing-completion sub-state.  After sun_cmd_complete, the
     * driver's state machine needs two more calls to reach its terminal
     * "done" state (a4@0 == 0): the first (state 27) must route through the
     * *phase* dispatcher (reg3 == 0x08) to read the final MESSAGE byte and
     * advance to state 32; the second (state 32) must route through the
     * *BSR* dispatcher (reg3 == 0x20) to complete to state 0.  reg3 therefore
     * reads 0x08 until the trailing MESSAGE byte (reg0) has been taken, then
     * 0x20.  sun_msg_taken records that transition.  Cleared on selection.
     */
    bool sun_msg_taken;
    /*
     * Sun si: set for one si_csr read when the previous selection found no
     * device (ncr5380_select_target()'s "no BSY appears" branch).  Real si
     * hardware's select sequencer always completes and interrupts, success or
     * failure -- it is the driver's software that then figures out from
     * register state whether a target answered.  The kernel "sm" driver's
     * interrupt handler (recovered from a symbol-matched standalone
     * kernel) gates almost everything on SI_CSR_SBC_IP
     * (bit 9 of the 32-bit CSR/DMA-address read); with that clear it no-ops
     * silently.  With it set, its very next check is SI_CSR_FIFO_EMPTY
     * (bit 10): if set, it spins a bounded ~1e6 iterations waiting for it to
     * clear and, finding it still empty (true here -- no data phase ever
     * ran), records its own "err 0x12" and returns claimed.  Presenting
     * SBC_IP|FIFO_EMPTY for exactly one read after a failed selection drives
     * that existing, otherwise-dead driver path instead of leaving it with
     * nothing to ever wake it.  One-shot: cleared by the read that consumes
     * it (sun3x_sireg_read()), so it never masquerades as a later stale IRQ.
     */
    bool sun_sel_failed;
    /*
     * Sun si: set (with sun_cmd_irq_reads below) when a *kernel-mode,
     * interrupt-driven* command has just completed -- the ACK that takes
     * the final MESSAGE IN byte in ncr5380_ack()'s kernel-gated PHASE_MI
     * branch (unlike the PROM/ufsboot blind-read completion, which already
     * raises BSR_IRQ and is serviced by that regime's own busy-poll).  Real
     * si hardware interrupts on this transition; the kernel "sm" driver's
     * real I/O path (as opposed to the busy-poll used throughout autoconfig)
     * sleeps waiting for exactly that trap.  sun3x_sireg_read() presents
     * bit1 of si_csr (the actual gate _sm_intr's dispatch chain tests --
     * not SBC_IP/bit9) for the next sun_cmd_irq_reads si_csr reads,
     * driving its dispatch through to the driver's real "done" state
     * instead of leaving it either unclaimed or mis-routed.  Cleared
     * (with the counter) once those reads are used up, or on the next
     * selection.
     */
    bool sun_cmd_irq;
    /*
     * Sun si: how many more si_csr reads should still see bit1 set once
     * sun_cmd_irq is pending, with NO guest-PC gating.  A PC gate on
     * _sm_intr's text locks the mechanism to one kernel binary's link
     * addresses (the installed GENERIC kernel links the identical function
     * ~0x4a000 bytes away from the MINIROOT's and stormed "iobus level 2
     * interrupt not serviced").  The access *pattern* is what is
     * invariant (~115 traced completions, cross-checked under sustained
     * dd I/O): one si_csr read from the "state-machine inner loop" (a
     * busy/bit0-only test, indifferent to bit1) immediately followed by
     * exactly three consecutive reads that all need bit1 (the isrlink
     * loop's claim test, the per-device recheck, the state-machine entry
     * gate).  Starting the counter at 4 reproduces those three dispatch
     * reads seeing bit1 regardless of where the kernel is linked.
     */
    int sun_cmd_irq_reads;

    /* register file */
    uint8_t odr;        /* output data */
    uint8_t icr;        /* initiator command */
    uint8_t mr;         /* mode */
    uint8_t tcr;        /* target command */
    uint8_t ser;        /* select enable */
    uint8_t csb;        /* current SCSI bus status (read reg 4) */
    uint8_t bsr;        /* bus and status (read reg 5) */
    uint8_t last_data;  /* last byte latched on the data lines */

    /* bus/transaction state */
    uint8_t phase;
    int target;
    int lun;
    uint8_t cmd[16];
    int cmd_len;
    uint8_t status;
    uint8_t dma_mode;
    bool status_done;
    bool msg_done;

    /*
     * Whole-transfer data buffer, owned by the controller.  Data-in
     * transfers are pre-read in full at do_command time so the guest's
     * blind/pseudo-DMA reads never race an in-flight aiocb; data-out
     * transfers are collected here in full before being fed to the
     * SCSI layer.
     */
    uint8_t *dbuf;
    uint32_t dbuf_size;
    uint32_t buf_len;
    uint32_t buf_pos;
    uint32_t out_pos;       /* data-out: bytes already fed to the device */
    bool collecting;        /* inside the synchronous transfer pump */
    bool cmd_done;          /* completion callback has run */
};

bool ncr5380_pdma_ready(NCR5380State *s, bool out);
uint8_t ncr5380_pdma_read(NCR5380State *s);
void ncr5380_pdma_write(NCR5380State *s, uint8_t val);
/* Sun si: move the bus to STATUS once the driver has finalised a data phase */
void ncr5380_sun_to_status(NCR5380State *s);
/*
 * Sun si: latch that the si DMA engine has just drained a data phase, so
 * the driver's very next reg3 poll still sees "phase pending" (0x08) even
 * though CSB_REQ itself will be clobbered before that poll happens.  See
 * the sun_dma_drained_pending comment in ncr5380.h.
 */
void ncr5380_sun_dma_drained(NCR5380State *s);
/*
 * Sun si: drop a stale, unclaimable BSR_IRQ.  See the comment at the call
 * site in hw/m68k/sun3x.c's si_csr claim-bit read.
 */
void ncr5380_sun_reap_stale_irq(NCR5380State *s);
/*
 * Sun si: a genuine SCSI bus reset assertion (si_csr bit 0x80, live-traced
 * -- see the comment at the call site in hw/m68k/sun3x.c and the
 * implementation in ncr5380.c). Cancels any still-open SCSI request (so a
 * WRITE aborted mid-transfer by a real reset never later completes to the
 * block layer with partial/stale data) and clears the rest of the bus/
 * transaction state, same as the pre-existing non-sun ICR-RST path.
 */
void ncr5380_sun_bus_reset(NCR5380State *s);

#endif
