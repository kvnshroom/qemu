# QEMU `-M sun3-60`: the Sun-3/60 hardware contract

The Sun-3/60 ("Ferrari") is a 20 MHz 68020 with the discrete Sun-3 MMU (8 contexts, segment map, 256 PMEGs of 16 8 KiB pages), zs serial/keyboard/mouse, an ICM7170 clock, an on-board 1152x900 bwtwo, the "si" NCR5380+Am9516 SCSI and an Am7990 LANCE; the model is `hw/m68k/sun3.c` plus the board hooks in `target/m68k` (`m68k_set_fc_ops`).
What it boots today: the genuine 3.60 v3.0.1 PROM to the `>` monitor on ttya or on its own screen with the Type-4 keyboard; NetBSD/sun3 1.5.2 RAMDISK by gdbstub injection and the INSTALL kernel from a miniroot disk; Linux/m68k sun3 (7.3.0-rc4 branch) to a shell natively from `-kernel`, from a boot disk through the genuine PROM (`sun3-tools/bootloader/`) or by injection; SunOS 4.1.1/sun3 MINIROOT from disk and an installed GENERIC system to multi-user `login:` with SunView and OpenWindows 2.0 on the screen and networking through `le0`.
Every register, bit and address below was cross-checked against the code named above; the evidence pointers (ROM offsets, kernel symbols) say where each fact was read from, and bracketed session numbers point at the old journals.
The session journals this file was distilled from (SUN3-NOTES.md at the top level of the old tree) are preserved in branch `sun3-history`.
Companion files: `sun3-tools/` (boot drivers, the install recipe in `install/INSTALL-LOG.md`, evidence logs and screenshots in `logs/`).

## 1. The Sun-3 MMU and the CPU hooks

Geometry: 28-bit virtual address (`SUN3_VA_MASK` 0x0FFFFFFF), 8 KiB pages, 128 KiB segments (16
pages), 2048 segments per context, 8 contexts, 256 PMEGs (4096 PTEs in one flat `pgmap[]`). PTE
index = `segmap[context][va >> 17] * 16 + ((va >> 13) & 15)`.

PTE layout (evidence: PROM diag maps segmaps 0..255 and 4096 PTEs; NetBSD `pte.h`):

| bit | meaning |
|---|---|
| 0x80000000 | VALID |
| 0x40000000 | WRITE |
| 0x20000000 | SYSTEM (supervisor only) |
| 0x10000000 | NC (no cache, ignored) |
| 0x0C000000 | TYPE: 0 obmem, 1 obio, 2 VME16, 3 VME32 |
| 0x02000000 | REF, set on every translation |
| 0x01000000 | MOD, set on stores |
| 0x0007FFFF | page frame (<< 13) |

Rules the translate hook (`sun3_mmu_translate`) enforces, in order: invalid PTE -> bus error
register INVALID (0x80); user access to a SYSTEM page -> PROTERR (0x40); store to a non-WRITE
page -> PROTERR. Physical = `(type << 32) | frame << 13 | offset` -- the four PTE type spaces
are four 4 GiB windows of the 64-bit system address space. A writable page enters the QEMU TLB
read-only until the first store has set MOD, so MOD is exact.

Control space (MOVES with SFC/DFC = 3) decodes address bits 31..28; the VA is the low 28 bits:

| 0xN.... | register | width / behaviour |
|---|---|---|
| 0 | IDPROM | byte, `idprom[va % 32]` |
| 1 | page map | 32-bit entry of the current context selected by the VA; byte/word accesses use big-endian lanes; a store flushes the page's TLB aliases |
| 2 | segment map | byte (PMEG number) of the current context; a store flushes the whole TLB |
| 3 | context | 3 bits; store flushes the TLB |
| 4 | system enable | byte (table below); store flushes the TLB and drives the bwtwo video enable |
| 5 | user DVMA enable | byte, stored only |
| 6 | bus error register | byte, read clears it |
| 7 | diagnostic LEDs | byte (the cold start writes lamp patterns here) |

Boot state: with `SUN3_ENA_NOTBOOT` (0x80, not bit 0 -- the monitor leaves boot state writing
0xA0) clear, every supervisor program fetch, and every MOVES read with FC 2/6 in supervisor
mode, reads `ROM[addr & 0xFFFF]` regardless of address (evidence: the ROM checksum loop at
0xFEFACA6 reads VA 0..0xFFFD with SFC = 6). Data accesses translate normally. Machine reset
loads SP/PC from ROM words 0/1 (0x0FE60C00 / 0x0FEF00EE, the watchdog/warm entry; cold start is
0xFEFAB80), zeroes both maps, context and all registers, and holds the bwtwo blank.

EPROM frames decode the *untranslated* low 16 address bits: any PTE whose frame lands in obio
0x100000..0x11FFFF reads `ROM[va & 0xFFFF]` (the monitor maps several ROM data pages with pfn
0x80 and expects each to show its own offset; its device map table is read that way at ROM
0xD350).

Page granularity: this tree's m68k `TARGET_PAGE_BITS` is 10, so one 8 KiB Sun page is filled
into the QEMU TLB as 1 KiB target pages with byte-exact physical addresses. A pgmap store
flushes every target page of the 8 KiB page in all 16 sign-extension aliases (bits 31..28),
because the PROM reaches its device pages with short absolute addresses (0xFFFFE004 -> VA
0x0FFFE004) and the TLB is tagged with 32 bits (session 1; the 4 KiB figure there is history).

MOVES with FC 1/2/5/6 goes through `sun3_moves_data`: an access crossing an 8 KiB page boundary
is split into two translated cycles (copyin/copyout corruption otherwise, session 1). FC 0/4/7
bus-error like the stock `moves_chk`.

Faults from the hook raise the 68020/030 bus error: data faults use the long format B frame with
SSW DF (0x0100), RW (0x0040) for reads and the function code in the SSW FC field (`4|1` = 5 for
supervisor data, 6 program, 1/2 user). NetBSD's `KDFAULT()` requires FC = 5 to route kernel copy
faults to the kernel map (session 1). Instruction faults use the short format A frame with SSW
FC (0x8000) or FB (0x4000) and, with them, the rerun flags RC (0x2000) / RB (0x1000): SunOS's
`trap()` copies RC->FC, RB->FB before choosing the stage to page in and killed every fork child
with SIGSEGV when only FC/FB were set (session 36, `target/m68k/op_helper.c`).

Debug accesses (`m68k_cpu_get_phys_addr_debug`) translate through the hook as supervisor data in
the current context, which is what lets the gdbstub write a kernel into RAM at the monitor
prompt.

## 2. Physical maps

RAM: type 0 at offset 0, default 16 MiB, maximum 24 MiB (`SUN3_RAM_MAX`; larger `-m` is
refused). Each type space has a 4 GiB background at priority -1 that latches bus error TIMEOUT
(0x20) and returns `MEMTX_DECODE_ERROR`, i.e. a bus error; that is how the PROM sizes memory and
probes optional devices.

On-board I/O (PTE type 1), from the PROM map table (ROM 0xD350) and the NetBSD kernels' cfdata:

| obio | device | ipl | size / notes |
|---|---|---|---|
| 0x000000 | zs1 keyboard (A) / mouse (B) | 6 | ESCC, `it_shift` 1 |
| 0x020000 | zs0 ttyb (B) / ttya (A) | 6 | +0 B csr, +2 B data, +4 A csr, +6 A data (PROM zs self-test uses 0xFFFE004/6) |
| 0x040000 | EEPROM | - | 2 KiB RAM, zeroed except byte 0x1F |
| 0x060000 | ICM7170 clock | 5 / 7 | byte registers, `addr & 0x1F` |
| 0x080000 | memory error registers | 7 | one page |
| 0x0A0000 | interrupt register | - | one byte, aliased over the page |
| 0x100000 | boot EPROM | - | 64 KiB, select range to 0x11FFFF |
| 0x120000 | LANCE | 3 | 4 bytes, only with `model=lance` |
| 0x140000 | si SCSI | 2 | 0x28 bytes |
| 0x1E0000 | memory control | - | stub: reads 0, writes ignored (PROM maps it with pfn 0xF0) |

On-board memory (type 0) above RAM: 0xFF000000 bwtwo VRAM (256 KiB window), 0xFF1C0000 bwtwo ID
byte (one byte), 0xFF600000 the EPROM twice (2 x 64 KiB). 0xFF200000 and 0xFF300000 are in the
PROM's map table (P4 register on other 3/xx boards) but do not exist on the 3/60 and stay bus
errors (session 37).

## 3. System enable register (control space 4)

| bit | name | notes |
|---|---|---|
| 0x01 | RES | |
| 0x02 | FPA | |
| 0x04 | COPY | |
| 0x08 | VIDEO | bwtwo display enable, 0 at reset (PROM fb dispatcher 0xFEF22E8 case 3; NetBSD `enable_video`; SunOS locore 0xE004C92) |
| 0x10 | CACHE | |
| 0x20 | SDVMA | |
| 0x40 | FPP | |
| 0x80 | NOTBOOT | boot-state exit; the monitor writes 0xA0 |

## 4. Interrupt register (obio 0xA0000) and levels

| bit | name | effect |
|---|---|---|
| 0x01 | ALL_ENAB | 0 masks every level |
| 0x02 / 0x04 / 0x08 | SOFT_1..3 | each both enables and asserts its level |
| 0x10 | VIDEO_ENAB | level-4 retrace latch, one per frame at 67 Hz while set; clearing the bit clears the latch (SunOS `bwtwointr`) |
| 0x20 | CLOCK_ENAB_5 | level-5 clock latch |
| 0x80 | CLOCK_ENAB_7 | level-7 clock latch (the monitor runs on 0x81, SunOS on 0xA1) |

Wired sources and levels: si 2, LANCE 3, video 4, clock 5, the four zs channels 6 (one board
line per channel: wiring only ESCC output 0 lost all tty interrupts, session 1), clock NMI and
memory error 7. The CPU level is the highest pending. Rule from session 33 (kept here): never
drive one CPU level from two places -- every device has its own source line and
`sun3_irq_update()` ORs them.

The clock interrupt is latched on the board once per enabled level: the ICM7170's INT output
sets both latches; writing the register with a clock bit clear drops that level's latch only.
SunOS acks the level-7 tick with 0xA1 -> 0x21 -> 0xA1; with one shared latch the stale level-5
latch came back as a second level-7 interrupt (`Exception 0x7C at 0x0FEF03DE`, session 35).

Level 7 is an edge: `m68k_set_irq_level()` sets `nmi_edge_pending` on a transition to 7, so the
parity NMI is taken at SR IPL 7 and the monitor's 100 Hz level-7 clock works with the mask
raised (session 1).

## 5. Bus error register (control space 6)

| bit | name | who sets it |
|---|---|---|
| 0x01 | WATCHDOG | reset entry checks it (never set by the model) |
| 0x02 / 0x04 | FPAENERR / FPABERR | not set |
| 0x08 | VMEBERR | not set |
| 0x20 | TIMEOUT | any access to an unassigned address in any type space |
| 0x40 | PROTERR | SYSTEM page from user mode, or store to a read-only page |
| 0x80 | INVALID | PTE not VALID |

The PROM self-test masks with 0xFC and compares. Reading clears the register. DVMA faults never
touch it (section 9).

## 6. Memory error (parity) registers, obio 0x80000

CSR at +0: 0x10 INTENA, 0x20 TEST (stores write bad parity), 0x40 CHECK; reads return the
enables plus 0x80 IPEND and the failing byte lanes in the low nibble (lane = `0x8 >> (addr &
3)`). Writing +0 sets the enables and clears the latch; a write to any other offset (the PROM
zeroes +4 around its test) clears the latch and keeps the enables; either write drops the
level-7 line. Bad parity cannot live in QEMU RAM, so while TEST is set or poisoned bytes remain,
an MMIO overlay (priority 1) shadows RAM: stores with TEST set add the address to a poison set,
other stores remove it, and a load with CHECK set that touches a poisoned byte latches
IPEND|lanes and, with INTENA, interrupts at level 7. Evidence: the PROM power-up "Parity Memory
Forced Error Test" expects exactly this (session 1).

## 7. EEPROM (obio 0x40000, 2 KiB)

Only byte 0x1F is written by the machine: console select 0x00 screen + keyboard, 0x10 ttya, 0x11
ttyb (PROM probe 0xFEF1F20; cold-start selector 0xFEF40B4..0xFEF4112; honoured without
checksumming). The `sunos-console` property sets it (`ttya` default -> 0x10, `fb` -> 0x00).
Other bytes the PROM reads and defaults for: 0x16 screen size code (0 = 1152x900, SunOS
`bwtwoprobe` size 0x1FA40), 0x50/0x51 console rows/columns (34/80).

## 8. ICM7170 clock (obio 0x60000)

Byte registers at `addr & 0x1F`: 0x00 1/100 s (a read latches all eight counters), 0x01 hour,
0x02 min, 0x03 sec, 0x04 month, 0x05 day, 0x06 year, 0x07 day-of-week, 0x08..0x0F RAM, 0x10
interrupt (read = status and clears it; write = mask: 0x02 100 Hz, 0x04 10 Hz, 0x08 1 Hz, 0x10
min, 0x20 hour, 0x40 day, 0x80 = pending on read), 0x11 command (0x03 crystal, 0x04 24-hour,
0x08 RUN, 0x10 INT_ENABLE, 0x20 TEST). INT is asserted while INT_ENABLE and `status & mask`;
reading the status register drops it, the board latches (section 4) keep the level. The monitor
programs command 0x1C and mask 0x02 (100 Hz). Alarms are plain RAM.

## 9. zs0 / zs1 (ESCC)

zs0 at obio 0x20000: channel A = `-serial` 0 (ttya), channel B = `-serial` 1 (ttyb), PCLK
4915200. Both channels' "dcd" inputs are raised by the board and survive chip resets
(`escc_set_dcd`; the NetBSD console open sleeps forever without carrier, and a listening socket
chardev looks disconnected at reset, session 1). RR15 reads back WR15 and RR2 is recomputed from
still-pending external/status interrupts (the three ESCC fixes carried by the branch).

zs1 at obio 0: channel A keyboard (`escc_kbd`, csr 0x0FE00004 / data +2 in the PROM's map, 1200
baud: WR12/13 = 0x7E/0), channel B mouse (`escc_mouse`). Disabled (idle) under `-nographic`.
Keyboard: reset command 0x01 answers `ff 04 7f` (Type 4), layout query 0x07 answers `fe <dip>`
with `chnA-sunkbd-layout=0` (US -- OpenWindows 2.0's NeWS tables know only Type-4 layouts). The
reply is delivered 10 ms after the command (`SUNKBD_REPLY_DELAY_NS`): NetBSD's `kbd_iopen()`
drains its transmitter before sleeping for the reply, so a synchronous reply logged `kbd0: reset
failed` on every open (session 37). PROM evidence: init 0xFEF3FB2, reset sender 0xFEF04C8, state
machine 0xFEF3B08 storing the byte after 0xFF as keyBid (0xDE = no keyboard -> "Using RS232 Port
A as input!"). Mouse: 5-byte MSC packets, each clamped to +-127 per axis; a larger host motion
goes out as consecutive packets in one sync, and a packet that does not fit the SERIO queue is
dropped whole (a remainder carried to the next event made a click land 268 px away, session 38,
commit 41733265a3).

## 10. si SCSI: NCR5380 + Am9516 UDC + FIFO, obio 0x140000, level 2

Register map (PROM si init at ROM 0x89B8 maps 0x22 bytes; NetBSD `si_obio_attach` maps 0x28;
PROM map table maps VA 0x0FE12000 with PTE 0xD40000A0):

| offset | width | register |
|---|---|---|
| +0x00..0x07 | byte | NCR5380 registers, byte-spaced (`reg-shift` 0, overlaid at priority 1) |
| +0x08/0x0A/0x0C/0x0E | word | VME DMA address/count: scratch (the PROM and SunOS write them anyway; 32-bit stores are split) |
| +0x10 | word | UDC register data |
| +0x12 | word | UDC register select |
| +0x14 | word | FIFO word latch (odd-byte pickup) |
| +0x16 | word | FIFO byte counter, loaded by the driver, counts down per byte |
| +0x18 | word | si CSR |
| +0x1A/0x1C | word | VME byte-pack: scratch |
| +0x1E | word | VME interrupt vector: scratch (PROM writes 0x3D00) |
| +0x20 | word | VME FIFO count high: scratch |

Byte accesses return the addressed lane; a byte write is merged into the 16-bit register.

CSR bits: 0x8000 DMA_ACTIVE (UDC chain armed), 0x4000 DMA_CONFLICT (never raised, only cleared),
0x2000 DMA_BUS_ERR, 0x1000 ID (reads 0 = on-board), 0x0800 FIFO_FULL (never), 0x0400 FIFO_EMPTY
(always), 0x0200 SBC_IP (5380 IRQ output), 0x0100 DMA_IP, 0x0010 DMA_EN (VME, r/w), 0x0008 SEND
(direction, 1 = to device), 0x0004 INTR_EN, 0x0002 FIFO_RES, 0x0001 SCSI_RES. The two resets are
**active low**: a 1->0 edge of SCSI_RES cold-resets the 5380, resets the UDC and clears
DMA_BUS_ERR/DMA_CONFLICT; FIFO_RES low zeroes the word latch (NetBSD `si_obio_reset` csr = 0,
delay, csr = 7; PROM 0x910E csr = 0, delay, csr = 3). Board interrupt = INTR_EN and (SBC_IP or
DMA_IP).

UDC (Am9516) through the select/data pair: 0x22 chain address low, 0x26 chain address high (bits
23..16 in 15..8), 0x2E master command (0x00 reset, 0x32 channel-interrupt enable, 0xA0 start
chain), 0x32 current word count (readable as the residual), 0x38 channel mode (0x0D).
Start-chain fetches six words from DVMA at the chain address: `{rsel 0x182 receive / 0x282 send,
addrh = ((pa & 0xFF0000) >> 8)
| 0x40, addrl = pa & 0xFFFF, count in words, cmrh 0x40, cmrl 0xD2 receive / 0xC2
send}` (PROM 0xFEF8DAC, NetBSD `si_obio_dma_setup` 0xE0A534A, SunOS `si_ob_dma_setup`). The
model keeps DMA address = `((addrh & 0xFF00) << 8) | addrl` and the word count.

The engine moves words between the 5380 data phase and DVMA: a receive assembles two bytes into
a big-endian word; an odd trailing byte stays in the FIFO latch's high byte for the driver (PROM
0xFEF8CA0, NetBSD `si_obio_dma_stop`, SunOS `si_dma_recv`); a send stops at the pad byte when
the byte counter reaches 0. A DVMA fault sets DMA_BUS_ERR and stops the chain. Traffic is logged
under `-d mmu`; a CSR SEND / 5380 direction mismatch under `-d guest_errors`.

Genuine-5380 behaviour added for this board (non-`sun-mode`, the 3/80's `sun_mode` paths
untouched): selection completes on ICR SEL|DATA with BSY released, no arbitration needed (the
PROM writes ICR = 1 then 5 and never asserts BSY) and no interrupt on selection; ATN + MESSAGE
OUT accepts an IDENTIFY byte (bit 7) and takes the LUN from it; a DRQ output (sysbus IRQ 1)
while DMA is armed and a byte is pending; `/EOP` sets END_OF_DMA and interrupts with MR
ENABLE_EOP (0x08); loss of BSY interrupts only with MR MONITOR_BSY (0x04); leaving MR DMA_MODE
disarms the chip and clears END_OF_DMA; ICR RST resets the bus.

### The completion contract (session 35: each rule broke a boot without it)

1. **DMA_IP is an error bit.** SunOS `si_cmdwait` waits for SBC_IP (`si_wait(csr, 0x4200, 1)`)
   and fails the command if `csr & 0x4100` (munix 0xE067660, `bne` into its reset path); the
   PROM decodes bit 8 as "dma overrun" (ROM 0xFEF8C7A). The model raises it only when the word
   count ran out while the target still offered data.
2. **The DMA engine takes time.** SunOS reads the 5380's reset-parity/interrupt register right
   after arming (MINIROOT 0x0E0704BE / 0x0E071EDA); an instantaneous engine let that read retire
   the completion interrupt. The transfer runs from a timer: 20 us + 100 ns per byte
   (`SUN3_SI_DMA_LATENCY_NS` + `count * 2 * SUN3_SI_DMA_NS_PER_BYTE`), started by the 5380's DRQ
   or the UDC start.
3. **The clock latch is per level** (section 4).
4. **The phase-mismatch interrupt needs only MR DMA_MODE**, no DMA start register: SunOS arms a
   no-data command with TCR = 4 (impossible phase) + DMA_MODE and sleeps (MINIROOT
   0x0E072098..0x0E0720AC = munix `_si_cmd`); without it every TEST UNIT READY timed out. It is
   an edge raised only where the target changes phase: the transition into STATUS (all drivers)
   and the start of a data phase for an initiator that armed itself that way at command issue
   (`irq_on_data_phase`). Less selective variants measured to break things: a level-triggered
   version made ufsboot abandon transfers (`si: wrong phase, cbsr 0x64`); edges on the COMMAND /
   MESSAGE IN transitions (whose TCR the PROM programs a byte-handshake behind this model's
   phase advance) stopped the PROM finding the disk.

SCSI unit numbering (INSTALL-LOG): qemu `unit=0/1/3` = SunOS sd0/sd2/sd6; the PROM boot string
takes the unit in hex, `b sd(0,18,0)` = unit 3. A blank sd0 runs a zero boot block (`Exception
0x2C at 0x00006000`) and drops to `>`; a target with `installboot` done auto-boots. ufsboot
truncates a partition's sector count to 16 bits (root <= 65024 sectors); the MINIROOT panics
"can't create kernel process" without a `b` swap partition.

## 11. Sun-3 DVMA

There is no IOMMU: a bus master presents a 24-bit address which the CPU's own MMU translates as
virtual `0x0F000000 | addr` in the **current context**, as a supervisor data access, chunked at
8 KiB pages, REF/MOD maintained (`sun3_dvma_rw`). Evidence: NetBSD `dvma_kvtopa` (0xE091BF8)
returns `kva & 0xFFFFFF` and requires the DVMA window (`dvma_init` maps 0x0FF00000..0x0FFFE000),
`dvma_mapin` installs its segments in all contexts (`set_segmap_allctx`); the PROM's DMA address
is the buffer VA & 0xFFFFFF (0xFEF8DC0) and its `le_xmit` uses a buffer in place only if `(va &
0x0F000000) == 0x0F000000`; SunOS's kernel heap lives inside the window (`_Sysbase` 0x0F000000,
`_Syslimit` 0x0F806000, `_DVMA` 0x0FF00000) so `kmem_zalloc` VAs masked to 24 bits are DVMA
addresses. A fault is the device's error: SI_CSR_DMA_BUS_ERR or pcnet CSR0 MERR (RXON/TXON off),
logged under `-d guest_errors`; the CPU's bus error register is restored, not latched.

## 12. On-board bwtwo (0xFF000000), `sunos-console`

| what | value | evidence |
|---|---|---|
| VRAM | 128 KiB at obmem 0xFF000000, 1152x900x1, 144 bytes/line, MSB first, 1 = black; mirrored at 0xFF020000 to fill the 256 KiB the PROM maps and clears | PROM map table: VA 0x0FE20000, 32 pages, pfn 0x7F800; `fbAddr` = 0x0FE20000; probe clears 256 KiB with `clrw` (0xFEF25EC); NetBSD `bw2attach` 0x20000 (no P4 register: `fb_pfour_id`'s write-back probe sees RAM); SunOS `bwtwoprobe` takes `p4probe() < 0` |
| display enable | enable register bit 0x08; blank is drawn white | section 3; `sun-bwtwo` property `p4=false` exposes a "video-enable" input |
| resolution ID | one byte at 0xFF1C0000 reading 0x80: bit 7 set = 1152x900, clear = 1600x1280; writes ignored | PROM probe 0xFEF2586 peeks VA 0x0FE1A000 (pfn 0x7F8E0) and stores width/height only when the peek succeeds -- without the byte the console drew with a zero stride; NetBSD `bw2attach` / SunOS `bwtwoprobe` peek it too, defaulting to 1152x900 on a bus error |
| retrace interrupt | interrupt register 0x10, level 4, 67 Hz, cleared by clearing the bit | SunOS `bwtwointr` (0xE09ED72) -> locore 0xE004CF6 `andib #~0x10,INTREG`; PROM and NetBSD never set it |

The early monitor stack at VA 0x0FE60C00 maps into the VRAM, so the VRAM must exist even for a
serial console. With `sunos-console=fb` (EEPROM 0x1F = 0) the PROM probes the bwtwo and
keyboard, sets romvec inSource/outSink = 0 (SunOS `consconfig` 0xE09599A follows them), writes
nothing at all to ttya and no getty runs there; the default `ttya` keeps every serial script
byte-identical.

## 13. LANCE (`le0`), obio 0x120000, level 3

Present **only** when a NIC of model `lance` is configured (`qemu_find_nic_info(TYPE_LANCE)`):
an implicit default NIC and a plain `-nic user` have no model and are ignored, so the default
machine still bus-errors on the PROM's le probe (`Timeout Bus Error ... Paddr: 00120003, FC 5 at
0x0FEF75F4`) and falls into the monitor, which `boot-linux.py` / `boot-netbsd-ramdisk.py` rely
on; with the chip present a diskless auto-boot RARPs forever (`Requesting Internet address for
8:0:20:36:0:60`).

| +offset | width | register |
|---|---|---|
| +0 | word | RDP, data of the CSR selected by RAP |
| +2 | word | RAP, CSR select 0..3 |
| +4.. | - | obio background -> bus error |

Byte cycles work on both (PROM `andib #-4,(3,a4)` / `orib #n` on RAP's low byte, ROM
0xFEF75DA..; SunOS `leprobe` = byte `pokec(reg, 0)` to RDP): a byte write is one word cycle with
the byte on both lanes (`val * 0x0101`), a byte read a word read of which the CPU keeps its lane
-- deliberately not a read-modify-write, which would write CSR0's write-1-to-clear bits back.
Drivers: PROM CSR3 = 4 (BSWP), INIT, polls IDON (no INEA), STRT; SunOS `leinit` CSR1/2 = init
block, CSR3 = 4, INIT, STRT|INEA (0x42), `leintr` acks 0x440 / 0x2040; NetBSD `le_match` is a
word peek at 0x120000, `sc_conf3 = 4`, 8 receive / 2 transmit buffers. DMA is section 11 through
pcnet's `phys_mem_read/write` hooks. Byte order: with BSWP the descriptors and init block
(`do_bswap == 0`) get a 16-bit swap and frame bodies pass byte for byte -- the sun4m `ledma`
contract. The chip's address PROM is never read: the guests take the MAC from the IDPROM, so the
qdev `mac` is forced to it (08:00:20:36:00:60).

IDPROM (control space 0, 32 bytes): [0] format 0x01, [1] machine 0x17 (Sun-3/60), [2..7] MAC,
[8..14] zero (date, serial), [15] XOR of bytes 0..14 -- exactly Linux's `SM_SUN3|SM_3_60`
expectation.

## 14. What each guest needs from the model

* **PROM 3.60 v3.0.1**: boot-state redirect incl. FC6 MOVES; NOTBOOT = 0x80 (with bit 0, `g
  4000` ran ROM[0x4000]); EPROM low-16 decode; VRAM for the early stack; parity test + level-7
  edge; ID byte at 0xFF1C0000; EEPROM 0x1F; the 100 Hz clock on level 7 with intreg 0x81; 5380
  selection without arbitration; either a bus error at 0x120003 (monitor) or a LANCE (RARP
  loop).
* **NetBSD 1.5.2**: SSW FC = 5 on kernel data faults; one interrupt line per zs channel; DCD;
  MOVES page splitting; the debug translation hook for injection; the 10 ms keyboard reply;
  `le0` word peek.
* **SunOS 4.1.1**: SSW RC/RB rerun flags; the four completion rules; IDENTIFY LUN and
  MONITOR_BSY; the video interrupt ack; `consconfig` keyboard/mouse on zs1; `/etc/netmasks`
  keyed by the class-A number (`10 255.255.255.0`) and `/sbin/hostname` present (rc.boot runs it
  before /usr is mounted; otherwise `Amnesiac login:`, session 39).
* **Linux/m68k sun3**: section 15.

## 15. Booting Linux/sun3: what the kernel needs from the model

No machine change was needed for Linux (`sun3-linux-NOTES.md`): the MMU, zs, ICM7170 at level 5,
interrupt register and IDPROM built for NetBSD suffice. The kernel-side contract:

* **romvec at 0x0FEF0000** (`LINUX_OPPROM_BEGVM`): `prom_init()` stores it, `config.c` reads
  `pv_sun3mem` and `pv_monid`, `mmu_emu_init()` calls `pv_setctxt()` to copy segmaps into the
  other 7 contexts, `hvc_sun3` drives `pv_nbputchar/pv_nbgetchar` (routed by the PROM to ttya).
  So the PROM must have run its power-on init first, and it stays mapped at runtime (mmu_emu
  reserves PMEG 247 for the ROM and copies the ROM/IO maps into every context).
* **Load/entry**: `KERNBASE` 0x0E000000, link at 0x0E002000; `sun3-head.S` runs at the physical
  copy through the monitor's low identity map, copies the segmaps from VA 0 up to KERNBASE until
  the first invalid PMEG (0xFF), then jumps high. A loader puts each PT_LOAD at `p_vaddr -
  0x0E000000`, zeroes the BSS tail (`sun3-head.S` never does), appends a bootinfo at `_end`
  (`setup_arch()` NULL-derefs without one) and enters at `e_entry - KERNBASE` = 0x2000 with the
  MMU on in context 0. Only `BI_MACHTYPE = MACH_SUN3` is consumed: `config_sun3()` takes the
  memory size from `pv_sun3mem` (BI_MEMCHUNK is overwritten), `CONFIG_BOOTPARAM_STRING`
  ("console=hvc0" in `sun3-linux.config`) replaces BI_COMMAND_LINE, BI_RAMDISK is generic
  (`CONFIG_BLK_DEV_INITRD=y`: an external initramfs is unpacked). Three loaders do this:
  * **`-kernel` without `-bios`** (`sun3_load_kernel()` / `sun3_build_prom()` /
    `sun3_kernel_boot_maps()`): no PROM image at all. The 64 KiB EPROM region holds a synthesised
    PROM: the romvec at offset 0 (= VA 0x0FEF0000, as in the genuine ROM) with `pv_sun3mem`,
    `pv_memoryavail`, `pv_monid` "QEMU", `pv_romvers` 2 and hand-assembled stubs for
    putchar/getchar (ttya through the PROM's zs page VA 0x0FE02004/6, enabling WR5.TXEN / WR3.RXEN
    themselves), halt/reboot (`bra .`) and `pv_setctxt`, transcribed from the ROM's setcxsegmap
    (0x0AC8: DFC = 3, clear NOTBOOT so that the stub's own fetches are redirected to the EPROM
    while the other context is selected, switch, store, switch back, restore). Reset preloads the
    maps the kernel expects: RAM identity-mapped with PMEG n = physical segment n (the monitor
    maps 8 MB; here all of RAM), the PROM's device pages in PMEG 254 at 0x0FE00000 (map table
    below), the EPROM in PMEG 247 at 0x0FEF0000, every other segment 0xFF; enable register
    NOTBOOT|SDVMA; PC = 0x2000. `-initrd` is loaded two pages below the top of RAM (BI_RAMDISK),
    `-append` becomes BI_COMMAND_LINE. Unlike sun3x's path (MMU off, PAGE_OFFSET 0, head.S copies
    the PROM's page table) no code of ours runs before the kernel: the boot state is pure MMU
    data. `-bios` with `-kernel` is refused.
  * **The boot disk** (`sun3-tools/bootloader/`, section 15b): the genuine PROM's `b sd(0,0,0)`
    loads an 8 KiB boot block which loads the kernel through the PROM's own disk driver.
  * **Injection** (`sun3-tools/boot-linux.py`): gdbstub writes at the `>` prompt, then `g 2000`.
* **The 8 MB budget**: the monitor identity-maps only the low 8 MB (reads through 0x7FE000,
  fault at 0x800000); an injected or disk-loaded image must fit (trimmed kernel + initramfs,
  about 4 MB; the boot block itself sits at 0x780000). The `-kernel` path maps all of RAM.
* **IDPROM**: `id_machtype` 0x17, `id_format` 0x01, XOR checksum -- section 13.
* Kernel deltas on the Linux `sun3` branch (master + 6 commits): memblock registration with
  `min_low_pfn`/`max_low_pfn` and bottom-up allocation, a PROM early console, `ELF_ET_DYN_BASE`
  lowered to 0x04000000 (user space ends at 0x0E000000), `hvc_sun3`; config from
  `sun3_defconfig` without NET/SCSI/most FS/self-tests, `HARDENED_USERCOPY`,
  `STRICT_KERNEL_RWX`. Boot with `-m 24M`.
* Result: `Linux version 7.3.0-rc4-sun3`, `CPU: 68020 / MMU: Sun-3 / FPU: 68881`, interactive
  smolsh (`sun3-tools/logs/linux-sun3-60.log`). Known: the guest wall clock runs several times
  fast (100 Hz ICM7170 tick vs `legacy_timer_tick`, cosmetic), and a sporadic spurious SIGUSR1
  seen with the earlier busybox initramfs was never root-caused.

## 15b. The PROM's boot-block interface (evidence for `sun3-tools/bootloader/`)

Read from the 3.60 v3.0.1 ROM (`objdump -D -b binary -m m68k:68020 --adjust-vma=0x0fef0000`)
and from the two working boot blocks on disks at hand: NetBSD's `bootxx` (miniroot.gz, sectors
1..10, 4728 bytes) and SunOS's `bootsd` (sun3-installed-gfx.img, sectors 1..9, 4404 bytes,
written by `installboot`); both disassembled with `--adjust-vma=0x4000`.

* **Dispatcher** (`b dev(c,u,p) file args`, ROM 0x5A00..0x5D7E): fills the bootparam block in
  globram (0xFFFFE21C; `romvec+12` points at a pointer to it): `argPtr[8]`@0, `strings[100]`@32,
  `devName[2]`@132, `ctlrNum`@134, `unitNum`@138, `partNum`@142, `fileName`@146, `bootDevice`@150
  (the device's boottab). Builds a saioreq on its stack, calls `b_probe`, then the generic
  `b_boot` (0x5E98): `si_bn = 1, si_cc = d_maxiobytes (8192), si_ma = 0x4000, b_strategy(si, 1)`,
  checks the count and returns 0x4000, which the caller jumps to ("Boot: load failed" otherwise).
  So a boot block is **sectors 1..16 of the partition, at most 8 KiB, entered by `jmp 0x4000`
  in supervisor mode**, with the monitor's maps and stack, nothing in registers: `bootxx` and
  `bootsd` both re-open the device through `bootParam->bootDevice`.
* **Tables** (2-byte packed, Sun cc): boottab `{b_dev[2]@0, b_probe@2, b_boot@6, b_open@10,
  b_close@14, b_strategy@18, b_desc@22, b_devinfo@26}` (sd: ROM 0xDB94); devinfo
  `{d_devbytes, d_dmabytes, d_localbytes, d_stdcount, d_stdaddrs, d_devtype, d_maxiobytes}`
  (sd: 0, 0x2020, 0x2C, 1, [0], 0, 0x2000); saioreq `{si_flgs@0, si_boottab@2, si_devdata@6,
  si_ctlr@10, si_unit@14, si_boff@18, si_cyloff@22, si_offset@26, si_bn@30, si_ma@34, si_cc@38,
  si_sif@42, si_devaddr@46, si_dmaaddr@50}`.
* **The caller provides the buffers**: `si_devdata` (d_localbytes, zeroed by the driver),
  `si_dmaaddr` (d_dmabytes: 8 KiB of data + the UDC chain table at +0x2000), `si_devaddr` for
  devices with d_devbytes (sd has none: the ROM's si driver uses the monitor's own page at
  0x0FE12000). `bootsd`'s `alloc(type, n)`: 0 = RAM from 0x200000 up, 1 = DVMA VA from
  0xFFF00000 up, 2 = DVMA VA mapped by page-map writes to pages taken downward from
  `(*memoryavail >> 13) - 1`; its `dev_mapin(type, addr, len)` writes PTEs `0xF0000000 | type
  << 26 | pfn`. `bootxx`'s `dvma_init` copies the segment map of 0x200000..0x2DFFFF (where it
  runs) over 0x0FF00000..0x0FFDFFFF, so DVMA address 0xF00000 + x is its own RAM.
* **sd `b_open`** (0x8576): target = unit >> 3, LUN = unit & 7, TEST UNIT READY, START UNIT,
  reads sector 0 into si_dmaaddr, validates the label (0x7002: "No label found - attempting boot
  anyway"), `si_cyloff = part[si_boff].start_cyl * ntrks * nsect`. Returns -1 on failure.
* **sd `b_strategy(si, rw)`** (0x866C -> 0x86A4): READ6 (rw 2 = WRITE) for `(si_cc + 511) / 512`
  sectors, **adds si_cyloff into si_bn in place**, DMAs into si_dmaaddr through its 24-bit DVMA
  address (`va & 0xFFFFFF`, section 11: translated as VA 0x0F000000 | addr in the current
  context), then `bcopy(si_dmaaddr, si_ma, si_cc)` when they differ. Returns the (rounded) byte
  count, 0 on error: ask for whole sectors.
* **State at entry** (maps dumped with `-d int` at the `>` prompt): context 0; VA 0..0x7FFFFF ->
  PMEG 0..0x3F identity (PTE 0xD000xxxx: VALID|WRITE|NC, no SYSTEM); 0x0FE00000 PMEG 0xFE (device
  pages), 0x0FE20000/0x0FE40000 0xFD/0xFC (VRAM), 0x0FE60000 0xFB (page 0 = the top RAM page:
  the PROM stack and, via `movec %sp,%vbr` at 0x01A6/0xAB8C, its vector table -- low RAM is free),
  0x0FEA0000 0xF9 (EPROM copy), 0x0FEE0000 0xF7 (ROM in pages 8..15), 0x0FF00000..0x0FF7FFFF
  0xF6..0xF3 (the PROM's DVMA area, 0xF6 pages 0..2 = top RAM pages), 0x0FFE0000 0xF8 (globram in
  page 15); all 8 contexts written alike. Both example blocks start with `move.w #0x2700,%sr` and
  copy themselves from 0x4000 to 0x240000 before loading anything.
* **Our block** (`bootloader/entry.S`, `boot.c`, 1.6 KB): copies itself to 0x780000, re-opens the
  device from bootparam, `segmap[0x0FF00000] := segmap[0x780000]` (the bootxx trick) so that
  `si_dmaaddr = 0x0FF00000 + (dmabuf - 0x780000)`, reads the header sector 17 (magic `SUN3LNX1`,
  entry, up to 30 segments of {sector, bytes, physical destination}) and the segments in 8 KiB
  strategy calls, restores the segment map, `b_close`, `jmp entry`. `mk-bootdisk.py` writes the
  label (install/mk-sun3-disks.py), the block, the header and the flattened kernel (+ initrd,
  + command line) from sector 32. Result: `Boot: sd(0,0,0)` -> `Linux/sun3 boot: sd(0,0,0),
  memory 24 MB` -> smolsh, `PROM Revision: 3.0.1` (`sun3-tools/logs/linux-sun3-60-bootdisk.log`;
  typed `b sd(0,8,0)` with the disk at unit 1 in `-bootdisk-typed.log`). Not chased: loading
  NetBSD's kernel with it (the block loads any segment list, but the NetBSD kernel wants its
  own a.out entry state).

## 16. How to run / verify

Build: `configure --target-list=m68k-softmmu --disable-docs --disable-werror && ninja
qemu-system-m68k`. ROM: `3.60_v3.0.1_rom` (64 KiB, `-bios`).

    # PROM to the > prompt on ttya (no disk: the le probe bus-errors into the monitor)
    qemu-system-m68k -M sun3-60 -bios 3.60_v3.0.1_rom -display none -serial mon:stdio

    # Linux/sun3 injection (SUN3_QEMU, SUN3_ROM, SUN3_KERNEL=vmlinux ELF, SUN3_GDBPORT,
    # SUN3_PROMPT, SUN3_TIMEOUT, SUN3_EXTRA, SUN3_CMDS=';'-separated)
    SUN3_QEMU=build/qemu-system-m68k SUN3_ROM=.../3.60_v3.0.1_rom SUN3_KERNEL=/tmp/sun3-kbuild/vmlinux \
      SUN3_GDBPORT=11354 SUN3_EXTRA="-m 24M" SUN3_PROMPT="smolsh / > " \
      SUN3_CMDS="uname -a; cat /proc/cpuinfo" python3 sun3-tools/boot-linux.py /tmp/sun3-linux.log
    # -> sun3-tools/logs/linux-sun3-60.log

    # Linux/sun3 natively, no PROM image (-append -> BI_COMMAND_LINE, overridden by CONFIG_BOOTPARAM;
    # -initrd -> BI_RAMDISK below the top of RAM)
    qemu-system-m68k -M sun3-60 -m 24M -kernel /tmp/sun3-kbuild/vmlinux -display none -serial mon:stdio
    # -> sun3-tools/logs/linux-sun3-60-kernel.log (PROM Revision: QEMU)

    # Linux/sun3 from a boot disk through the genuine PROM (sun3-tools/bootloader/)
    make -C sun3-tools/bootloader CROSS=m68k-linux-gnu-        # bootblk.bin, 1.6 KB
    python3 sun3-tools/bootloader/mk-bootdisk.py --out /tmp/boot.img [--initrd F] [--append "..."] vmlinux
    qemu-system-m68k -M sun3-60 -m 24M -bios 3.60_v3.0.1_rom -display none -serial mon:stdio \
      -drive file=/tmp/boot.img,format=raw,if=scsi,bus=0,unit=0
    # -> auto-boot "Boot: sd(0,0,0)", "Linux/sun3 boot: ...", smolsh (logs/linux-sun3-60-bootdisk.log);
    #    at unit 1: `b sd(0,8,0)` at the > prompt (logs/linux-sun3-60-bootdisk-typed.log)

    # NetBSD 1.5.2 RAMDISK injection (SUN3_QEMU, SUN3_ROM, SUN3_KERNEL=netbsd.RAMDISK,
    # SUN3_GDBPORT, SUN3_TIMEOUT, SUN3_EXTRA); loads at physical 0x4000, `g 4000`
    SUN3_QEMU=... SUN3_ROM=... SUN3_KERNEL=.../netbsd.RAMDISK SUN3_GDBPORT=11353 \
      python3 sun3-tools/boot-netbsd-ramdisk.py /tmp/nb.log
    # -> welcome banner on ttya, then the open(2) bug loop (section 17); no log kept

    # NetBSD miniroot from disk (INSTALL kernel, auto-boots from sd0; writes to the image)
    zcat _sun3_assets/miniroot.gz > /tmp/nbmini.img
    qemu-system-m68k -M sun3-60 -bios .../3.60_v3.0.1_rom -display none -serial mon:stdio \
      -drive file=/tmp/nbmini.img,format=raw,if=scsi,bus=0,unit=0 [-nic user,model=lance]
    # -> sun3-tools/logs/netbsd-1.5.2-miniroot-le0-sun3-60.log (le0 attach, root on sd0a, # shell)

    # SunOS 4.1.1 MINIROOT (miniroot + 16 MiB swap disk from sun3-tools/install/mk-sun3-disks.py)
    qemu-system-m68k -M sun3-60 -bios .../3.60_v3.0.1_rom -display none -serial mon:stdio \
      -drive file=miniroot-sun3.img,format=raw,if=scsi,bus=0,unit=0
    # -> root on sd0a, dump on sd0b, `#` in 12 s

    # SunOS installed image: login, 12 commands incl. cc, sync; reboot, login again
    # (env SUN3_LOGIN_TIMEOUT 600, SUN3_CMD_TIMEOUT 300; --halt, --no-reboot, --cmds-file F.py)
    python3 sun3-tools/boot-sunos-installed.py /tmp/sunos.log -- qemu-system-m68k -M sun3-60 \
      -bios 3.60_v3.0.1_rom -display none -drive file=sun3-installed.img,format=raw,if=scsi,bus=0,unit=0
    # -> sun3-tools/logs/sunos-4.1.1-installed-sun3-60.log (cold boot to login 20 s, reboot 38 s)

    # Screen console (no GTK/SDL in this build: VNC + HMP screendump)
    qemu-system-m68k -M sun3-60,sunos-console=fb -bios .../3.60_v3.0.1_rom -m 16M \
      -drive file=sun3-installed-gfx.img,format=raw,if=scsi,bus=0,unit=0 \
      -vnc 127.0.0.1:12 -L <qemu-data> -monitor unix:/tmp/mon.sock,server,nowait
    python3 sun3-tools/hmp.py /tmp/mon.sock 'type:root\n' 'sleep:5' 'type:suntools\n'
    python3 sun3-tools/hmp.py /tmp/mon.sock 'mouse_move -280 -395' 'mouse_button 1' 'mouse_button 0'
    python3 sun3-tools/screendump-png.py /tmp/mon.sock NAME      # writes /tmp/m38/NAME.png + ink stats
    # HMP mouse_button is a bitmask: 1 = left, 2 = right, 4 = middle (2 opens the SunView root menu
    # and the OpenWindows Workspace menu). A halt on the screen console takes > 12 s: it is syncing.
    # -> sun3-60-prom-screen.png, sun3-60-sunos-fb-login.png, sun3-60-sunview-desktop.png,
    #    sun3-60-sunview-typing.png, sun3-60-openwin-desktop.png, sun3-60-openwin-typing.png

    # Networking (SunOS): 10.0.2.2 = slirp gateway, 10.0.2.3 = DNS
    qemu-system-m68k -M sun3-60 -m 16M -bios 3.60_v3.0.1_rom -display none -serial stdio \
      -drive file=<copy of sun3-installed-gfx.img>,format=raw,if=scsi,bus=0,unit=0 \
      -nic user,id=n0,model=lance[,hostfwd=tcp:127.0.0.1:12834-:23][,guestfwd=tcp:10.0.2.100:7-cmd:...] \
      [-object filter-dump,id=f,netdev=n0,file=x.pcap]
    # in the guest (persisted on the gfx image: hostname.le0, hosts, netmasks, defaultrouter + rc.local hook)
    hostname sun3-60
    ifconfig le0 10.0.2.15 netmask 255.255.255.0 up
    route add default 10.0.2.2 1
    ping 10.0.2.2 5; netstat -in; netstat -rn; arp -a
    # -> sun3-tools/logs/sunos-4.1.1-le0-network-sun3-60.log
    # guestfwd caveat: telnet to a plain `cat` echo loops at wire speed on option negotiation
    # (10 M packets in five minutes); use a one-shot echo command.

## 17. Known gaps / open bugs

* **NetBSD RAMDISK: one EFAULTing open(2) per shell cycle** (session 1, still present). init
  forks the small shell, `.sshrc` prints the welcome banner, then exactly one `open` fails with
  EFAULT (errno store at 0xE09754A: sys 5 errno 14; the readlink/access/wait4 errors per cycle
  are benign) and the shell is respawned, so the installer loops on its banner. The fault is a
  recurring transient at kernel VA 0xEC54000 (a namei pathname pool page, seg 0x762 pmeg 0x2B);
  usually `uvm_fault -> pmap_enter` resolves it, in this one case it fails into `pcb_onfault`.
  Also unexplained: the first ~4 characters of each process's first tty write burst arrive as
  NULs. Next steps: check `subr_pool.c`/`uvm_km.c` against the fault trail, `trap.c`'s
  fault-type derivation for instruction faults, control-space sub-word lanes. The INSTALL kernel
  from disk does not show it.
* **LTO-built smolsh SIGSEGVs on PATH lookup** (extraction session): the same static nolibc
  `-flto` binary runs on sun3x but on sun3-60 dies in the inlined getenv/try_search loop, always
  `pc=040013e4 addr=3a000000 a0=3a000000`, the saved a1 inconsistent with the instruction
  stream. Not page-size related (TARGET_PAGE_BITS 12 behaves the same), not BSS/brk, not the
  individual calls; without LTO it works. Suspects: bus-error frame / fc_ops translate /
  postincrement writeback on the 68020 + Sun-3 MMU path. Same smell as the EFAULT above and the
  old spurious SIGUSR1.
* No RARP/TFTP server behind slirp: the PROM's diskless boot only requests (7 broadcasts of
  ethertype 0x8035 in 150 s); `b le()` was not attempted.
* `-nic user` without `model=lance` silently has no LANCE (by design, section 13); QEMU prints
  no warning for the unused default NIC.
* Linux `sun3lance` is not in `sun3-tools/sun3-linux.config` (CONFIG_SUN3LANCE unset); not
  exercised.
* The MERR path (`pcnet_dma_error`) is modelled from the Am7990 datasheet; no guest was made to
  fault a DVMA transfer. Likewise SI_CSR_DMA_CONFLICT is never raised and the FIFO reads always
  empty.
* Hostname: `sun3-installed.img` (session 36) has no `hostname.le0` and prints `Amnesiac
  login:`; only the gfx image carries the network configuration and `/sbin/hostname` (session
  39). Hostname was otherwise set per session (`hostname sun3-60`).
* NetBSD GENERIC (bwtwo/kbd/ms drivers) was never booted on the screen: it needs a root file
  system it can find (`b sd()netbsd`); the INSTALL kernel uses the PROM console.
* Linux: wall clock runs fast; the busybox-era sporadic SIGUSR1 (section 15).
* Cosmetic: after `Exit` from OpenWindows one stale PROM cursor block stays above the shell
  prompt; filemgr's `install_filemgr` complaint is painted on the frame buffer by the console
  before the cmdtool takes `/dev/console` (genuine 4.1.1 behaviour).
* Fixed, do not re-chase: the SunOS idle stall after `dump on sd0b` (SSW rerun flags, session
  36); `kbd0: reset failed` (10 ms reply, session 37); the 268 px click offset (mouse sync,
  session 38); `si0: resetting scsi bus` on every command (DMA_IP rule, session 35).

## 18. Debug crutches

* `-d int` logs diag-LED writes, enable/segmap/pgmap traffic, interrupt-register writes, ICM7170
  register writes and MMU faults (`invalid pte va=... seg=... pmeg=...`, `write prot`); `-d mmu`
  logs every si register access and UDC start-chain (table address, rsel, DMA address, word
  count, CSR, FIFO count); `-d guest_errors` logs DVMA faults (dva, va, context) and si
  direction mismatches. All `qemu_log_mask`-gated, zero cost otherwise.
* gdb-multiarch on the gdbstub reads and writes kernel/user memory through the debug hook
  (current context, supervisor data), but register reads/writes are **byte-swapped** with this
  gdb -- sample the PC with the QEMU monitor (`info registers` over `-monitor unix:` / QMP
  `human-monitor-command`) instead, and breakpoints re-trap on plain `continue` (interpose
  `stepi`).
* Attach gdb from `-S`; attaching to a running guest hangs in this build. Python breakpoint
  commands that write and flush a file are the only way to keep trace output when gdb is killed
  (session 36's sleep/syscall/trap traces).
* The RAMDISK FFS can be parsed offline: fs base at file offset 0xB7984 of `netbsd.RAMDISK`, 400
  KiB, bsize 8192 / fsize 1024, ipg 128; `/sbin/init` is one crunched a.out (text 0x44000 at VA
  0x2000, data at 0x46000).
* SunOS symbols: `munix_sun3` text starts at file offset 512 (not 32) and its string table has
  to be located by consistency search; `stand/vmunix` from `sun3_kvm.tar.Z` is OMAGIC with text
  at offset 32 (`sun3x-tools/sunos-symbols.py`).
* The 4.1.1 tty silently drops canonical input lines over 256 characters; getty std.9600 sets
  even parity and the banner carries bit 7 on some bytes (the drivers strip it).


