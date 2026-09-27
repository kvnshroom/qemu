# Sun 3/80 (`-M sun3x`) — hardware contract and condensed history

The QEMU `sun3x` machine (`hw/m68k/sun3x.c`) is a **Sun 3/80**: a Motorola
68030 with its on-chip PMMU (the same unit already exercised by HP9000/340 and
Apollo — *not* the discrete Sun-3 MMU of the sun3-60). With no `-bios` it is a
Linux shim that boots the mainline Linux/m68k `sun3x` kernel (`CONFIG_SUN3X`) to
a serial shell via a synthesised minimal PROM. With `-bios sun3_80_v3.0.3.bin`
it runs the **genuine Sun 3/80 v3.0.3 boot PROM** (self-test → `>` monitor) and
boots **SunOS 4.1.1**: the MINIROOT installer to a `#` shell, and an installed
GENERIC system to multi-user with `login:`, and — with `sunos-console=fb` — a
local bwtwo screen running SunView and OpenWindows 2.0 with keyboard and mouse.

The full session-by-session debugging journal (hw/m68k/sun3x-SUNOS-NOTES.md, 9457 lines) is preserved in branch `sun3-history`; this file keeps only the conclusions.

---

# PART 1 — THE HARDWARE CONTRACT

The distilled register/behaviour reference. Every numeric fact below was
cross-checked against the final code (`hw/m68k/sun3x.c`, `hw/scsi/ncr5380.c`,
`include/hw/scsi/ncr5380.h`, `hw/display/sun-bwtwo.c`); where the journal and the
code disagree, the code wins. Evidence pointers (ROM address / kernel symbol /
NetBSD source) are inline.

## CPU / MMU / boot flow

* **m68030** with the standard on-chip PMMU. `mc->default_cpu_type = m68030`,
  `max_cpus = 1`, default RAM 16 MiB (`SUN3X_RAM_DEFAULT`), max 64 MiB
  (`SUN3X_RAM_MAX`).
* SunOS vs Linux mode is chosen by `machine->firmware != NULL` (i.e. `-bios`).
  Linux mode installs a zero/absorb background over all of memory (no bus
  errors); SunOS mode deliberately omits it so unmapped accesses raise genuine
  68030 bus errors, which the PROM self-test relies on to size memory and probe
  devices.

### Linux-shim boot (no `-bios`)

* `PAGE_OFFSET_RAW == 0` → kernel VA == PA for RAM. The kernel is entered
  **MMU-off** at its ELF entry, running physically in RAM at phys 0; `head.S`
  builds `kernel_pg_dir` and `mmu_engage`s.
* Standard m68k bootinfo appended after the image (`sun3x_load_kernel`):
  BI_MACHTYPE(MACH_SUN3X), BI_MMUTYPE(MMU_68030), BI_CPUTYPE(CPU_68030),
  BI_MEMCHUNK(0, ram_size), [BI_COMMAND_LINE], [BI_RAMDISK], BI_LAST. No
  BI_FPUTYPE (kernel uses FPU emulation). Reset PC = ELF entry, SP = top of RAM
  − 0x1000.
* head.S requirements the machine satisfies: an `oriw #0x4000,0x61000000`
  "enable copro" write to ENAREG (absorbed); an early `putc` via the PROM
  `pv_putchar` pointer (romvec+24); and it copies the PROM's page table for the
  window 0xfee00000..0xff000000, reading the root pointer indirectly through
  `*(0xfefe00d4)` and copying 256 8K→4K descriptors.

### The genuine 68030 page-address rule (fixed in the core)

`target/m68k/helper.c get_physical_address_030()` masks a leaf PAGE
descriptor's frame to the page size (`table &= ~((1u<<MAX(M68K_TC030_PS(tc),8))-1)`,
helper.c:1275), per **MC68030 UM 9.1.2 / MC68851 UM 5.1.5.3.15 "Page Address"**:
real silicon discards the low (PS−8) bits of the frame field for a leaf page,
which are reserved for software. SunOS and the PROM both keep a software tag
there (e.g. a `+0x100` tag on every kernel obio descriptor). An earlier walk
folded those bits into the physical address, forcing several sun3x devices to be
placed at "double-added" addresses; the fix removed those compensations (the si
`+0x100` alias/fold, the split IDPROM holes, the level-7 clock ack device).

### SunOS mode: the PROM window and monitor RAM

| Region | Phys | Notes |
|---|---|---|
| Genuine boot PROM (128 KiB ROM) | 0xfefe0000 (`SUN3X_PROM_BASE`) | `memory_region_init_rom`; reset SSP/PC from ROM offsets 0/4 (SSP 0xfef60c00, PC 0xfefe0228) |
| Monitor scratch/stack/VBR RAM | 0xfef00000 (`SUN3X_PROMRAM_BASE`) | up to the PROM base; the monitor's own fixed VBR is 0xfef60c00 (`SUN3X_MON_VBR`) |
| PROM relocation window | 0x63000000 (`SUN3X_PROM_ALIAS`), 16 MiB | writable RAM preloaded with the ROM image; the cold-start self-test sets VBR := 0x63000000, reads its TC sentinel from 0x63007860, then copies itself here and runs from it (patches vectors, jumps past the first 128 KiB) |

The SunOS kernel installs its own VBR (observed 0xf8003800) as one of its first
acts; comparing `env->vbr == SUN3X_MON_VBR` is how the model tells "monitor in
control" from "kernel running" (used to gate the boot-countdown clock).

## Physical memory map (SunOS mode; NetBSD sun3x obio.h Type-1 space)

| Phys base | Size | Device |
|---|---|---|
| 0x0000_0000 | ram_size | Main RAM (default 16 MiB). VA==PA. |
| 0x5000_0000.. | — | OBMEM / P4 framebuffer window (bwtwo, see below) |
| 0x6000_0000 | 0x8000 | SUN3X_IOMMU — DVMA page table (RAM-backed scratch, 2048 PTEs × 4) |
| 0x6100_0000 | 0x2000 | ENAREG/DIAG block (absorb; reads 0) |
| +0x0400 | 1 | Bus error register (`SUN3X_BUSERR`) |
| +0x0c00 | 0x400 | sun3-IDPROM probe hole (bus-errors; `idprobe_hole`) |
| +0x1000 | 4 | Memory-error control/status register (parity) |
| +0x1004 | 4 | Memory-error address register |
| +0x1400 | 1 | Interrupt/enable register (`SUN3X_INTREG`) |
| 0x6200_0000 | 0x2000 | ZS1 — Z8530: keyboard/mouse in SunOS mode, console in Linux mode |
| 0x6200_2000 | 0x2000 | ZS2 — Z8530: console (ttya/ttyb) in SunOS mode, kbd/mouse (idle) in Linux mode |
| 0x6400_0000 | 0x800 | EEPROM/NVRAM: config byte @+0x1f, IDPROM @+0x7d8, MK48T02 TOD @+0x7f8 |
| +0x0010 | 1 | Monitor level-7 clock-pending status (`SUN3X_CLK_STAT`) |
| +0x2010 | 1 | Kernel-side clock-pending alias (`SUN3X_CLK_STAT2`, same state) |
| 0x6500_2000 | — | LANCE "le" Am7990 ethernet (autovector level 3) |
| 0x6600_0000 | 0x2000 | "si" NCR5380 SCSI + DMA/CSR block (autovector level 2) |
| 0x6e00_0000 | 0x1000 | 82072 floppy controller (no drive) |
| 0x6f00_003c | 1 | Printer / diagnostic port latch |
| 0x58000000..0x7bffffff | — | low-priority OBIO absorb (read 0 / write ignored); real registers overlay it |
| 0xfee0_0000 | 0x200000 | PROM window (identity in Linux mode) |

Everything otherwise unmapped raises a genuine 68030 bus error via a
lowest-priority background region that latches TIMEOUT (`sun3x_busfault_ops`).
Absent RAM banks above installed memory read back all-ones (floating bus,
`sun3x_memhole_read`) so the PROM's write/read-back sizer treats them as empty
rather than "partially filled".

## IDPROM (0x640007d8)

`struct idprom`, 32 bytes, checksum = XOR of bytes[0..14] in byte 15. Built by
`sun3x_build_nvram()`: byte0 id_format = 0x01, byte1 id_machtype =
SM_SUN3X|SM_3_80 = 0x40|0x02 = **0x42** (Sun 3/80), bytes2..7 ethaddr =
08:00:20:11:22:33, rest plausible/zero. The kernel accepts machtype 0x41 (3/470)
or 0x42 only when format==1. Both ufsboot and the kernel probe the *sun3*
discrete IDPROM first (a bus-error-guarded peek that must fault — served by the
`idprobe_hole` at 0x61000c00) before falling back to this sun3x IDPROM.

## Interrupt / enable register (0x61001400, 1 byte)

`sun3_intreg` bitmask (`sun3ints.c`):
* bit0 = master enable (`SUN3X_INT_ENABLE_ALL`)
* bit1/2/3 = software interrupt level 1/2/3 (`SUN3X_INT_SOFT_1/2/3`, SunOS only)
* bit5 = enable level-5 clock (`SUN3X_INT_ENABLE_5`)

The **level-5 clock tick** is the periodic (100 Hz, `SUN3X_HZ`) hardclock:
`level5 = tick_latch && master && (intreg & ENABLE_5)`, delivered to IRQC gpio 4.
The handler acks by pulsing bit5 low→high; a write with bit5 clear clears the
latch. SunOS's own hardclock ISR does the identical `andib #~0x20 / orib #0x20`
pulse through its page tables (virt 0xfedf9400 → phys 0x61001400).

### Interrupt routing and the OR gates

Level lines are driven through `TYPE_M68K_IRQC`. Three levels are **wire-ORed**
(open-drain), each via a `TYPE_OR_IRQ`, because more than one source shares the
line and driving one gpio input from two places is "last writer wins":

* **Level 6** (`zs_orgate`, 4 inputs): both Z8530 chips' channel-A/B IRQ outputs.
* **Level 3** (`lance_orgate`, 2 inputs): the "le" LANCE IRQ + software-int-3.
* **Level 2** (`si_orgate`, 2 inputs): the "si" NCR5380 IRQ + software-int-2.

The level-2 OR gate is the fix for the six-session "sustained-I/O completion
race" (journal session 33): `sun3x_update_irq()` re-drove level 2 from
software-int-2 alone ~200×/s (every tick, every INTREG ack pulse), erasing any
pending si interrupt at the controller while the si held its own line asserted —
the sleeping I/O process was never woken until the driver's 60–140 s watchdog
fired. The device/completion model was never wrong.

### Level-7: monitor boot-countdown clock vs NMI

Level 7 is used only as the genuine PROM monitor's boot-countdown clock, and
only while the monitor itself is in control. `sun3x_tick()` presents a fresh
0→7 NMI edge each tick; delivery is gated on `clk_pending && clk_enabled &&
vec_ok && !level5_active`, where **`vec_ok` requires `env->vbr == SUN3X_MON_VBR`**
before it even reads the level-7 vector slot (the VBR-identity rule). Once the
kernel installs its own VBR (0xf8003800) and acks a real level-5 tick
(`level5_active` latches), the auto-pulse stops for good. Without the VBR gate
the model stormed genuine kernel code with spurious NMIs → "stray level 7
interrupt" (journal session 15).

**A memory (parity) error is a BUS ERROR, not a level-7 NMI** (journal session
34 corrected session 15's premise). No model path raises level 7 for a memory
error; the kernel sees level 7 only for genuine NMIs.

## Memory-error / parity registers (0x61001000, 0x61001004) and bus error register (0x61000400)

Control/status byte at 0x61001000: `INTR 0x80` (r/o, error latched),
`INTENA 0x40`, `TEST 0x20` (writes store bad parity), `CHECK 0x10` (reads check
parity), `ERR24/ERR16/ERR08/ERR00 0x08/0x04/0x02/0x01` (r/o lane nibble).
Decoded **without A1/A0** — the PROM reads it both as `moveb 0x61001000` and as
`movel 0x61001000,%d0; andil #0xf` (0xfefe7fd2), so it is mapped 4 bytes wide
with every byte a mirror. `sun3x_memreg_read` returns only the r/w control bits
OR the latched status; a write clears the latch.

Error address register at 0x61001004: low 30 bits = the faulting byte address
(PROM masks `& 0x3fffffff` at 0xfefe7f88). The kernel acks with `andil
#0xc0000000` there and never touches the control register, so a write here must
also release the latch.

A CPU read of a byte lane carrying bad parity while CHECK is set terminates the
cycle with a genuine 68030 bus error (`MEMTX_DECODE_ERROR`), loads the bus error
register with `SUN3X_BE_MEMERR 0x80` (a *cause* register: loaded, not OR'd, so it
cannot accumulate with `SUN3X_BE_TIMEOUT 0x20`), and latches `INTR | (0xf &
~lanes)` + the address. The nibble the PROM expects after a single forced byte
write is `0x7/0xb/0xd/0xe` for byte 0/1/2/3 (0xfefe7fa2-0xfefe7fd2), i.e. `0xf`
with the bad lane's bit cleared. Bad parity cannot live in QEMU RAM, so a poison
set of byte addresses is kept and an MMIO overlay shadows RAM only while TEST is
set or poison remains — the same mechanism `hw/m68k/sun3.c` uses for the 3/60.

Evidence it is a bus error: both PROM parity subtests hook their "parity error
(level 7)" continuation into **exception vector 2** (`lea 0x20008,%a0; movel
%a1,%a0@` at 0xfefe7dbc / 0xfefe7f1e, VBR=0x20000) and never write level 7's own
slot (slot 31 holds the romvec `fbthere` char pointer, i.e. data). The installed
kernel agrees: its `T_BUSERR` path (munix_sun3x 0xf805826e) tests machtype 0x42
and `btst #7` of the bus error register before reading the memory error register;
its level-7 handler (0xf8004882) only reports "stray level 7 interrupt" unless
INTR is set. The kernel's INTR-gated level-7 path is for non-CPU (DVMA) errors,
which this model never generates.

## TOD clock — Mostek MK48T02 (NVRAM last 8 bytes, 0x640007f8)

Layout (`sun3x_tod_ops`, confirmed against the PROM subtest's own writes at
0xfefe769e): `+0` control, `+1` seconds, `+2` minutes, `+3` hours, `+4`
day-of-week (1..7), `+5` date, `+6` month, `+7` year, all BCD. Control bit 7 =
WRITE (freeze, accept a new time, commit on clear), bit 6 = READ (freeze for a
consistent multi-byte read). Seconds bit 7 = ST (stop).

**It has to really tick.** The PROM's TOD subtest (0xfefe75f8) polls the seconds
byte up to 65536 times for a *change* and otherwise reports `<Oscilator is not
running!, Re-Initializing TOD Clock Module>`; its recovery (0xfefe765a)
re-initialises with seconds = 0, after which the first comparison always matches
and the wedge is permanent. A frozen snapshot passed only by luck (the first
compare is against `seconds & 0x80`) — the intermittent hazard of journal
sessions 4/11/12/14, root-caused and fixed in session 34. Time comes from
`qemu_get_timedate()` + a guest-settable offset (committed via
`qemu_timedate_diff()`), so it advances at 1 Hz and honours `-rtc base=`. The
kernel's reader (munix_sun3x 0xf8051f82) sets control 0x40, reads the fields,
clears 0x40.

**Two-digit year:** the kernel's converter (0xf80520fe) reads BCD `yy` as
**1968 + yy**, so a truthful clock in 2026 (BCD 26) makes SunOS print 1994 — an
authentic SunOS 4.1.1 limitation real hardware reproduces; month/day/time/weekday
are all correct. `-rtc base=` inside SunOS's own 1968..2067 window makes it show
a matching absolute year (verified against the PROM's own self-test timestamps).

## EEPROM/NVRAM config byte and the `sunos-console` property

NVRAM byte 0x1f selects the console (NetBSD sun3 eeprom.h `EE_CONS_*`: BW 0x00,
TTYA 0x10, TTYB 0x11, COLOR 0x12). `sun3x_build_nvram()` writes it from the
machine property **`sunos-console=ttya|fb`** (`-M sun3x,sunos-console=...`,
default `ttya`): `fb` → 0x00 (local bwtwo + Sun keyboard/mouse), `ttya` → 0x10
(serial). The kernel's `_consconfig()` only links `conskbd`/`consms` to the real
`kbd`/`ms` STREAMS modules — and hence lets `/dev/kbd`/`/dev/mouse` open — when
this byte says local, which is why `sunos-console=fb` is required for the window
systems (journal sessions 28/29). Held as a file-scope `static bool`.

## Printer / diagnostic port (0x6f00003c)

A plain 8-bit read/write latch (`sun3x_lpreg_ops`). The PROM uses it as the
diagnostic LED register (writes the running subtest number, 0xfefe9c44), as its
progress-spinner scratch bit (`btst/bclr/bset #6`, 0xfefe9e08), and as the
subject of the "Printer Controller Check" subtest (0xfefee2c4), which walks all
256 values and requires each to read back exactly. Absorbing it (read 0) failed
that subtest on its first value and wedged the self-test. It is the **last**
subtest before "Selftest Completed." and the autoboot.

## The PROM loop-on-error contract (why a failed subtest hangs)

`0xfefe9c34` (subtest start) sets `%d7` bits 6 and 7; the error printer
`0xfefe9d94` clears bit 7; the post-subtest routine `0xfefe9fd2` does `btst
#7,%d7 ; bne -> jmp %fp@` (next subtest) else `moveal %sp,%a0; jmp %a0@` — and
every subtest stashed its own retry PC in `%sp`. **Any subtest that reports an
error is retried forever** (measured: 207704 re-entries of the ESP subtest in
80 s). This one mechanism explains every "hangs retrying" symptom in the
journal, and it means reaching `>` unattended requires *every* subtest to pass.

## zs / ESCC wiring (two Z8530s)

Both are `TYPE_ESCC`, `it_shift == 1`, `frequency == SUN3X_ZS_FREQ = 4915200`.
For it_shift 1: `channel = (addr>>2)&1` (A==1), `reg = (addr>>1)&1` (ctrl 0 /
data 1), so ch-A CTRL @ +4, ch-A DATA @ +6.

* **Console chip**: `chnA/chnBtype = escc_serial`, `chrA/chrB = serial_hd(0/1)`.
  Mapped at ZS2 (0x62002000) in SunOS mode (matching the 3/60), ZS1 in Linux
  mode. DCD is raised on both channels (`qemu_irq_raise` on the "dcd" gpio) —
  the Sun-3 serial ports have hardwired carrier; without it SunOS's console
  `open()` sleeps in the carrier wait forever (same fix as `hw/m68k/sun3.c`).
* **Keyboard/mouse chip**: `chnAtype = escc_kbd`, `chnBtype = escc_mouse`,
  `chrA/chrB = NULL`, `disabled = !machine->enable_graphics`. Mapped at ZS1
  (0x62000000) in SunOS mode. Same technique as sun4m's on-board ms/kb chip —
  the Sun keyboard/mouse protocol is generated internally, not bit-banged, so no
  baud wiring. Both IRQ outputs are wired into the level-6 OR gate.
* **Type-4 keyboard, US layout byte 0x00**: `chnA-sunkbd-layout = "0"`. escc.c's
  reset ID is hardcoded to Type-4 (matches the PROM's "Type-4 Keyboard." banner),
  but `sunkbd_layout_dip_switch()` defaults to a Type-5 dip-switch byte 0x21
  ("en-us", US5.kt). OpenWindows 2.0's NeWS tables only understand Type-4-era
  codes (KB_SUN4_US = 4 = type 4 + layout 0x00 … KB_SUN4_JAPAN = 36 = 4 + 0x20);
  a (type=4, layout=0x21) pair matches nothing → `ClassKeyboard couldn't
  initialize the keyboard` (journal session 32). The `"0"` override reports the
  correct (type=4, layout=0x00). sun4m is left at its default.
* **ESCC character-length masking**: TX/RX truncate to the WR5/WR3-programmed
  character length (`escc_tx_bits`/`escc_rx_bits`) instead of forwarding the full
  8-bit byte, fixing garbled output once a driver switches the line to a reduced
  width (journal session 23). At the 8-bit reset default the mask is `0xff` — a
  no-op — so sun4m/q800/NeXT are unaffected.
* **RR3 Tx-pending resync**: escc.c's WR1 write path resyncs RR3's
  `INTR_TXINTA/B` bit to `s->txint && (val & INTR_TXINT)`, so a poll/interrogate
  routine reading RR3 to find the interrupt source sees the live, correctly-masked
  condition rather than a stale snapshot. This was the true root cause of the
  ~630,000/s level-6 zs storm (journal session 12), *not* the OR-gate wiring or
  the zs IPL.

## si SCSI — NCR5380 at 0x66000000 + DMA/CSR block

The "si" is an `NCR5380` (`TYPE_NCR5380`) instantiated with `reg-shift = 2` and
`sun-mode = true`, plus a surrounding DMA/CSR block (`sun3x_sireg_ops`, an 8 KiB
region at 0x66000000). Autoconfig prints `sm0 at obio 0x66000000 pri 2`.

**Register map** (5380 registers 4 bytes apart, reg0 at **+0x08**):

| Offset | Read | Write |
|---|---|---|
| +0x00 | DMA count low byte | DMA count low byte |
| +0x04 | DMA count high byte | DMA count high byte |
| +0x08 | 5380 reg0 CSD (data in / FIFO drain) | 5380 reg0 ODR (CDB staged pre-select) |
| +0x0c | 5380 reg1 ICR | ICR (bit7 = DMA arm strobe in sun-mode, **not** bus reset) |
| +0x10 | 5380 reg2 = live bus phase (sun_phase_bits) | MR (selection: target = MR & 7) |
| +0x14 | 5380 reg3 = dispatch code (0x08 phase / 0x20 state) | TCR |
| +0x18 | 5380 reg4 CSB | SER |
| +0x1c | 5380 reg5 BSR (IRQ bit = live /REQ) | — |
| +0x20/+0x24/+0x2c | si scratch registers (read-back register test; these are the real ESP CFG1/CLKCONV/CFG2) | scratch |
| +0x1000 | si_csr (16-bit) | si_csr |
| +0x1004 | si DMA address (HW auto-increments) | si DMA address |
| +0x1008 | si DMA byte count | si DMA byte count |

The 5380 device MMIO is mapped at 0x66000008 at higher priority; the scratch
words at +0x20/+0x24 are reclaimed by a small overlay forwarding to the sireg
handlers.

**si_csr bits** (NetBSD sun3 sireg.h): DMA_ACTIVE 0x8000, DMA_CONFLICT 0x4000,
DMA_BUS_ERR 0x2000, ID 0x1000 (r/o), FIFO_FULL 0x0800, FIFO_EMPTY 0x0400,
SBC_IP 0x0200 (5380 int pending), DMA_IP 0x0100, DMA_EN 0x0010, SEND 0x0008,
INTR_EN 0x0004, FIFO_RES 0x0002, SCSI_RES 0x0001.

* **The real SCSI-bus-reset strobe is `SI_CSR_SUN_RESET 0x0080`**, *not* the
  layout's SCSI_RES/FIFO_RES names (those toggle on every ordinary command's
  finalise). Every genuine reset — PROM self-test (0xfefe7284), ufsboot
  (0xfeff0fc8), and the kernel `sm_reset()` (0xf8075158 writes 0x80 then 0) —
  writes 0x80 then 0, one-for-one with the console's "resetting scsi bus". A
  0x80 write calls `ncr5380_sun_bus_reset()` (`scsi_req_cancel` any open request
  via the bus cancel path, then clear bus/transaction state) and zeroes the
  si DMA address/count latches (journal session 25).
* **si_csr read semantics**: always returns `ID | (si_csr & (SBC_IP|DMA_IP))`.
  Bit0 ("bus/command busy") is synthesised per consumer, keyed by guest PC:
  kernel text (0xf8000000..0xf9000000) sees bit0 = `dev != NULL` (a present
  target holds it through a command, an absent target reads 0 → clean no-device);
  the PROM (0xfefe…) and ufsboot (0x0021…) keep bit0 = 1.
* **DVMA / IOMMU** (`sun3x_dvma_to_phys`): 16 MiB DVMA space, 8 KiB pages, 2048
  PTEs of 4 bytes at SUN3X_IOMMU (0x60000000); PTE valid when low bits set;
  `phys = (pte & 0xffffe000) | (dva & 0x1fff)`.
* **Pseudo-DMA engine** (`sun3x_si_dma_run`): direction comes from the 5380's
  live data phase, **not** the si_csr bits — the PROM's DMA-arm encodes a
  data-IN read as `si_csr |= 0x300` and a data-OUT write as `|= 0x200`, the
  opposite of a naive SEND/DMA_IP reading. Moves bytes one at a time through the
  IOMMU, auto-incrementing dma_addr (the byte count the driver reads back), and
  sets DMA_IP on completion.
* **DMA arm/count write order** (journal session 9): the PROM/ufsboot load
  dma_addr and both count bytes *before* writing `si_csr |= SBC_IP`; the kernel
  writes the arm bit *before* the count latches. `sun3x_si_maybe_run_dma()` is
  therefore called from every one of the four writes that can complete the
  "armed" state (both count bytes, the 32-bit count alias, the si_csr arm) so the
  run fires when the last lands regardless of order, gated on `sun_dma_done`
  (cleared per command) rather than an SBC_IP edge (si_csr is never cleanly
  cleared between commands).
* **The completion interrupt** (journal session 7): the kernel `sm` driver's
  real I/O path sleeps in tsleep()/biowait() for a level-2 trap. When
  `ncr5380_ack()`'s kernel-gated PHASE_MI branch takes the final MESSAGE-IN byte,
  it raises BSR_IRQ and arms `sun_cmd_irq` with `sun_cmd_irq_reads = 4`.
* **The 4-read completion pulse** (journal session 21): `_sm_intr`'s dispatch
  needs si_csr bit1 set for a short, fixed run of reads. It is **address-
  independent**: every completion is one indifferent "state-machine inner loop"
  read followed by exactly three consecutive reads that need bit1 (the isrlink
  claim test, the handler's recheck, the state-machine entry gate). Presenting
  bit1 for the first *four* si_csr reads after arming, with no PC gate,
  reproduces the shape on any kernel binary (MINIROOT and the installed GENERIC,
  linked ~0x4a988 apart). This is an access-*count* contract, verified exact over
  1,430 completions — still on the open-issues list as a latent fragility (see
  Part 2).
* **DMA_IP / SBC_IP semantics**: `_sm_intr`'s first gate is `si_csr & 3` (bits
  0-1), not SBC_IP/bit9 (a session-6 misreading corrected in session 7).
* **Stale BSR_IRQ** (`ncr5380_sun_reap_stale_irq`, journal session 5): the
  polled kernel driver never reads reg7 (RPI) to retire the 5380's /IRQ latch;
  several BSR_IRQ causes fire and clear `dev` in the same synchronous step, so by
  the time the CPU takes the trap the cause is stale and unclaimable. When the
  kernel claim test (`si_csr & 3`) finds nothing to claim while /IRQ is still up,
  the model reaps the latch — standing in for the reg7 read the driver never
  issues — instead of storming.
* **`sun_dma_drained_pending`** (journal sessions 13/14): real hardware
  re-asserts /REQ the instant the si drains a data phase; the model's per-byte
  drain clears CSB_REQ and nothing re-asserts it, so the driver's next reg3 poll
  mis-routes into the state dispatcher → "Lost Busy". A one-shot latch (set at
  drain, consumed by exactly the one reg3 read that would otherwise mis-route)
  fixes it *outside* `ncr5380_ack()`/`ack_release()` — re-asserting CSB_REQ
  itself doesn't survive the driver's own DMA-arm-strobe ICR pulse, whose
  `ncr5380_ack()` clobbers REQ. `ncr5380_sun_to_status()` additionally asserts
  CSB_REQ unconditionally at the DMA-finalise site (the "Unexpected phase"
  state-0x11 fix, session 14): the arm-strobe leaves ICR_ASSERT_ACK set, so
  `ncr5380_enter_status()`'s REQ guard would otherwise never fire.
* **DATA OUT completion** (journal session 16): `ncr5380_flush_data_out()` takes
  an `advance_phase` param; the sun-mode DMA SEND path passes `!sun_mode`
  (defers the STATUS transition to `ncr5380_sun_to_status()`, symmetric with the
  read side) while the Mac ROM's byte-by-byte writer passes `true`. Before this
  every sun-mode DMA write hung in an infinite "write retry" loop.
* **FIFO-dump integrity** (journal session 18): the kernel's post-timeout
  diagnostic "Snap shot" reads reg0 sixteen times to sample the FIFO; the model
  now presents the byte *without* advancing buf_pos for kernel-text reads during
  a live DATA IN phase, so the dump no longer eats real bytes off the front of a
  still-pending DMA buffer (which had truncated a retried READ by 16 bytes).
* **IDENTIFY byte** (journal session 5): the kernel's interrupt-driven launch
  (ICR=0x42, ATN) stages an IDENTIFY message byte (0xC0|lun) ahead of the CDB in
  the reg0 FIFO. `ncr5380_select_target()` strips a leading byte with bit 7 set
  before building the CDB (no real opcode has bit 7 set); the CDB length comes
  from `scsi_cdb_length(cmd)` capped by the staged count (the kernel pads reg0 to
  16 bytes). An absent-target select under the ATN regime raises BSR_IRQ +
  `sun_sel_failed` so the driver's err-0x12 completion path runs; the plain
  polled probe scan (no ATN) does not, to avoid re-storming.
* This model completes each SCSI command **synchronously** inside one guest
  register access (`ncr5380_pump` drains the whole request); integrity across the
  self-healing timeout/reset events is verified byte-exact host-side.

## LANCE "le" ethernet (0x65002000, autovector level 3)

An AMD Am7990 modelled with the generic `TYPE_LANCE` (`hw/net/lance.c` /
`pcnet.c`) already used by sparc/sun4m — RDP/RAP at +0/+2, matching
`drivers/net/ethernet/amd/sun3lance.c`'s `iobase[0]`/`[1]`. DMA goes through the
**same IOMMU/DVMA translator** as the si SCSI (`sun3x_lance_dma_read/write` on
the realized device's `phys_mem_read`/`phys_mem_write`, one byte at a time so an
invalid PTE mid-buffer is caught). Byte-swap contract: `sun3lance.c` always
programs CSR3 with BSWP set, so descriptors/init-block get the 16-bit swap
(do_bswap==0 path) and frame bodies pass through byte-for-byte (BSWP path) —
reusing pcnet.c/ledma's existing convention. IRQ shares level 3 via
`lance_orgate`. MAC forced to the IDPROM address 08:00:20:11:22:33 (SunOS reads
the ether address from the IDPROM and programs it into the init block, so the
two must agree). A one-line portability fix guards lance.c's default
`ledma_memory_read/write` assignment behind `#ifdef CONFIG_STP2000` so a
non-sparc target can link `TYPE_LANCE` and supply its own DMA hooks.

## bwtwo P4 framebuffer (0x50300000) and video

`hw/display/sun-bwtwo.c` (`TYPE_SUN_BWTWO`) models the on-board mono frame buffer
as a genuine Sun **P4-bus device** (property `p4=true` for sun3x; `p4=false` is
the on-board 3/60 variant). Autoconfig prints `bwtwo0 at obmem 0x50300000 pri 4`
/ `resolution 1152 x 900`. Four MMIO regions mapped by sun3x.c:

* **0x50300000, 4 bytes — the P4 register.** Top byte READ-ONLY = board-ID nibble
  (0x0 bwtwo) | resolution nibble (0x1 = 1152x900) = fixed **0x01**; low byte
  read/write control: 0x80 DIAG, 0x40 READBACK_CLR, **0x20 VIDEO** (display
  enable), 0x10 SYNC, **0x08 VTRACE** (free-running ~67 Hz read-only status bit,
  no interrupt), 0x04 INTCLR, 0x02 INTEN, 0x01 FIRSTHALF; middle two bytes read
  0; writes to the top byte silently discarded. Confirmed byte-exact against the
  kernel's `p4probe()` (0xf80a9482, pokes `orig ^ 0x7f000000` and checks the
  readback: unchanged top bits = read-only ID = real card) and `bwtwoioctl`
  (video = 0x20), and the PROM's own `bset #5/bclr #2/bset #3` on reg+3.
* **0x50302000, 128 KiB — kernel VRAM alias (phase 0).** `bwtwoprobe()` sets
  `softc.addr = reg + 8192` and `bwtwommap()` resolves mmap offset → `softc.addr
  + offset`; this is where any userland `mmap(/dev/fb)` and pixel (0,0) land.
* **0x50400000 and 0x50420000, 128 KiB each — PROM VIDEOMEM_BASE aliases (phase
  0).** The PROM font blitter and its generic P4 scan touch a 256 KiB span from
  reg+0x100000; two consecutive mirrors cover it (an 18-bit onboard decode).

All three are true `memory_region_init_alias()` onto one 128 KiB VRAM backing
store, so pixel (0,0) is byte 0 in every window (the phase-mismatch fix, journal
session 31). No separate control register at OBMEM base and no catch-all
wraparound over the rest of OBMEM. Pixels: 129600 bytes (1152x900x1bpp, 144-byte
rows, MSB = leftmost pixel, 1 = black), dirty-tracked via `DIRTY_MEMORY_VGA` +
`gfx_update`, like `hw/display/cg3.c`. VIDEO is enabled at reset so the PROM
banner is visible without SunOS touching the register. Verified: guest
`mmap()`-and-fill paints the whole screen byte-exact
; the PROM banner and Sun logo
render crisply (`sunos-prom-screen.png`).

The bwtwo interrupt line is deliberately **not wired** to a CPU IRQ: `bwtwointr`
(0xf80a3248) never inspects any register when `softc.p4regs != 0` for the
onboard case, nothing in open/attach/ioctl sleeps on it, and there is no ack path
safe to hold a level asserted with.

## FDC (82072 floppy, 0x6e000000, no drive)

A minimal empty-controller responder (`sun3x_fdc_ops`) so the kernel's fd probe
concludes "drive absent" via the NEC/82072 command/result handshake instead of
timing out on a dead MSR and crashing its timeout/reset handler on the tiny ISP.
MSR at +0 (always RQM; DIO|CB in the result phase), data/FIFO at +1; SENSE
INTERRUPT STATUS → ST0+PCN, RECALIBRATE/SEEK to the absent unit → not-ready ST0
(0x70|unit), VERSION → 0x90. FCR @0x6e000400, FVR @0x6e000800 absorb.

## The synthesised PROM (Linux mode, phys 0xfee00000, identity)

Built by `sun3x_build_prom()`. `romvec` at 0xfefe0000: pv_getchar (+20),
pv_putchar (+24), pv_nbgetchar (+28), pv_nbputchar (+32) → hand-assembled zs
stubs; pv_monid (+76) → "QEMU sun3x"; pv_reboot (+96)/pv_abortentry (+152) →
halt stub. `*(0xfefe00d4)` → 0xfefe0200 → 0xfefe0400 = 256 × u32 identity page
descriptors `(0xfee00000 + i*0x2000) | 0x59` (PRESENT|ACCESSED|DIRTY|NOCACHE030).
The putchar/getchar stubs drive ch-A of the ZS at 0x62000000 (ctrl +4, data +6),
enabling WR5.TXEN / WR3.RXEN themselves and polling RR0. The Linux runtime console
is a native console-only Z8530 driver (`sun3x-tools/zs-console.patch`,
`console=ttyS0`); the romvec stubs remain required for head.S's earliest `putc`
and the page-table root.

---

# PART 2 — HOW WE GOT HERE, WHAT IS KNOWN, WHAT IS STILL OPEN

Milestones are conclusion-only; journal session numbers are pointers in brackets.

## Milestones

* **M1 — genuine PROM to the `>` monitor prompt.** Map the 128 KiB ROM,
  reset-into-PROM from its own vector, real bus-error-on-unmapped, floating-bus
  memory sizing, IOMMU scratch RAM, INTREG software interrupts, zs console, valid
  IDPROM. Determinism came from merged 68030/68851 PMMU core fixes (PTEST `An`
  writeback, PMOVEFD TLB flush, ATC root-tagging) plus the level-7 NMI vector
  gate. The monitor's boot-countdown clock is level-7, reached through virtual
  0xfef06010 (pending) / 0xfef0b400 (ack) which its page tables map to physical
  0x64000010 / 0x61001400 (the ack is a plain INTREG write).

* **M2 — si NCR5380 + DVMA, disk READ fires.** 5380 at +0x08, reg-shift 2; reg2
  = live phase, reg3 = dispatch; CDB staged via reg0 FIFO and run on select;
  ICR bit7 = DMA strobe, not reset; IOMMU DVMA. Two coupled bugs fixed:
  `sun_phase_bits()` returned 3 up front (finalise without arming DMA) → return 1
  until drained then 3; and the DMA-run trigger keyed off an SBC_IP edge that a
  stale si_csr swallowed → gate on `sun_dma_done`. INQUIRY(36) → READ(6) →
  disklabel 0xDABE → ufsboot loads the kernel.

* **M3 — root mounts.** IDPROM: both ufsboot and the kernel probe the absent
  *sun3* discrete IDPROM first, which must bus-error (the `idprobe_hole` at
  0x61000c00) so they fall back to the valid sun3x IDPROM. reg5/BSR IRQ modelled
  as the live target /REQ so the PROM's `(reg5 & 0x1f) != 0` and ufsboot's `reg5
  == 0` polls both work. The trailing-completion state walk (`sun_cmd_complete` /
  `sun_msg_taken`: reg2→7, reg0→0x00, reg3→0x08 then 0x20) makes a no-data
  command's sd-open TUR return ≥0 instead of "bdevvp: bad open". A disklabel
  16-bit `dkl_nblk` truncation in the mkdisk tool was fixed for the root
  partition. → `root on sd6a fstype 4.2`.

* **M4 — Sun-3/80 recognised.** Same sun3-probe bus-error hole for the kernel's
  own IDPROM reader (0x61000c00) → machtype 0x42, autoconfig reaches sm/st/sr.

* **M5 — kernel SCSI probe completes.** Selection fires on any staged CDB with
  `target = MR & 7` (not the "arbitrate" bit 0, which is just the target LSB and
  was breaking even targets); si_csr bit0 = `dev != NULL` for kernel PCs so the
  driver's busy-poll exits; CDB length from `scsi_cdb_length`. → full sd0-sd6
  config.

* **M6/M7 — sd-open, FDC, autoconfig.** The kernel `sm` STATUS handshake holds
  ACK; advance STATUS→MESSAGE-IN on the ACK assert (kernel-gated). The ISP-
  overflowing level-7 NMI storm during a long IPL-7 busy-wait was fixed by gating
  the pulse to the monitor's VBR plus the 82072 FDC responder (which removes the
  IPL-7 FDC-probe wait). "unknown memory error" panic fixed by masking the
  memory-error register read to the control bits. → full autoconfig
  (sm/st/sr/sd/fdc/fd/zs/le).

* **M8 — the page-address-bits bug.** The kernel-memory allocator spun forever
  because the 68030 walk folded a page descriptor's low software-tag bits into
  the physical address. The fix masks the leaf frame to `TC.PS` (MC68030 UM
  9.1.2); SunOS's rmap sentinel then reads back where it was written. This
  invalidated several empirically-placed device addresses, which were re-homed
  (the level-7 clock, the si register window, the IDPROM holes).

* **Level-2 storm / INTR_EN gate.** Early on the si IRQ was gated through
  SI_CSR_INTR_EN, which the real driver never writes — equivalent to disabling
  the interrupt, so the kernel's interrupt-driven I/O path blocked forever
  [session 3]. Forwarding it unconditionally left a bounded residual burst that
  the stale-BSR_IRQ reap [session 5] and address-independent completion signalling
  [session 21] cleaned up; the residual "storm" was finally root-caused to the
  level-2 wire-OR collision, not the device [session 33].

* **Level-5 hardclock** [session 4]: SunOS's hz clock is level-5, was being
  suppressed in SunOS mode; delivered and acked (INTREG 0x20 pulse) like Linux.
  Level-7 reverts to NMI-only once a real level-5 tick is acked.

* **Lost Busy and its final fix** [sessions 7-14]: root-caused to reg3
  mis-routing because CSB_REQ does not survive the driver's DMA-arm-strobe ACK
  pulse; fixed by the one-shot `sun_dma_drained_pending` latch, and the state-0x11
  "Unexpected phase" hang it unmasked fixed by asserting CSB_REQ unconditionally
  in `ncr5380_sun_to_status()`. DATA OUT completion [session 16] and the
  FIFO-dump integrity bug [session 18] followed.

* **The sustained-I/O race, root-caused (session 33).** The single most-chased
  bug: not the si/5380 model but a **level-2 interrupt-line wire-OR collision** in
  sun3x.c that erased the pending si interrupt ~200×/s. Sampling the *guest CPU*
  (idle loop, IPL 0, /IRQ asserted, no trap) instead of the device settled it.
  Fix: the level-2 OR gate. Throughput: newfs of the 238 MB partition 1355 s / 29
  events → 4.4 s / 0; ~160 KB/s → 9-17 MB/s.

* **Warm-reboot bus error (session 33).** A leaked one-shot in QEMU's own m68k
  68030 bus-error emulation: `env->bus_error_suppress` ("complete the
  instruction, don't rerun the data cycle") was global and sticky, so a
  guarded peek whose handler resumes *without* re-running the access left it armed
  to eat the next boot's first guarded IDPROM peek. Fixed by binding the
  suppression to the faulting address (`bus_error_suppress_ar`). `sync; reboot` →
  login in ~70 s.

* **LANCE (session 22)**, **bwtwo P4 model and VRAM base (sessions 24/27/30/31)**,
  **consconfig/ENXIO and the `sunos-console` property (sessions 28/29)**, and the
  **OpenWindows keyboard layout (session 32)** — all as described in Part 1.

* **memory-error-as-bus-error and TOD clock (session 34).** Session 15's premise
  (a memory error needs a level-7 NMI) was wrong — it is a bus error via vector 2
  — which removed the fragile NMI vector gate and made the forced-parity subtest
  pass unattended. The TOD "Oscilator is not running!" wedge was the clock never
  ticking; replaced with a real MK48T02. The printer port and the ROM
  loop-on-error contract were also modelled/identified.

## Things that were tried and must not be re-tried

* **A level-triggered phase-mismatch interrupt** breaks ufsboot: raising the
  5380's phase-mismatch IRQ as a level (not an edge from `set_phase`) fires the
  instant a driver arms TCR=4 while data is still flowing, making the 3/60
  ufsboot abandon a transfer mid-way ("si: wrong phase"). It is raised only from
  `ncr5380_set_phase`.
* **The direct level-6→level-3 zs IPL retarget double-faults.** Retargeting the
  zs group from level 6 to level 3 (on the evidence of the kernel's `pri 3`
  autoconfig line) reproducibly caused a fatal double MMU fault right after "dump
  on". `pri 3` is the *software* interrupt level the tty layer runs at after the
  real level-6 hardware handler; the zs is correctly level 6. (Reverted [session
  11]; root cause was the RR3 staleness [session 12].)
* **Gating the si IRQ on SI_CSR_INTR_EN** disables the interrupt entirely (the
  real driver never writes it) → the interrupt-driven path blocks forever
  [session 3].
* **PC-locked (literal-address) completion signalling** works on one kernel
  binary and storms on another — the installed GENERIC links `_sm_intr` ~0x4a988
  from MINIROOT. Use the address-independent 4-read access-count instead [session
  21].
* **Deferring the SCSI command / DMA-arm / whole selection** with a qemu_clock
  timer: three attempts, all reverted. A deferred CDB dispatch storms "Illegal
  cmd" because reg2 and the real CSB disagree during the window; deferring the
  DMA arm/drain desyncs the driver's finalise (which does not poll between arm and
  finalise); a 67× delay-length change had no effect. The race was never a timing
  problem [sessions 17/18].
* **REQ-persistence via `ncr5380_ack()`/`ack_release()` gating**: the DMA-arm ACK
  pulse always clobbers REQ before the next poll, and the PROM self-test depends
  on `ack_release()`'s *unconditional* generic STATUS transition — any added gate
  hangs the self-test. Use the isolated one-shot latch [session 13].
* **Advancing to STATUS the instant `sun_dma_done` latches** breaks PROM/ufsboot
  disk detection (their trailing blind reg0 reads reach the ST/MI teardown too
  early) [session 10].
* **Seeding VRAM[0]=0x41** to fix the PROM font stride steers the PROM into a
  register-dump routine whose scratch resolves to the Mem Err Reg MMIO and
  double-faults; the real fix is the read-only P4 ID byte = 0x01 [sessions 30/31].
* **A held bwtwo level-4 vsync interrupt** is dead code for the onboard config
  and has no ack path (storm risk) — VTRACE is a status bit only.
* **Don't gdb-`break` inside the ROM window** (writes to a ROM region silently
  fail); shadow it or breakpoint just before the call site.

## What is verified (regression suite)

Tools in `sun3x-tools/`: `boot-sunos.py` (PROM + MINIROOT/installed boot,
`SUN3_CMDS`/`SUN3_CMD_TIMEOUT` overrides, `SUN3_MAXLOG` flood guard),
`boot-sunos-reboot.py` (warm-reboot test, first-boot Esc handling only),
`sunos-symbols.py` (UFS walker + a.out nlist/`match` cross-binary symbol naming),
`sunos-write-verify.py` / `sunos-read-verify.py` (host-side byte-exact partition
verifiers), `install/mk-install-disks.py` + `install/install-drive.py` (the
disk-only install recipe). Logs in `sun3x-tools/logs/`.

* **PROM-only** (`-bios`, no disk, Esc-abort): self-test passes, `>` monitor
  prompt, `le: cannot initialize` / netboot retry (no disk). Unattended
  (`SUN3_NO_ESC=1`): all of phase 1 `{pass}` + Configuration Memory + LANCE
  Controller checks, then the ESP wedge (see open issues).
* **SunOS MINIROOT** (`sunos-boot-pristine.img` at unit 3): full autoconfig,
  `root on sd6a`, `#` shell, all ten transcript commands (`boot-sunos.py`
  prints `SUNOS_SHELL_OK`). 0 Lost Busy / stray level 7 / lost interrupt /
  timeout.
* **Installed GENERIC, ttya**: multi-user, `login:`, root login, `uname -a` =
  `SunOS 4.1.1 1 sun3x`, `cc` builds and runs; `sync; reboot` → login in ~70 s,
  0 bus errors (`sunos-4.1.1-installed-multiuser-sun3x.log`).
* **Installed GENERIC, `sunos-console=fb`**: PROM banner, kernel autoconfig
  (`bwtwo0 ... 1152 x 900`), `Amnesiac login:` all on the local screen
  (`sunos-console-boot.png`, `sunos-prom-screen.png`); SunView desktop with mouse
  focus and keyboard (`sunos-sunview-desktop.png`, `-typing.png`); OpenWindows
  2.0 NeWS/X11 desktop, cmdtool + File Manager, mouse + keyboard reach a real
  client (`sunos-openwin-desktop.png`, `-typing.png`).
* **WRITE / READ integrity**: `sunos-write-verify.py` (25 writes, 238 MiB
  partition) and `sunos-read-verify.py` (cross-target interleaved copy) both
  byte-exact, including bytes outside any write reading back zero, across the
  self-healing events.
* **Linux/sun3x**: boots to `smolsh`, `uname -a` answered
  (`linux-sun3x.log`). **Linux/sun3-60**, **NetBSD/sun3-60**: unaffected
  (the shared-core `bus_error_suppress` and escc.c/lance.c changes are no-ops for
  their paths).

## What is still open (latest status; do not list anything later sessions fixed)

* **The PROM's unattended forced-parity self-test does now pass** (session 34) —
  but the unattended self-test still cannot reach `>` on its own because of the
  next item; every driver script therefore Esc-aborts the self-test.
* **The remaining self-test blocker: the phase-2 `ESP SCSI Controller Check`**
  (d7=0x23, 0xfefee09c). It reset-pulses `0x66001000` (0x80→0) then requires
  `+0x08` to behave as a 16-deep FIFO and `+0x1c` as its FIFO-flags count —
  i.e. the Sun 3/80's SCSI is really an **NCR53C90/53C94 (ESP)**, and the
  NCR5380 "si" this project has used since M2 is a stand-in bent to satisfy the
  PROM boot driver and SunOS's `sm` driver. Modelling the real ESP
  (`hw/scsi/esp.c` exists) would invalidate the completion-race tuning of
  sessions 5-25 and was explicitly not attempted. A contained alternative:
  present the FIFO/FIFO-flags view at +0x08/+0x1c only while armed by the 0x80→0
  pulse — but only after measuring whether the PROM boot driver or `sm` ever
  writes 0x80 alone there (needs runtime instrumentation; a ROM grep can't answer
  it, both reach the block through a base register). Reconnaissance with a
  throwaway FIFO stand-in confirmed that with the ESP check satisfied the floppy
  and printer checks pass and the self-test reaches `>`.
* **TOD year**: SunOS 4.1.1 reads the two-digit BCD year as 1968+yy, so a
  truthful 2026 clock prints 1994 — an authentic OS limitation, not a model bug;
  `-rtc base=` gives a matching absolute year.
* **The 4-read completion pulse (`sun_cmd_irq_reads == 4`)** is an access-*count*
  contract rather than a level held until the ISR acknowledges. Verified exact
  over 1,430 completions but with no margin, so it stays a latent fragility. The
  natural acknowledge points, if reworked: `sm_intr`'s entry `orl #16,(csr)` and
  `sm_cmd`'s `csr |= 0x10` / `csr &= ~0x10` pair (bit 4 = interrupt-driven).
* **OpenWindows right-click / workspace menu**: reported unverified on the 3/80;
  the 3/60 work (session 38) found HMP `mouse_button 2` (not 4) is the
  middle/right button and both menus then draw and select — the 3/80 attempt used
  the wrong bitmask. The `target-gfx.img` disk image also sometimes ships a
  broken/empty `/dev/fb` symlink (a data artifact, not a model bug); the window
  systems need `ln -s bwtwo0 /dev/fb` once per boot.
