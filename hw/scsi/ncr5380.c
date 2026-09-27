/*
 * NCR 5380 SCSI controller (the single-chip initiator of the compact
 * Macs and of the Sun-3 "si" boards).  The CPU drives the SCSI bus
 * phases directly through the chip's registers; data phases go either
 * byte by byte or through the board's pseudo-DMA path.  Backed by QEMU's
 * generic SCSI bus so a scsi-hd/scsi-cd can be attached.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/scsi/scsi.h"
#include "scsi/utils.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/cpu.h"
#include "system/block-backend.h"
#include "hw/scsi/ncr5380.h"
#include "trace.h"

static void ncr5380_ack(NCR5380State *s);
static void ncr5380_ack_release(NCR5380State *s);

/* Write registers */
#define R_ODR    0   /* output data */
#define R_ICR    1   /* initiator command */
#define R_MR     2   /* mode */
#define R_TCR    3   /* target command */
#define R_SER    4   /* select enable */
#define R_DMASND 5   /* start DMA send */
#define R_DMATR  6   /* start DMA target receive */
#define R_DMAIR  7   /* start DMA initiator receive */

/* Read registers */
#define R_CSD    0   /* current SCSI data */
#define R_ICR_R  1
#define R_MR_R   2
#define R_TCR_R  3
#define R_CSB    4   /* current SCSI bus status */
#define R_BSR    5   /* bus and status */
#define R_IDR    6   /* input data */
#define R_RPI    7   /* reset parity/interrupt */

/* ICR bits */
#define ICR_ASSERT_DATA  0x01
#define ICR_ASSERT_ATN   0x02
#define ICR_ASSERT_SEL   0x04
#define ICR_ASSERT_BSY   0x08
#define ICR_ASSERT_ACK   0x10
#define ICR_ARB_LOST     0x20   /* r/o */
#define ICR_ARB_IN_PROG  0x40   /* r/o */
#define ICR_ASSERT_RST   0x80

/* MR bits */
#define MR_ARBITRATE     0x01
#define MR_DMA_MODE      0x02
#define MR_MONITOR_BSY   0x04
#define MR_ENABLE_EOP    0x08
#define MR_ENABLE_PAR    0x10
#define MR_PAR_INT       0x20
#define MR_BSY_INT       0x40   /* interrupt on loss of BSY */
#define MR_TARGET        0x80

/* CSB (read reg 4) bits — active high in the register */
#define CSB_DBP   0x01
#define CSB_SEL   0x02
#define CSB_IO    0x04
#define CSB_CD    0x08
#define CSB_MSG   0x10
#define CSB_REQ   0x20
#define CSB_BSY   0x40
#define CSB_RST   0x80

/* BSR (read reg 5) bits */
#define BSR_ACK        0x01
#define BSR_ATN        0x02
#define BSR_BUSY_ERR   0x04
#define BSR_PHASE_MATCH 0x08
#define BSR_IRQ        0x10
#define BSR_PAR_ERR    0x20
#define BSR_DRQ        0x40
#define BSR_END_DMA    0x80

/* SCSI bus phases (C/D, I/O, MSG bits) */
#define PHASE_DO   0            /* data out */
#define PHASE_DI   CSB_IO       /* data in */
#define PHASE_CMD  CSB_CD       /* command */
#define PHASE_ST   (CSB_CD | CSB_IO)         /* status */
#define PHASE_MI   (CSB_CD | CSB_IO | CSB_MSG) /* message in */
#define PHASE_MO   (CSB_CD | CSB_MSG)          /* message out */

static void ncr5380_do_command(NCR5380State *s);

static uint64_t ncr5380_guest_pc(void)
{
    if (current_cpu) {
        CPUClass *cc = CPU_GET_CLASS(current_cpu);

        if (cc->get_pc) {
            return cc->get_pc(current_cpu);
        }
    }
    return 0;
}


static void ncr5380_update_irq(NCR5380State *s)
{
    int level = (s->bsr & BSR_IRQ) ? 1 : 0;

    qemu_set_irq(s->irq, level);
}

static void ncr5380_set_phase(NCR5380State *s, uint8_t phase)
{
    s->phase = phase;
    s->csb = (s->csb & ~(CSB_CD | CSB_IO | CSB_MSG)) | phase;
}

static void ncr5380_reset_bus(NCR5380State *s)
{
    if (s->req) {
        scsi_req_unref(s->req);
        s->req = NULL;
    }
    s->dev = NULL;
    s->csb = 0;
    s->bsr = 0;
    s->phase = PHASE_DO;
    s->cmd_len = 0;
    s->buf_len = 0;
    s->buf_pos = 0;
    s->out_pos = 0;
    s->collecting = false;
    s->cmd_done = false;
    s->status = 0;
    s->sun_dma_done = false;
    s->sun_dma_drained_pending = false;
    s->sun_cmd_complete = false;
    s->sun_msg_taken = false;
    s->sun_sel_failed = false;
    s->sun_cmd_irq = false;
    s->sun_cmd_irq_reads = 0;
}

static void ncr5380_grow_dbuf(NCR5380State *s, uint32_t size)
{
    if (s->dbuf_size < size) {
        s->dbuf = g_realloc(s->dbuf, size);
        s->dbuf_size = size;
    }
}

/*
 * Run the AIO machinery until the current request's completion callback
 * has fired.  Called with the whole-transfer buffer in place, so every
 * transfer_data callback immediately re-continues the request: the
 * command runs to completion synchronously from the guest's viewpoint.
 */
static void ncr5380_pump(NCR5380State *s)
{
    while (!s->cmd_done && s->req) {
        if (s->dev && s->dev->conf.blk) {
            blk_drain(s->dev->conf.blk);
        } else {
            aio_poll(qemu_get_aio_context(), true);
        }
    }
}

/* all data consumed/delivered: move the bus to the STATUS phase */
static void ncr5380_enter_status(NCR5380State *s)
{
    ncr5380_set_phase(s, PHASE_ST);
    s->last_data = s->status;
    s->status_done = false;
    s->msg_done = false;
    s->bsr &= ~BSR_DRQ;
    /*
     * REQ for the new phase may be asserted only when the initiator is
     * not holding ACK; otherwise ncr5380_ack_release asserts it.
     */
    if (!(s->icr & ICR_ASSERT_ACK)) {
        s->csb |= CSB_REQ;
    }
}

/*
 * The guest delivered the last data-out byte: feed it all to the device.
 *
 * advance_phase selects whether the bus moves straight to STATUS here.
 * The Mac ROM's byte-by-byte ACK'd blind writer (ncr5380_ack_release()'s
 * PHASE_DO case, below) needs that immediate transition -- nothing else
 * will ever advance it.  Sun si DMA-driven writes (ncr5380_pdma_write(),
 * the sun3x_si_dma_run() SEND loop) must NOT: mirroring the DATA IN
 * contract (ncr5380_pdma_read() defers to sun3x.c's
 * ncr5380_sun_to_status(), called once the driver finalises the DMA
 * latches), the kernel "sm" driver's write finalise protocol expects to
 * poll reg2 (sun_phase_bits(), PHASE_DO case) and see "0" while pending,
 * then "3" once sun_dma_done latches, and only THEN see the bus reach
 * STATUS -- advancing phase here, before the driver ever gets to see
 * that "3", desyncs its whole finalise sequence (traced live: reg2 never
 * reads 3 for PHASE_DO at all, the bus is already in STATUS/MESSAGE IN
 * by the time the driver polls, and a stale status_done/msg_done pair
 * left over from the *previous* command on this target makes the
 * driver's own STATUS ACK jump straight to MESSAGE IN and complete the
 * command on completely unvalidated state -- the "sm0: resetting scsi
 * bus" / "write retry" infinite loop, reproducible with a single
 * 512-byte write, cdb "a 0 0 64 1 0").
 */
static void ncr5380_flush_data_out(NCR5380State *s, bool advance_phase)
{
    if (!s->req) {
        return;
    }
    s->out_pos = 0;
    s->collecting = true;
    scsi_req_continue(s->req);
    ncr5380_pump(s);
    s->collecting = false;
    if (!advance_phase) {
        return;
    }
    ncr5380_enter_status(s);
    /*
     * In DMA send mode the empty output register re-asserts DRQ once
     * the target has taken the final byte; the Mac Plus ROM's blind
     * writer waits for that trailing DRQ before it drops DMA mode.
     */
    if (s->mr & MR_DMA_MODE) {
        s->bsr |= BSR_DRQ;
    }
}

static void ncr5380_do_command(NCR5380State *s)
{
    int32_t buflen;

    if (!s->dev) {
        return;
    }
    trace_ncr5380_command(s->cmd[0], s->cmd_len);
    if (trace_event_get_state_backends(TRACE_NCR5380_CMD_PC) && current_cpu) {
        CPUClass *cc = CPU_GET_CLASS(current_cpu);

        if (cc->get_pc) {
            trace_ncr5380_cmd_pc(cc->get_pc(current_cpu));
        }
    }
    /*
     * The Mac ROM's boot reader is "blind": it does not issue TEST UNIT
     * READY / REQUEST SENSE to absorb the power-on UNIT ATTENTION that a
     * freshly reset SCSI device reports.  A real, already-running disk
     * has long since cleared it, so drop any pending unit-attention on
     * the target to match that warm-drive behaviour.
     */
    s->dev->unit_attention.key = 0;
    s->bus.unit_attention.key = 0;
    s->cmd_done = false;
    s->sun_dma_done = false;
    s->sun_dma_drained_pending = false;
    s->buf_len = 0;
    s->buf_pos = 0;
    s->out_pos = 0;
    s->req = scsi_req_new(s->dev, 0, s->lun, s->cmd, s->cmd_len, s);
    buflen = scsi_req_enqueue(s->req);

    if (buflen > 0 && !s->cmd_done) {
        /*
         * Data-in.  Pre-read the ENTIRE transfer synchronously before
         * showing the guest the data phase.  The Mac ROM/SCSI Manager
         * reads blind (pseudo-DMA, or CSD polling with a trailing
         * "handshake until the phase changes" loop): if the DI->ST
         * transition waited on an async aiocb, the trailing loop would
         * clock in phantom stale bytes past the real transfer and
         * corrupt the guest's buffer (observed: 515 phantom bytes after
         * a 512-byte READ -> double MMU fault).
         */
        ncr5380_grow_dbuf(s, buflen);
        s->collecting = true;
        scsi_req_continue(s->req);
        ncr5380_pump(s);
        s->collecting = false;
        if (trace_event_get_state_backends(TRACE_NCR5380_DATAIN)) {
            uint32_t sum = 0, lba = 0, i;

            for (i = 0; i < s->buf_len; i++) {
                sum = (sum * 31 + s->dbuf[i]) & 0xffffffff;
            }
            if (s->cmd[0] == 0x08) {
                lba = ((s->cmd[1] & 0x1f) << 16) | (s->cmd[2] << 8)
                    | s->cmd[3];
            } else if (s->cmd[0] == 0x28) {
                lba = (s->cmd[2] << 24) | (s->cmd[3] << 16)
                    | (s->cmd[4] << 8) | s->cmd[5];
            }
            trace_ncr5380_datain(s->cmd[0], lba, s->buf_len, sum);
        }
        if (s->buf_len > 0) {
            ncr5380_set_phase(s, PHASE_DI);
            s->buf_pos = 0;
            s->last_data = s->dbuf[0];
            s->bsr |= BSR_DRQ;
            if (!(s->icr & ICR_ASSERT_ACK)) {
                s->csb |= CSB_REQ;
            }
        } else {
            ncr5380_enter_status(s);
        }
    } else if (buflen < 0 && !s->cmd_done) {
        /*
         * Data-out: collect the whole transfer from the guest first;
         * ncr5380_flush_data_out feeds it to the device when the last
         * byte arrives.
         */
        ncr5380_grow_dbuf(s, -buflen);
        s->buf_len = -buflen;
        s->buf_pos = 0;
        ncr5380_set_phase(s, PHASE_DO);
        s->bsr |= BSR_DRQ;
        if (!(s->icr & ICR_ASSERT_ACK)) {
            s->csb |= CSB_REQ;
        }
    }
    /*
     * No-data commands complete during scsi_req_enqueue: the completion
     * callback has already moved the bus to the STATUS phase.
     */
}

/* complete selection of an explicit target id */
static void ncr5380_select_target(NCR5380State *s, int id)
{
    SCSIDevice *d = scsi_device_find(&s->bus, 0, id, 0);

    s->dev = d;
    if (s->dev) {
        s->target = id;
        s->csb |= CSB_BSY;              /* target asserted BSY */
        ncr5380_set_phase(s, PHASE_CMD);
        s->csb |= CSB_REQ;
        s->bsr |= BSR_PHASE_MATCH | BSR_IRQ;
        s->cmd_len = 0;
        ncr5380_update_irq(s);
        trace_ncr5380_select_ok(s->target);
        /*
         * Sun si: the CDB was pre-staged into sun_fifo via reg0 writes.  On
         * a real board the si clocks it out during the command phase; here we
         * hand the whole CDB to the target now, which moves the bus to the
         * data (or status) phase the driver then services via DMA.
         */
        if (s->sun_mode && s->sun_fifo_count) {
            int i, clen, n = s->sun_fifo_count;
            int base = 0;

            /*
             * Kernel "sm" I/O (unlike the PROM/ufsboot probe, and unlike the
             * autoconfig-time no-ATN probe launch at 0xf8073178) stages an
             * explicit IDENTIFY message ahead of the CDB -- built at
             * 0xf8073040..0xf807304e as 0xC0|lun -- and asserts ATN when it
             * launches (ICR=0x42 at 0xf8073170, vs. the plain 0x41 the probe
             * uses).  Our synthetic "select on MR write" mechanism runs the
             * whole selection+command synchronously, before the driver's own
             * ATN write even happens, so we cannot key off ICR_ASSERT_ATN at
             * this point; instead recognise the IDENTIFY byte itself.  SCSI-2
             * IDENTIFY messages always have bit 7 set (0x80-0xff); none of
             * the real opcodes this driver issues (0x00 TUR, 0x03 REQUEST
             * SENSE, 0x08/0x28 READ, 0x0a/0x2a WRITE, 0x12 INQUIRY, 0x15/0x1a
             * MODE SELECT/SENSE, 0x1b START STOP, 0x25 READ CAPACITY, 0x35
             * SYNCHRONIZE CACHE, ...) do.  Treating the whole FIFO (message
             * byte included) as the CDB hands the SCSI layer a bogus 0xC0
             * "vendor command", which the disk answers with CHECK CONDITION
             * instead of GOOD -- desyncing the driver's phase/message state
             * machine right where it is observed to stall (stuck in MSG IN
             * after "Cmd complete MSG", watchdog timeout, bus reset).  Strip
             * a leading IDENTIFY byte before building the CDB; the PROM/
             * ufsboot/autoconfig-probe paths (real opcodes only, no ATN,
             * bit 7 clear) are unaffected.
             */
            if (n > 1 && (s->sun_fifo[0] & 0x80)) {
                base = 1;
                n--;
            }
            for (i = 0; i < n && i < (int)sizeof(s->cmd); i++) {
                s->cmd[i] = s->sun_fifo[base + i];
            }
            /*
             * The CDB length is defined by the opcode (cmd[0]), not by how many
             * bytes the driver happened to write to reg0.  The PROM/ufsboot
             * "si" path stages exactly the CDB (fifo count == CDB length), but
             * the kernel sd driver stages a padded 16-byte command buffer, so
             * using the raw fifo count would hand a 16-byte "TUR" (opcode 0x00)
             * to the SCSI layer and corrupt the transfer.  Use the SCSI-defined
             * length, capped by what was actually staged.
             */
            clen = scsi_cdb_length(s->cmd);
            s->cmd_len = (clen > 0 && clen <= n) ? clen : n;
            s->sun_fifo_count = 0;
            ncr5380_do_command(s);
        }
    } else {
        /*
         * Selection timeout: no BSY appears; the bus stays released.  Under
         * the Sun kernel's interrupt-driven regime (ATN launch, IDENTIFY
         * staged ahead of the CDB in sun_fifo -- same discriminator used
         * above to strip that byte from the CDB) there is no software
         * polling loop waiting on this -- the driver's tsleep()/biowait()
         * needs a real completion signal, same as the present-target case
         * gets from BSR_IRQ below.  Real si hardware raises exactly that: the
         * on-board select sequencer always finishes and interrupts, success
         * or failure.  Without it, an absent-target select under this regime
         * never wakes the sleeper; sun_sel_failed
         * tells sun3x_sireg_read() to present SI_CSR_SBC_IP|SI_CSR_FIFO_EMPTY
         * for the one si_csr read that services this interrupt, driving the
         * kernel "sm" driver's own existing (and otherwise dead) no-data/
         * err-0x12 completion path.
         *
         * Gated strictly on the IDENTIFY-staged launch, NOT on sun_mode
         * alone: the plain polled autoconfig-probe scan (PROM/ufsboot too)
         * also calls this function, dozens of times per boot, for every
         * absent id it walks, with no ATN/IDENTIFY staged and nobody waiting
         * on an interrupt at all.  Raising BSR_IRQ unconditionally there
         * re-triggers the exact unbounded "iobus level 2 not serviced" storm
         * ncr5380_sun_reap_stale_irq() exists to prevent (confirmed: an
         * unconditional version of this fix produced a multi-million-line
         * storm within seconds, this time starving hardclock badly enough
         * that the *unrelated* level-7/level5_active clock gate never
         * latched either).
         */
        bool sun_atn_launch = s->sun_mode && s->sun_fifo_count > 1 &&
                              (s->sun_fifo[0] & 0x80);

        s->csb = 0;
        s->sun_sel_failed = sun_atn_launch;
        if (sun_atn_launch) {
            s->bsr |= BSR_IRQ;
            ncr5380_update_irq(s);
        }
        trace_ncr5380_select_timeout();
    }
}

/* select a target: ODR holds the initiator+target ID bitmask */
static void ncr5380_select(NCR5380State *s)
{
    int id;

    /* target id = the non-initiator (bit 7 = host) bit set in ODR */
    for (id = 0; id < 7; id++) {
        if ((s->odr & 0x7f) & (1 << id)) {
            if (scsi_device_find(&s->bus, 0, id, 0)) {
                break;
            }
        }
    }
    ncr5380_select_target(s, id);
}

/*
 * Sun 3/80 si read-map quirk: the PROM driver derives the live SCSI bus phase
 * from the low 3 bits of reg2 (0x66000010), and the bus-and-status (with
 * PHASE_MATCH) from reg3 (0x66000014), rather than from CSB/BSR the way a
 * stock 5380 driver does.  reg2 uses the *standard* SCSI phase encoding
 * (bit0 = I/O, bit1 = C/D, bit2 = MSG):
 *
 *   1 = DATA IN, 0 = DATA OUT  -> the driver's state machine ARMS the si DMA
 *                                 (0xfeff0e04: writes dma_addr + si_csr|=SBC_IP)
 *   3 = STATUS-encoding, reused by the si driver as "data transfer complete"
 *                              -> it finalises the DMA (0xfeff0e6e), reading the
 *                                 auto-incremented dma_addr as the byte count
 *   7 = MESSAGE IN, reused for STATUS -> it reads the status/message bytes
 *   2 = COMMAND -> dispatcher error (never presented: we run the CDB at select)
 *
 * So a data-in transfer must be presented first as DATA (reg2==1) so the arm
 * fires, then — once the si DMA engine has drained it (sun_dma_done) — as the
 * "complete" code (reg2==3) so the finalise reads a non-zero byte count.
 * Returning 3 for the data phase up-front (the old behaviour) made the driver
 * jump straight to finalise without ever arming DMA, so dma_addr never
 * advanced and the returned count was 0 — the PROM then skipped the disk READ.
 */
static uint8_t sun_phase_bits(NCR5380State *s)
{
    if (!s->dev) {
        /*
         * After a blind (ufsboot / sd-open) command has completed and torn
         * the bus down to bus-free, present MESSAGE IN (7) for the fixed
         * number of trailing state-machine calls the transfer wrapper makes,
         * so it re-takes the benign success branch rather than the phase-0
         * retry path (err5 -> -1 -> "bdevvp: bad open").  The PROM probe's
         * ACK-handshake teardown does not set sun_cmd_complete, so its
         * trailing calls still see phase 0 (unchanged, benign).
         */
        return s->sun_cmd_complete ? 7 : 0;
    }
    switch (s->phase) {
    case PHASE_DI:
        return s->sun_dma_done ? 3 : 1;
    case PHASE_DO:
        return s->sun_dma_done ? 3 : 0;
    case PHASE_ST:
    case PHASE_MI:
        return 7;
    case PHASE_CMD:
        return 2;
    default:
        return 0;
    }
}

/*
 * Sun si: the driver, having finalised the data phase (read back the
 * auto-incremented dma_addr as the byte count), clears the si DMA latches;
 * the board then advances the bus to STATUS.  Model that here.
 *
 * (A second, earlier "data phase drained" interrupt raised from here is
 * not what the driver's "Data req" state 0x12 needs: its state machine is
 * driven by direct busy-poll calls into _sm_intr, not by asynchronous
 * traps, so a pulse timed against a register write lands in the wrong
 * call and changes nothing.  The fix for state 0x12 is the /REQ
 * re-assertion in ncr5380_sun_dma_drained().)
 */
void ncr5380_sun_to_status(NCR5380State *s)
{
    if (s->dev && (s->phase == PHASE_DI || s->phase == PHASE_DO)) {
        s->sun_dma_done = false;
        ncr5380_enter_status(s);
        /*
         * ncr5380_enter_status()'s own REQ guard ("assert REQ only if the
         * initiator is not currently holding ACK") is correct for the
         * byte-by-byte (PROM/ufsboot) handshake, where ICR_ASSERT_ACK
         * genuinely tracks an outstanding per-byte ACK the initiator is
         * mid-handshake on and ncr5380_ack_release() will assert REQ for
         * it once released.  It is NOT correct here: live-traced (gdb +
         * `-trace ncr5380_read,write` against the real /vmunix, the first
         * READ(6) after the "Lost Busy" fix),
         * the kernel "sm" driver's own DMA-arm-strobe pulse (ICR=0x90,
         * bit7 arm-strobe + bit4 ASSERT_ACK) sets ICR_ASSERT_ACK once,
         * before the transfer even runs, and never clears it before this
         * finalise -- unlike a real per-byte ACK, that bit does not track
         * any actually-outstanding SCSI ACK once DMA is under way (the
         * real 5380's DMA logic pulses ACK internally, decoupled from the
         * CPU-visible ICR bit, which the driver only uses here for its own
         * DMA bookkeeping).  With the guard applied, REQ was left down
         * permanently: nothing else was ever going to assert it (no
         * ACK-release edge was coming, since the bit was never cleared),
         * so the driver's reg3 poll fell back to the state/BSR dispatcher,
         * found no live REQ, exhausted its retry budget and declared
         * "Unexpected phase error" (state 0x11, "Comp req") -- a new,
         * non-recovering wall one dispatch state past the original "Lost
         * Busy" bug.  Real hardware genuinely asserts /REQ for the new
         * (STATUS) phase the instant the DMA engine drains, regardless of
         * this stale bit; assert it unconditionally here to match.  Safe
         * for the PROM/ufsboot path too: it shares this exact call site
         * (sun3x.c's si_csr latch-clear), and either its own ICR_ASSERT_ACK
         * is already clear at this point (making this a no-op -- REQ was
         * going to be set anyway) or its own dispatch does not key off
         * CSB_REQ at that specific poll the way the kernel's reg3/BSR
         * dispatch does (sun_phase_bits() answers from s->phase alone).
         */
        s->csb |= CSB_REQ;
    }
}

/*
 * Sun si: the DMA engine has just drained a data phase (sun3x's
 * sun3x_si_maybe_run_dma(), right after latching sun_dma_done).  See the
 * sun_dma_drained_pending comment in ncr5380.h for why a one-shot latch,
 * rather than re-asserting CSB_REQ itself, is what survives the driver's
 * own immediately-following DMA-arm-strobe ICR pulse.
 */
void ncr5380_sun_dma_drained(NCR5380State *s)
{
    if (s->dev && (s->phase == PHASE_DI || s->phase == PHASE_DO)) {
        s->sun_dma_drained_pending = true;
    }
}

/*
 * Sun si: BSR_IRQ (the raw NCR5380 /IRQ this model forwards 1:1 to the OBIO
 * level-2 line, see sun3x_si_update_irq()) is a genuine hardware interrupt
 * cause -- selection-complete, bus-free, parity/busy error -- and on real
 * silicon it latches until the initiator reads reg7 (RPI).  The SunOS kernel
 * "sm" driver never reads reg7: its claim test is the si board's own CSR
 * (si_csr & 3, synthesised here from `dev != NULL` for kernel-mode reads, see
 * sun3x_sireg_read()), not the 5380's BSR bit.  Several BSR_IRQ causes fire
 * and clear `dev` in the very same synchronous step (selection races ahead of
 * the driver's own housekeeping; bus-free always does), so by the time the
 * CPU actually takes the trap -- which, during autoconfig, can be many
 * hundreds of milliseconds later, since the kernel keeps SR raised
 * (splbio()-style) across the whole polled probe scan -- the cause is
 * already stale and unclaimable.  Without a reg7 read to retire it, a stale
 * BSR_IRQ has no way to ever go away: every subsequent selection's
 * `bsr |= BSR_IRQ` is a no-op once the bit is already 1, so the physical line
 * stays asserted permanently, and the first time SR finally drops (leaving
 * autoconfig, tsleep()/biowait()) the CPU retakes an unclaimable level-2 trap
 * forever -- an unbounded "iobus level 2 interrupt not serviced" storm that
 * starves even the higher-priority level-5 hardclock of enough real time to
 * ever reach its own watchdog.  Reap it here, at the exact moment the kernel
 * claim test (si_csr & 3) has just found nothing to claim: this is the model
 * standing in for the reg7 read the real driver never issues, so a stale
 * cause is retired instead of re-presented every single instruction the CPU
 * is unmasked.  A live, still-valid cause is unaffected: it is re-asserted by
 * whatever raised it (select_target()'s own `bsr |= BSR_IRQ`) on its own very
 * next event, and the claim test then correctly reads 1 (dev non-NULL) and
 * this function is not reached.
 */
void ncr5380_sun_reap_stale_irq(NCR5380State *s)
{
    if (s->bsr & BSR_IRQ) {
        s->bsr &= ~BSR_IRQ;
        ncr5380_update_irq(s);
    }
}

/*
 * Sun si: a genuine SCSI bus reset assertion (si_csr bit 0x80 -- see the
 * comment above the SI_CSR_SUN_RESET call site in sun3x.c for how that bit,
 * not the register layout's own SI_CSR_SCSI_RES/FIFO_RES names, was found to
 * be the real reset signal).  A sustained-write-integrity repro
 * live-traced every "resetting scsi bus"
 * event across three regimes (PROM self-test, ufsboot, and the kernel "sm"
 * driver's own sm_reset()) and found `s->dev`/`s->req` already NULL at
 * every single one: this model completes each SCSI command synchronously
 * (ncr5380_pump() drains the whole request before returning control to the
 * guest), so a "the driver timed out and is now resetting" event is always,
 * in practice, a *notification* race (the command already finished; the
 * guest just did not learn about it fast enough) rather than a genuinely
 * still-in-flight transfer -- confirmed live for both the READ and the
 * WRITE case, with zero placement/content corruption found across three
 * separate multi-timeout repro runs (including cross-target interleaving).
 *
 * The one path that CAN leave `s->req` genuinely open at this point is a
 * DVMA translation failure partway through a data-out (WRITE) transfer
 * (sun3x_si_dma_run() breaking out of its byte-copy loop on
 * SI_CSR_DMA_BUS_ERR before the collected buffer ever reaches
 * ncr5380_pdma_write()'s buf_pos==buf_len flush): the SCSI-layer request
 * exists, mid-collection, with no data yet handed to the block backend. A
 * real hardware bus reset must abort that cleanly rather than let it later
 * complete (or be resumed) with partial/stale data, so cancel it via the
 * bus's own cancel path -- scsi_req_cancel(), not a bare unref -- which
 * synchronously invokes ncr5380_request_cancelled() (this model's
 * `.cancel` bus callback) before returning, guaranteeing `s->req` is NULL
 * again by the time ncr5380_reset_bus() runs. ncr5380_reset_bus() then
 * clears the rest of the bus/transaction state (phase, buf_pos/buf_len/
 * out_pos, the sun_* one-shot completion latches, the staged CDB FIFO) the
 * same way the pre-existing non-sun ICR-RST path already does, so a stale
 * buffer position or one-shot latch from the aborted command can never
 * bleed into whatever the driver selects next.
 */
void ncr5380_sun_bus_reset(NCR5380State *s)
{
    if (s->req) {
        scsi_req_cancel(s->req);
    }
    ncr5380_reset_bus(s);
    ncr5380_update_irq(s);
}

static uint64_t ncr5380_read(void *opaque, hwaddr addr, unsigned size)
{
    NCR5380State *s = opaque;
    int reg = (addr >> s->reg_shift) & 7;
    uint64_t val = 0;

    switch (reg) {
    case R_CSD:
        /*
         * Current data byte.  During a data-in phase the Mac driver can
         * "blind" read: it reads CSD repeatedly without a per-byte ACK,
         * relying on the hardware to advance.  Deliver the current byte
         * and step to the next so successive reads drain the buffer.
         */
        if (s->sun_mode && !s->dev && s->sun_cmd_complete) {
            /*
             * Trailing state-machine calls after a blind completion: the
             * benign phase-7 completion handler (0x213c24) reads reg0 as the
             * final message byte and dispatches on it.  Present COMMAND
             * COMPLETE (0x00) so it settles into the done state (32) with no
             * error, rather than a stale CDB/data byte that trips err4.
             * Record that the trailing message has been taken so reg3 flips
             * from 0x08 (phase dispatch) to 0x20 (BSR dispatch) and the next
             * call completes the state machine to its "done" state 0.
             */
            val = 0x00;
            s->sun_msg_taken = true;
        } else if (s->phase == PHASE_DI && s->sun_mode &&
                   (unsigned)ncr5380_guest_pc() >= 0xf8000000u &&
                   (unsigned)ncr5380_guest_pc() < 0xf9000000u) {
            /*
             * Kernel-regime sun-mode I/O never uses this register (R_CSD,
             * reg0) for real per-byte data movement -- bulk transfers go
             * exclusively through the SI DMA aperture (ncr5380_pdma_read(),
             * driven by sun3x_si_dma_run()).  The only kernel-text reads of
             * R_CSD seen during a live DATA IN phase are the driver's own
             * post-timeout diagnostic "Snap shot" dump (sm_deque: lost
             * interrupt / spurious interrupt / I/O request timeout): it
             * blindly reads reg0 sixteen times back to back to sample the
             * hardware FIFO for its crash report's "fifo: ..." line, with
             * no intention of consuming the pending transfer's data at
             * all.  Treating that the same as the PROM/ufsboot byte-by-
             * byte blind reader (which genuinely does want to consume and
             * advance, and remains untouched below since it never runs
             * from this PC range) silently eats real bytes off the FRONT
             * of the still-pending sun-mode DMA buffer; when the driver
             * later manages to actually arm the DMA for that same buffer
             * (either after a self-healing timeout, or on the very next
             * command to reuse this dbuf), the transfer comes up short by
             * exactly the
             * number of diagnostic peeks that ran first, desyncing the
             * driver's own byte-count bookkeeping into a "spurious
             * interrupt" and delivering a truncated/corrupt read to the
             * application -- evidenced live: a `zcat | tar` reading the
             * sets disk
             * during exactly such an event failed with "uncompress:
             * corrupt input" / "tar: read error: unexpected EOF",
             * root-caused via register tracing to precisely 16 consuming
             * R_CSD reads at PC 0xf8072236 (this diagnostic dump) landing
             * on a target's still-pending DATA IN buffer, immediately
             * followed by "sm0: spurious interrupt" reporting the
             * transfer 16 bytes short.  Present the byte without
             * advancing buf_pos so the pending transfer this buffer
             * belongs to is delivered intact once genuinely armed.
             */
            val = (s->buf_pos < s->buf_len) ? s->dbuf[s->buf_pos]
                                             : s->last_data;
        } else if (s->phase == PHASE_DI) {
            if (s->buf_pos < s->buf_len) {
                val = s->dbuf[s->buf_pos++];
                s->last_data = val;
                /*
                 * Buffer exhausted?  The DI->ST transition happens on
                 * the ACK release that follows this read (the ROM's
                 * per-byte handshake), see ncr5380_ack_release.
                 */
            } else {
                trace_ncr5380_stale_read(s->buf_pos, s->buf_len);
                val = s->last_data;
            }
        } else if (s->sun_mode && !(s->icr & ICR_ASSERT_ACK)
                   && (s->phase == PHASE_ST || s->phase == PHASE_MI)) {
            /*
             * Sun si blind STATUS/MESSAGE read.  ufsboot's polled driver reads
             * reg0 in the status/message phase *without* asserting ACK (unlike
             * the PROM probe, which does the reg1/ICR ACK handshake), and
             * spins `while (reg5 != 0) read reg0` waiting for the bus to go
             * idle (0x213aba).  With reg5-IRQ modelled as the live /REQ, that
             * spin can only end if reading the byte here also advances the
             * handshake -- take the byte, drop /REQ, and step the phase:
             * STATUS -> MESSAGE IN, MESSAGE IN -> bus free.  Gated on ACK being
             * de-asserted so the PROM's explicit-handshake path is untouched.
             */
            val = s->last_data;
            s->csb &= ~CSB_REQ;
            if (s->phase == PHASE_ST) {
                ncr5380_set_phase(s, PHASE_MI);
                s->last_data = 0x00;            /* COMMAND COMPLETE message */
                s->csb |= CSB_REQ;
            } else {
                /* MESSAGE IN consumed: bus free, end of transaction */
                s->csb = 0;
                s->phase = PHASE_DO;
                s->bsr |= BSR_IRQ;
                ncr5380_update_irq(s);
                if (s->req) {
                    scsi_req_unref(s->req);
                    s->req = NULL;
                }
                s->dev = NULL;
                /*
                 * Trailing state-machine calls after this blind completion
                 * must see MESSAGE IN (7), not phase 0, or the phase-0 retry
                 * path exhausts (err5) and the wrapper returns -1.
                 */
                s->sun_cmd_complete = true;
            }
        } else if (s->phase & CSB_IO) {
            val = s->last_data;
        } else {
            val = s->odr;
        }
        break;
    case R_ICR_R:
        /* bits 5 (lost-arb) and 6 (arb-in-progress) are read-only status */
        val = s->icr;
        break;
    case R_MR_R:
        val = s->sun_mode ? sun_phase_bits(s) : s->mr;
        break;
    case R_TCR_R:
        if (s->sun_mode) {
            /*
             * reg3 read drives the driver's dispatcher (values are decoded
             * against {0x80,0x50,0x40,0x20,0x18,0x10,0x08,...}): 0x20 routes
             * into the STATE dispatcher (advance/complete, incl. state 32 ->
             * done) while 0x08 routes into the phase handler that services an
             * active data/status phase.  Present 0x08 while the target has a
             * live REQ to service, else 0x20 so the state machine advances.
             */
            if (s->dev && (s->csb & CSB_REQ)) {
                val = 0x08;
            } else if (s->dev && s->sun_dma_drained_pending) {
                /*
                 * The si DMA engine drained a data phase since the driver's
                 * last poll: real hardware's /REQ for the next (STATUS)
                 * phase is already up, but this model's CSB_REQ was clobbered
                 * by the driver's own intervening DMA-arm-strobe ICR pulse
                 * (see the sun_dma_drained_pending comment in ncr5380.h).
                 * Present the phase-pending encoding for exactly this one
                 * read so the driver's dispatch does not mis-route into the
                 * state/BSR dispatcher and declare a spurious "Lost Busy".
                 */
                val = 0x08;
                s->sun_dma_drained_pending = false;
            } else if (!s->dev && s->sun_cmd_complete && !s->sun_msg_taken) {
                /*
                 * Trailing call after a blind completion, before the final
                 * MESSAGE byte has been taken (state 27): present 0x08 so the
                 * phase-match dispatcher (0x213d36) routes to the *phase*
                 * handler (0x213d00), which with reg2==7 reads the message
                 * (reg0) and advances state 27 -> 32.  Once the message is
                 * taken, fall through to 0x20 so the *BSR* dispatcher
                 * (0x213b50) completes state 32 -> 0 (done).
                 */
                val = 0x08;
            } else {
                val = 0x20;
            }
        } else {
            val = s->tcr;
        }
        break;
    case R_CSB:
        val = s->csb;
        break;
    case R_BSR:
        {
            /*
             * Phase match: the phase programmed into the TCR (bit0 I/O,
             * bit1 C/D, bit2 MSG) equals the actual bus phase.
             */
            uint8_t want = ((s->tcr & 0x01) ? CSB_IO : 0)
                         | ((s->tcr & 0x02) ? CSB_CD : 0)
                         | ((s->tcr & 0x04) ? CSB_MSG : 0);

            val = s->bsr & ~BSR_PHASE_MATCH;
            if (s->dev && want == s->phase) {
                val |= BSR_PHASE_MATCH;
            }
            /*
             * Sun si: model reg5 (BSR) bit4 (IRQ) as a *live per-/REQ signal*,
             * not a latch.  The polled boot driver reads reg5 two opposite
             * ways with no reg7/RPI clear in between: at the start of a
             * STATUS/MESSAGE byte it waits for `(reg5 & 0x1f) != 0` = "a bus
             * event/byte is pending" (PROM 0xfeff12da; masking IRQ there hangs
             * the probe), and after servicing it waits for `reg5 == 0` = "bus
             * idle" before completing (ufsboot 0x213aba).  Both are satisfied
             * if IRQ tracks the live target /REQ: asserted while a phase byte
             * is waiting to be transferred, de-asserted once the initiator has
             * taken it (CSB_REQ drops).  So drive bit4 from CSB_REQ rather than
             * the stored latch.
             */
            if (s->sun_mode) {
                val &= ~BSR_IRQ;
                if (s->csb & CSB_REQ) {
                    val |= BSR_IRQ;
                }
            }
        }
        break;
    case R_IDR:
        val = s->last_data;
        break;
    case R_RPI:
        /* reading resets parity/interrupt */
        s->bsr &= ~(BSR_IRQ | BSR_PAR_ERR | BSR_BUSY_ERR);
        ncr5380_update_irq(s);
        val = 0;
        break;
    }

    trace_ncr5380_read(reg, (uint8_t)val, ncr5380_guest_pc());
    return val;
}

static void ncr5380_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    NCR5380State *s = opaque;
    int reg = (addr >> s->reg_shift) & 7;

    trace_ncr5380_write(reg, (uint8_t)val, ncr5380_guest_pc());

    switch (reg) {
    case R_ODR:
        s->odr = val;
        /*
         * Sun si: the driver stages the CDB by writing all its bytes to the
         * data register (reg0) before it arms selection.  Buffer them; they
         * are delivered to the target as the command once selection lands.
         */
        if (s->sun_mode && !s->dev
            && s->sun_fifo_count < sizeof(s->sun_fifo)) {
            s->sun_fifo[s->sun_fifo_count++] = val;
        }
        /* command / message / data-out byte handed over on ACK */
        break;
    case R_ICR:
        {
            uint8_t old = s->icr;

            /* keep read-only status bits 5 (LA) and 6 (AIP) */
            s->icr = (val & 0x9f) | (s->icr & 0x60);
            /*
             * In sun-mode ICR bit7 is a DMA-arming strobe (the driver writes
             * 0x80/0x90 while setting up a transfer), not a SCSI bus reset.
             */
            if ((val & ICR_ASSERT_RST) && !s->sun_mode) {
                ncr5380_reset_bus(s);
                s->icr = 0;
                break;
            }
            /*
             * Selection completes on the transition where SEL and the
             * data drivers are asserted while BSY is released: the target
             * ID is then on the data bus (ODR, own-id bit 7 excluded).
             */
            if ((val & ICR_ASSERT_SEL) && (val & ICR_ASSERT_DATA)
                && !(val & ICR_ASSERT_BSY) && (old & ICR_ASSERT_BSY)
                && !s->dev) {
                ncr5380_select(s);
            }
            /*
             * REQ/ACK byte handshake for the non-DMA phases: the target
             * asserts REQ, the initiator transfers a byte and asserts
             * ACK (REQ drops); when ACK is released REQ re-asserts for
             * the next byte.  The driver paces on REQ (CSB bit 5).
             */
            if ((val & ICR_ASSERT_ACK) && !(old & ICR_ASSERT_ACK)) {
                ncr5380_ack(s);
            }
            if (!(val & ICR_ASSERT_ACK) && (old & ICR_ASSERT_ACK)) {
                ncr5380_ack_release(s);
            }
        }
        break;
    case R_MR:
        s->mr = val;
        if (val & MR_ARBITRATE) {
            /* arbitration always wins on this single-initiator bus */
            s->icr &= ~ICR_ARB_LOST;
            s->icr |= ICR_ARB_IN_PROG;
        } else {
            s->icr &= ~(ICR_ARB_IN_PROG | ICR_ARB_LOST);
        }
        /*
         * Sun 3/80 "si" selection: the driver stages the CDB into reg0, then
         * writes reg2 (MR) = the *target id* (low 3 bits) followed by ICR to
         * launch selection.  The "arbitrate" bit (0x01) is NOT a mode flag
         * here — it is just bit 0 of the target id, so it is set only for ODD
         * targets.  The PROM/ufsboot boot target 3 (odd), so the old
         * "trigger on MR_ARBITRATE" heuristic happened to work; but the SunOS
         * kernel autoconfig probes ALL ids, and its first probe (target 6,
         * MR=0x06, bit0=0) never fired selection -> dev stayed NULL -> the
         * probe state machine looped on CSB=0 forever.  Trigger selection
         * whenever a CDB has been staged (sun_fifo_count > 0) and no device is
         * yet selected, using target = MR & 7 regardless of bit 0.  (Non-
         * selection MR writes during a transfer have dev set / the FIFO
         * drained, so they never re-trigger.)
         */
        if (s->sun_mode && !s->dev && s->sun_fifo_count > 0) {
            s->icr &= ~ICR_ARB_LOST;
            s->icr |= ICR_ARB_IN_PROG;
            /*
             * New selection: a fresh command, no trailing-completion yet.
             * A still-pending sun_cmd_irq from the previous command is
             * deliberately cleared here: preserving it (and accumulating
             * the read credit in the PHASE_MI branch) traded a rare,
             * self-healing timeout for a non-recovering hang under heavier
             * autoconfig load with two disks attached.
             */
            s->sun_cmd_complete = false;
            s->sun_dma_drained_pending = false;
            s->sun_msg_taken = false;
            s->sun_sel_failed = false;
            s->sun_cmd_irq = false;
            s->sun_cmd_irq_reads = 0;
            ncr5380_select_target(s, val & 0x07);
        }
        break;
    case R_TCR:
        s->tcr = val;
        break;
    case R_SER:
        s->ser = val;
        break;
    case R_DMASND:
    case R_DMATR:
    case R_DMAIR:
        /* pseudo-DMA is driven through the PDMA aperture; arm DRQ */
        s->dma_mode = reg;
        s->bsr |= BSR_DRQ;
        break;
    }
}

/* initiator asserted ACK: accept/deliver the current byte and drop REQ */
static void ncr5380_ack(NCR5380State *s)
{
    s->csb &= ~CSB_REQ;

    switch (s->phase) {
    case PHASE_CMD:
        if (s->cmd_len < (int)sizeof(s->cmd)) {
            s->cmd[s->cmd_len++] = s->odr;
        }
        break;
    case PHASE_DI:
        /* the byte was already consumed and advanced by the CSD read */
        break;
    case PHASE_DO:
        if (s->buf_pos < s->buf_len) {
            s->dbuf[s->buf_pos++] = s->odr;
        }
        break;
    case PHASE_ST:
        s->status_done = true;
        /*
         * Sun kernel "sm" status handshake: unlike the PROM/ufsboot (which
         * release ACK, so the STATUS->MESSAGE advance happens in
         * ncr5380_ack_release), the SunOS kernel driver ACKs the status byte
         * (ICR=0x10 at 0xf8073f84) and HOLDS ACK, then re-runs its state
         * machine expecting the target already in MESSAGE IN.  Advance the bus
         * here on the ACK assert so the held-ACK poll sees it.  Gated to kernel
         * text (0xf8000000..0xf9000000) so the PROM/ufsboot release-based
         * handshake is byte-for-byte unchanged.
         */
        if (s->sun_mode) {
            unsigned pc = (unsigned)ncr5380_guest_pc();
            if (pc >= 0xf8000000u && pc < 0xf9000000u) {
                ncr5380_set_phase(s, PHASE_MI);
                s->last_data = 0x00;            /* COMMAND COMPLETE message */
                s->csb |= CSB_REQ;
            }
        }
        break;
    case PHASE_MI:
        s->msg_done = true;
        /* kernel: the message byte is ACKed -> target releases the bus */
        if (s->sun_mode) {
            unsigned pc = (unsigned)ncr5380_guest_pc();
            if (pc >= 0xf8000000u && pc < 0xf9000000u) {
                s->csb = 0;
                s->phase = PHASE_DO;
                if (s->req) {
                    scsi_req_unref(s->req);
                    s->req = NULL;
                }
                s->dev = NULL;
                s->sun_cmd_complete = true;
                /*
                 * Real si hardware interrupts here, success or failure alike
                 * (see ncr5380_select_target()'s sun_sel_failed for the
                 * failure twin of this fix).  The PROM/ufsboot blind-read
                 * completion (the CSD-read branch above) already raises
                 * BSR_IRQ because that regime polls reg0/reg5 directly and
                 * needs the live /REQ semantics; the busy-poll used
                 * throughout kernel autoconfig does not need a real trap
                 * either, since it calls _sm_intr directly in a bounded
                 * loop.  But the kernel's *interrupt-driven* real I/O path
                 * (post-autoconfig, sleeping in tsleep()/biowait()) has
                 * nothing else to invoke _sm_intr with.  Without this,
                 * the transaction
                 * genuinely finishes at the register level (dev goes NULL,
                 * the kernel's own busy/bit0 poll would read 0) but nothing
                 * ever tells the sleeping process, so it just sits until its
                 * own watchdog re-kicks the request from scratch.
                 */
                s->bsr |= BSR_IRQ;
                s->sun_cmd_irq = true;
                /*
                 * 4, not 3: with no PC gate (see the sun_cmd_irq_reads
                 * comment in ncr5380.h) the leading "state-machine inner
                 * loop" read is counted too, harmlessly, since that read
                 * never tests bit1.
                 */
                s->sun_cmd_irq_reads = 4;
                ncr5380_update_irq(s);
            }
        }
        break;
    default:
        break;
    }
}

/* initiator released ACK: advance the phase and re-assert REQ as needed */
static void ncr5380_ack_release(NCR5380State *s)
{
    switch (s->phase) {
    case PHASE_CMD:
        if (s->cmd_len >= scsi_cdb_length(s->cmd) && s->cmd_len > 0) {
            ncr5380_do_command(s);      /* → DI/DO, or completes → ST */
            /* fall through to arm REQ for whatever phase we are now in */
        } else {
            s->csb |= CSB_REQ;          /* next command byte */
            break;
        }
        /* fallthrough */
    case PHASE_DI:
        if (s->phase == PHASE_DI) {
            if (s->buf_pos < s->buf_len) {
                s->last_data = s->dbuf[s->buf_pos];
                s->csb |= CSB_REQ;
            } else {
                /*
                 * Whole transfer consumed: move to STATUS right here,
                 * synchronously, so the initiator's very next phase
                 * poll sees the change (no phantom-data window).
                 */
                ncr5380_enter_status(s);
            }
            break;
        }
        if (s->phase == PHASE_ST) {
            s->csb |= CSB_REQ;          /* status byte available */
        }
        break;
    case PHASE_DO:
        if (s->buf_pos < s->buf_len) {
            s->csb |= CSB_REQ;
        } else {
            /*
             * Last byte received via the byte-by-byte ACK'd handshake
             * (not the sun-mode si DMA aperture, see ncr5380_pdma_write()
             * below): always advance to STATUS immediately here -- this
             * is the Mac ROM's blind writer contract, unaffected by the
             * sun-mode DMA deferral.
             */
            ncr5380_flush_data_out(s, true);
        }
        break;
    case PHASE_ST:
        if (s->status_done) {
            ncr5380_set_phase(s, PHASE_MI);
            s->last_data = 0x00;        /* COMMAND COMPLETE message */
            s->csb |= CSB_REQ;
        } else {
            s->csb |= CSB_REQ;
        }
        break;
    case PHASE_MI:
        if (s->msg_done) {
            /*
             * Bus free: end of transaction.  The target releases ALL
             * bus lines, phase lines included: CSB must read 0.  Stale
             * phase bits (0x1c) latched here make the Mac driver's
             * wait-for-bus-free poll spin into its timeout and fail an
             * otherwise perfect transfer.
             */
            s->csb = 0;
            s->phase = PHASE_DO;
            s->bsr |= BSR_IRQ;
            ncr5380_update_irq(s);
            if (s->req) {
                scsi_req_unref(s->req);
                s->req = NULL;
            }
            s->dev = NULL;
        } else {
            s->csb |= CSB_REQ;
        }
        break;
    default:
        break;
    }
}

/*
 * Is a pseudo-DMA byte ready to move?  The Mac's handshake aperture
 * bus-errors when DRQ does not arrive in time; the machine uses this
 * to fault accesses outside an active data phase.
 */
bool ncr5380_pdma_ready(NCR5380State *s, bool out)
{
    uint8_t want = out ? PHASE_DO : PHASE_DI;

    return s->phase == want && s->buf_pos < s->buf_len;
}

/* pseudo-DMA data movement: one byte per aperture access */
uint8_t ncr5380_pdma_read(NCR5380State *s)
{
    uint8_t v = 0;

    if (s->phase == PHASE_DI && s->buf_pos < s->buf_len) {
        v = s->dbuf[s->buf_pos++];
        s->last_data = v;
        if (s->buf_pos >= s->buf_len) {
            /*
             * Transfer complete.  Pseudo-DMA has no per-byte ACK, so
             * flip to STATUS right now: the blind loop's byte counter
             * runs out on this access and the ROM's trailing phase
             * poll must already see the change.
             *
             * Sun si: do NOT advance to STATUS here.  The si driver first
             * finalises the data phase (reads back the auto-incremented
             * dma_addr as the byte count) while the bus still reads DATA
             * (reg2==3, sun_dma_done); the board only moves to STATUS once
             * the driver clears the si DMA latches — ncr5380_sun_to_status,
             * driven from sun3x.c, does that.
             */
            if (!s->sun_mode) {
                ncr5380_enter_status(s);
            }
        } else {
            /*
             * Model the per-byte SCSI /REQ handshake: the target
             * deasserts /REQ once the initiator's pseudo-DMA read
             * (DACK) has taken this byte, and reasserts it for the
             * next one.  A blind reader that drains the whole transfer
             * in a single loop only ever samples /REQ *after* the last
             * byte (by which point enter_status above has moved the bus
             * to STATUS), so this is invisible to it -- e.g. the IIci/
             * IIsi ROMs are unaffected.  But a *chunked* reader hangs
             * without it: the Mac II / SE30 ROM SCSI Manager drains a
             * multi-sector transfer 512 bytes at a time and, between
             * chunks, spins on CSB bit 5 waiting for /REQ to drop
             * before fetching the next chunk (observed live: a
             * 192-sector READ(10) that stalled after exactly one 512-
             * byte chunk).  The chunk's byte-move loop gates on DRQ,
             * which stays asserted until the buffer fully drains, so
             * clearing /REQ here does not disturb the copy loop; it
             * only lets the inter-chunk /REQ poll make progress.
             */
            s->csb &= ~CSB_REQ;
        }
    } else {
        trace_ncr5380_stale_read(s->buf_pos, s->buf_len);
        v = s->last_data;
    }
    trace_ncr5380_pdma_rd(v);
    return v;
}

void ncr5380_pdma_write(NCR5380State *s, uint8_t val)
{
    trace_ncr5380_pdma_wr(val);
    if (s->phase == PHASE_CMD) {
        if (s->cmd_len < (int)sizeof(s->cmd)) {
            s->cmd[s->cmd_len++] = val;
        }
        if (s->cmd_len >= scsi_cdb_length(s->cmd) && s->cmd_len > 0) {
            ncr5380_do_command(s);
        }
        return;
    }
    if (s->phase != PHASE_DO) {
        return;
    }
    if (s->buf_pos < s->buf_len) {
        s->dbuf[s->buf_pos++] = val;
    }
    if (s->buf_pos >= s->buf_len) {
        /*
         * Whole transfer collected: deliver it to the device now.  Sun
         * si DMA (sun_mode): defer the STATUS transition to
         * ncr5380_sun_to_status(), driven from sun3x.c once the driver
         * finalises the DMA latches -- mirrors ncr5380_pdma_read()'s
         * equivalent deferral for DATA IN (see the sun_mode comment on
         * ncr5380_flush_data_out() above).  Non-sun boards keep the
         * immediate transition, matching the pre-existing behaviour.
         */
        ncr5380_flush_data_out(s, !s->sun_mode);
    }
}

static const MemoryRegionOps ncr5380_ops = {
    .read = ncr5380_read,
    .write = ncr5380_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

/* SCSI bus callbacks */

/*
 * SCSI-layer chunk handover.  Only ever called inside the synchronous
 * pump: for data-in, append the chunk to the whole-transfer buffer; for
 * data-out, feed the next chunk of the collected buffer to the device.
 * Then immediately re-continue so the request runs to completion.
 */
static void ncr5380_transfer_data(SCSIRequest *req, uint32_t len)
{
    NCR5380State *s = req->hba_private;
    uint8_t *chunk = scsi_req_get_buf(req);

    trace_ncr5380_transfer(len);
    if (!s->collecting) {
        /* shouldn't happen with the synchronous pump; drop the chunk */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ncr5380: unexpected transfer_data outside pump\n");
        return;
    }
    if (req->cmd.mode == SCSI_XFER_FROM_DEV) {
        ncr5380_grow_dbuf(s, s->buf_len + len);
        memcpy(s->dbuf + s->buf_len, chunk, len);
        s->buf_len += len;
    } else {
        if (s->out_pos + len > s->buf_len) {
            len = s->buf_len > s->out_pos ? s->buf_len - s->out_pos : 0;
        }
        memcpy(chunk, s->dbuf + s->out_pos, len);
        s->out_pos += len;
    }
    scsi_req_continue(req);
}

static void ncr5380_command_complete(SCSIRequest *req, size_t resid)
{
    NCR5380State *s = req->hba_private;

    trace_ncr5380_complete(req->status);
    s->status = req->status;
    s->cmd_done = true;
    if (s->collecting) {
        /* do_command / flush_data_out set up the next bus phase */
        return;
    }
    /* no-data command: completes during scsi_req_enqueue */
    s->buf_len = 0;
    s->buf_pos = 0;
    ncr5380_enter_status(s);
}

static void ncr5380_request_cancelled(SCSIRequest *req)
{
    NCR5380State *s = req->hba_private;

    if (req == s->req) {
        scsi_req_unref(s->req);
        s->req = NULL;
        s->dev = NULL;
        s->cmd_done = true;     /* unblock the pump */
    }
}

static const struct SCSIBusInfo ncr5380_scsi_info = {
    .tcq = false,
    .max_target = 7,
    .max_lun = 7,
    .transfer_data = ncr5380_transfer_data,
    .complete = ncr5380_command_complete,
    .cancel = ncr5380_request_cancelled,
};

static void ncr5380_reset_hold(Object *obj, ResetType type)
{
    NCR5380State *s = NCR5380(obj);

    s->icr = 0;
    s->mr = 0;
    s->tcr = 0;
    s->ser = 0;
    s->odr = 0;
    s->last_data = 0;
    s->dma_mode = 0;
    ncr5380_reset_bus(s);
    ncr5380_update_irq(s);
}

static void ncr5380_realize(DeviceState *dev, Error **errp)
{
    NCR5380State *s = NCR5380(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->mmio, OBJECT(s), &ncr5380_ops, s,
                          "ncr5380", 8 << s->reg_shift);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    scsi_bus_init(&s->bus, sizeof(s->bus), dev, &ncr5380_scsi_info);
}

static const Property ncr5380_properties[] = {
    DEFINE_PROP_UINT8("reg-shift", NCR5380State, reg_shift, 4),
    DEFINE_PROP_BOOL("sun-mode", NCR5380State, sun_mode, false),
};

static void ncr5380_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = ncr5380_realize;
    device_class_set_props(dc, ncr5380_properties);
    rc->phases.hold = ncr5380_reset_hold;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo ncr5380_info = {
    .name = TYPE_NCR5380,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NCR5380State),
    .class_init = ncr5380_class_init,
};

static void ncr5380_register_types(void)
{
    type_register_static(&ncr5380_info);
}

type_init(ncr5380_register_types)
