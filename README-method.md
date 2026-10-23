# BusTiming: method and results

This is for us: how BusTiming measures, what each number means, why each
register was chosen, and how the program does on the emulators.

## Stopwatch

VIA1 timer 2 counts the VIA clock (783.36 kHz on all these Macs: C16M / 20,
or the 68000's 7.8336 MHz / 10). The Time Manager owns it and has its
interrupt enabled, so BusTiming only borrows it, in short **windows**:

1. Mask interrupts (SR = $2700).
2. BorrowTimer: disable the T2 interrupt in IER, save whether its flag is
   set, save the count.
3. Run the measurement: load T2 with $FFFF, run the code, read T2.
4. ReturnTimer: reload T2 with the saved count **less the ticks we used**
   (or 1 if the flag was set or the count has run out), so the Time
   Manager gets its interrupt about when it expected it; enable the T2
   interrupt again if it was enabled.
5. Unmask interrupts.

One window holds one timed run (plus a warm-up run on CPUs with caches),
1.5 to 3 ms with interrupts masked. Interrupts are served between windows,
so the mouse and the Time Manager keep going. ASCTester threw the Time
Manager's interrupt away and froze the mouse of a real IIci; this does not.

The timer is read high, low, high. If the two high bytes differ, the low
byte tells which one goes with it (below $80: the first, else the second).

### VBL sync on the 68000 compacts

On the Plus, SE and Classic (and the 512Ke) the video and sound take RAM
cycles from the CPU outside the blanking periods, so code running from RAM
is slowed down at random. There, and only there, a window starts at the
beginning of the vertical blanking, where the CPU has the RAM to itself
for 1.26 ms: wait for Ticks to change (the VBL interrupt was served), wait
about 12 ms with interrupts enabled (a busy loop calibrated against T2),
mask interrupts and poll the CA1 flag in IFR. CA1 is the VBL interrupt on
these machines (Guide to the Macintosh Family Hardware). The flag is left
set, so the VBL interrupt is served late, after the window. A run there is
kept under 600 VIA ticks (0.77 ms). The header says how many windows
missed the VBL ("misses"): they ran unsynchronised.

No other machine uses CA1: on later machines CA1 is a 60.15 Hz tick
generated from the system clock (Mac II, Quadra 700...) or something else,
and their RAM is not shared with the video.

### VIA clock check

The VIA clock is checked against the real time clock, which has its own
32.768 kHz crystal: over 5 seconds of the RTC (the Time global, $20C),
Microseconds() counts VIA ticks converted at the nominal 783.36 kHz, so
`783360 x microseconds / 5000000` is the real VIA clock. This needs the
extended Time Manager (System 7); otherwise the header says "not
measured". Ticks per second is only indicative (±1 tick in 5 s). The
emulators give 783.36 kHz within 15 ppm.

## Timed code

The code is generated at run time, then the instruction cache is flushed:

    move.b d7,(a2)          ; T2 low latch = $FF
    move.b d7,(a3)          ; T2 high = $FF: counts from $FFFF
    loop:
      N times:
        <access>            ; move.b (a0),d0 / move.b d1,(a0) / move.b d1,d0
        k times: move.l d1,d5   ; filler
      dbra d6,loop          ; L times
    move.b (a3),d2          ; T2 high
    move.b (a2),d3          ; T2 low
    move.b (a3),d4          ; T2 high
    rts

- **N** (accesses per block) is the largest that keeps the whole code in
  the instruction cache, at most 64: 240 bytes on the 68020 and 68030 (256
  byte I-cache; the code is aligned on 16 bytes), 2 KB on the 68040. On the
  68000 N is 64.
- **L** (loops) is chosen from a first run with L = 1 so that a run takes
  about 1200 VIA ticks (1.5 ms), 600 on the compacts.
- **k** (fillers per access) is swept: 0 1 2 3 4 5 6 7 8 10 12 ... 256, until
  the fillers take 4.5 us per access.
- The runs are repeated (7 times, 3 on the compacts) and the fastest is
  kept ("raw"); the median is printed too ("med"), and the spread of the
  runs in ticks ("spr").
- **t0**, the cost of the timer start and read with an empty block, is
  measured once and taken off every run.
- On the 68020 and later every window runs the code twice and times the
  second run, so it runs from a warm I-cache.
- On the 68030 and 68040 the **data cache is off** during a window (CACR
  ED, or DE after CPUSHA DC on the 68040), so RAM and ROM reads go to the
  bus and the device accesses reach it whatever the MMU says about their
  cacheability. The data cache is cleared (68030) when it is turned back
  on. The I-cache stays as the system set it (the header shows CACR).

The access instruction is the same size as the register-only one (2
bytes), so the loops differ only by the bus cycle.

## The numbers

All times are per access, from the fastest run.

- **raw_ns**: time per access, in ns: `(ticks - t0) / (N x L) x 1276.55`.
- **gap_ns**: the time the k fillers take: register-only at k less
  register-only at k = 0. This is the CPU work between two accesses beyond
  the back to back case. It is measured, not computed from instruction
  timings.
- **bus_ns**, **bus_clk**: raw less register-only at the same k. This is
  what the bus cycle costs over an instruction that doesn't use the bus:
  the bus cycle, its wait states, and any synchronisation or recovery wait.
  In CPU clocks with the clock from Gestalt ('pclk') or, when Gestalt
  doesn't have it, from a table (then marked "assumed").
- **ram_ns**: raw less RAM read at the same k.

The wait states of a device are `bus_clk - base cycle` where the base
cycle is 4 clocks on the 68000 and 3 on the 68020/68030 (asynchronous
cycle). On the 68040 the measured RAM write is 2 bus clocks.

The **Summary** has, per target:

- **b2b_ns**, **b2b_via**: back to back (k = 0), in ns and in VIA clock
  cycles. b2b_via tells the synchronous devices apart: 1.00 on the Mac II,
  LC and IIci VIAs (a VIA access starts on a VIA clock edge and the CPU
  is released half way through the VIA cycle, so back to back accesses
  take one VIA cycle each), 2.00 on the compact Macs (a 68000 VPA cycle
  ends on the E clock edge, and the prefetch between two accesses makes
  the second one miss the next edge).
- **b2b_bus**, **b2b_clk**: bus time back to back.
- **floor_ns**, **floor_clk**: the lowest bus time over the sweep: the
  access itself once no recovery or synchronisation wait is left.
- **knee_gap_ns**: the filler time from which the bus time is within 1/4
  clock of the floor. For a device with a recovery time, the recovery
  (from the end of an access to the start of the next) is about
  `b2b_bus - floor` and shows as the knee. Example, Mac II SCC: b2b 37
  clocks, floor 3, knee 2.1 us: 34 clocks of recovery after a 3 clock
  access.

For a device synchronous to the VIA clock, the bus time is a saw tooth
over the sweep (it falls as the fillers eat into the wait for the next
edge, then jumps by a VIA cycle), so floor and knee only sample it; read
the table.

### Caveats

- The dbra adds a few clocks once per block. For a synchronous device
  this can push one access in N past a VIA edge: on the IIci the VIA raw
  time is 1 VIA cycle at k = 0 to 4, then rises in small steps (k = 5 to
  8: one access in 13 to 20 takes two cycles) before it is 2 cycles for
  all accesses. The smaller N at large k makes this worse.
- On the 68020/68030 the bus controller takes a write and lets the
  sequencer go on with the next instructions, so fillers overlap a write
  but not a read. A write and a read of the same device can differ in raw
  time for that reason only.
- Reading T2 is a VIA access itself: its synchronisation varies with what
  ran before it by up to one tick, which is at most 1/(N x L) tick per
  access (under 0.01 tick at k = 0).
- On real hardware the RAM refresh, the video (compacts outside the VBL,
  and the Plus's sound/disk-speed buffer reads one per line even in the
  VBL) and NuBus video cards on the bus can slow some runs: the fastest run
  is kept, the median shows how often it happens.

## Registers, and why they are safe

Interrupts are masked during every access, so no driver is half way
through a sequence of accesses (SCC pointer, IWM state).

| Device | Access | Why it is safe |
|---|---|---|
| VIA1 | DDRB ($0400) read, IER ($1C00) read, DDRB written back with the value read | No side effect. Not ORA/ORB (handshake), T1/T2 low (clear flags), SR (shift). |
| VIA2 / pseudo-VIA | IER at +$1C13 | A real VIA2 decodes A9-A12 ($1C00: IER), the RBV/V8/Sonora/IOSB pseudo-VIAs the low bits ($13: IER). ASCTester's VIA2 mirror test showed it works on both. |
| SCC | Channel A control (+2) read: RR0; write 0: WR0 null command | With the pointer at 0 (no driver half way) a control read is RR0 and leaves it at 0. On the compacts reads are at $9FFFFA (A21 = 0: /RD) and writes at $BFFFFB (odd: /LDS is /WR), byte accesses only (both strobes at once reset the SCC). |
| IWM / SWIM | See below | |
| 5380 | Register 4 (Current SCSI Bus Status, base+$40) read; register 2 (Mode) written back | Never register 7 (Reset Parity/Interrupt). The Mode register is plain read/write. The write test only runs where the chip is known to be a 5380 (68000 to 68030, not the PowerBook 150), since register 2 is the FIFO of a 53C96. |
| 53C96 | Register 4 (Status, base+$40) read | Never register 5 (Interrupt: reading it clears the interrupt and the status). Only at a known base: $50F0F000 on the Quadra 700, $50F10000 on the IOSB/PrimeTime machines (Centris/Quadra 610/650/800, LC 475/575, Quadra 605). The pseudo-DMA ports ($50F10000 on the Quadra 700, $50F10100 on the IOSB) are never touched. Other 68040 Macs: skipped. |
| ASC | Version ($800) read; $806 written back on the original ASC (version $00) only | $800 is read only. $806 (volume) reads back what was written on the original ASC. |
| RAM | A buffer of the program | |
| ROM | ROMBase + $100 | |

**Compacts**: the low memory SCSIBase ($C00) is not the 5380 on the Plus
and SE (it reads $00049610 and $005FF000 in Snow), so the 5380 is taken at
its fixed address: reads at $580000 + reg x 16, writes at $580001 + reg x
16 (Guide to the Macintosh Family Hardware). The other compact addresses
come from low memory and are checked against the known ones.

**Other machines**: a device pointer from low memory is only used if it
points to I/O space ($F00000-$FFFFFF in 24-bit terms, which covers
$50F00000 and up). AddrMapFlags ($DD0) bits 11, 12 and 13 (VIA2, ASC,
RBV) say which devices exist, but the $0178 ROM (Mac II, IIx, IIcx, SE/30)
and the Portable ROM leave it at $FFFFFFFF: the Mac II family then gets a
VIA2 and an ASC, the Portable and the PowerBook 100 an ASC.

**IOP machines** (IIfx, Quadra 900/950): the SCC and the SWIM are behind
IOPs, and SCSI is not tried for now. The Duos have no internal floppy and
are not probed for one.

### IWM and SWIM

In IWM mode every access sets or clears a state line chosen by the
address ($200 apart: phases, motor, drive select, Q6, Q7). In ISM mode
(SWIM, after the floppy driver switched it for a 1.4 MB disk, or for good
on some machines) the same addresses are registers, and some reads have
side effects (data and mark pop the FIFO, error clears, parameter
advances). BusTiming only uses accesses that are harmless in both modes:

    $1C00   IWM: Q7L          ISM: read mode register
    $1A00   IWM: Q6H          ISM: read setup register
    $1C00   IWM: Q7L (status) ISM: read mode register    <- timed access
    $1800   IWM: Q6L          ISM: read phases register  (after)

In IWM mode this reads the status register (Q6 high, Q7 low) and leaves
Q6 and Q7 low, the idle state. The probe at the start reads the same
sequence once: the ISM mode register has bit 6 set and reads the same
twice; the IWM status register has bit 6 clear and reads the same at
$1A00 and $1C00. Anything else: not measured ("unknown"). If the drive is
enabled (IWM status bit 5) or the ISM motor, action or write bits are set,
it waits up to 10 s for the floppy driver to turn the motor off, then
gives up ("busy"). The target is named "IWM status read" or "ISM mode
read" after the mode found.

## Results on the emulators

The full results files are in [results/](results/).

`bt_report.py <file>...` turns results files into CPU clocks, wait states,
recovery times and VIA synchronisation in words; `bt_report.py -c
<file>...` puts several of them side by side in one table.

Built from Snow lc-family 5a5749d2 (origin/master + the LC series, VIA
cycles releasing the CPU half way through) and q700 c4202b61 for the
Quadra 700, System 7.1; MAME e9972422ce0 (a subset
build) with System 7.1 (Quadra 700, LC III) or 7.5.5 (Quadra 800,
Centris 650, LC 475). Bus time back to back and floor, in CPU clocks.

| Machine | RAM rd | ROM rd | VIA b2b | SCC b2b / floor | IWM/SWIM | SCSI | ASC rd / wr |
|---|---|---|---|---|---|---|---|
| Snow Plus, SE (7.8336 MHz) | 4 | 4 | 2.00 VIA cycles | 4 / 4 | 4 | 4 | - |
| Snow Portable (15.67 MHz) | 5 | 6 | 1.01 VIA cycles | 26.8 (32 clk period) / 4 | 4 | 4 | 4 / 4 |
| Snow Mac II (15.67 MHz) | 5 | 5 | 1.00 VIA cycle | 34.9 (37 clk period) / 3 | 4 | 4 | 5 / 4 |
| Snow IIx | 4 | 4 | 1.00 VIA cycle | 34.9 / 3 | 4 | 4 | 5 / 4 |
| Snow LC (15.67 MHz) | 4 | 5 | 1.00 VIA cycle | 4 / 4 | 4 | 4 | 4 |
| Snow Quadra 700 (25 MHz) | 2 | 2 | 2 clk | 2 | 13 | 2 | 2 |
| MAME Quadra 700 | 4 (wr 2) | 4 | 1.00 VIA cycle | 1.00 VIA cycle | 9 | 7 | 4 |
| MAME Centris 650, LC 475 | 4 (wr 2) | 4 | 1.00 VIA cycle | 1.00 VIA cycle | 9 (ISM) | 7 | 4 |
| MAME Quadra 800 (33 MHz) | 4 (wr 2) | 4 | 1.00 VIA cycle | 1.00 VIA cycle | 9 (ISM) | 7 | 4 |
| MAME LC III (25 MHz) | 3 (wr 2) | 3 | 1.00 VIA cycle | 3 (wr 2) | 8 (ISM) | 3 (wr 2) | 3 |

Snow, what its bus code models, and what the program found:

- Mac II (macii/bus.rs): RAM and ROM 2 wait states on the 68020 (5 clocks),
  1 on the 68030 (4); IWM and SCSI +1 (4); ASC read +2 (5), write +1 (4);
  VIA cycles start on a VIA clock edge and last a VIA cycle, the CPU
  being released half way through it (1 VIA cycle back to back, floor
  half a VIA cycle); the SCC is held off for 2.2 us (34 clocks) after a 3
  clock access (37 clock period). All recovered.
- Plus/SE (compact/bus.rs): E clock synchronous VIA (VPA cycle from the E
  edge, 10 clocks, as in the MC68000 User's Manual, Appendix B: VPA
  recognised up to 3 clocks after E falls ends with the next fall of E,
  later than that it waits one more E cycle), no wait states elsewhere. Recovered: VIA floor 10.0
  clocks, 2 E cycles back to back; everything else 4 clocks.
- LC (maclc/bus.rs): I/O 1 wait state (4 clocks), RAM 1 (4), ROM 2 (5); a
  VIA cycle as on the Mac II (1 VIA cycle back to back). Recovered.
- Portable (portable/bus.rs): RAM 1 and ROM 2 wait states, SCC recovery
  1.8 us after a 4 clock access (32 clock period); VIA cycle as on the
  Mac II (the GLU makes the VIA clock and chip select, not the 68000's E
  clock). Recovered.
- Quadra 700 (q700 branch): only the SWIM has wait states (2 + 11 = 13
  clocks); the VIA, SCC, SCSI and ASC accesses are plain 2 clock cycles.
  Recovered. MAME has VIA and SCC accesses synchronous to the VIA clock
  (1 VIA cycle back to back), 7 clocks for the 53C96, 9 for the SWIM, 4
  for RAM reads: real hardware will tell.

## Results on real hardware

The full results files are in [results/](results/), named `hw-*.txt`.
Bus time back to back and floor, in CPU clocks.

| Machine | RAM rd / wr | ROM rd | VIA1 b2b / floor | VIA2 | SCC rd b2b / floor | SCSI rd / wr | ASC rd / wr |
|---|---|---|---|---|---|---|---|
| Macintosh IIci (25 MHz 68030, System 7.1) | 3 / 3 | 3 | 1.00 VIA cycle / 15.9 (half a VIA cycle) | 6 (RBV) | 11 / 8 | 6 / 4 | 9 / 7 |

What the IIci shows:

- VIA1 is synchronous with the VIA clock: one access per VIA cycle back to
  back, reads and writes, and an isolated read takes half a VIA cycle
  (636.8 ns, the period being 1276.4 ns). The access starts on a VIA clock
  edge and the CPU is released half way through the VIA cycle, once the
  data from the VIA is latched; held to the end of the cycle, it would miss
  the next edge and take two VIA cycles per access. The sweep is the saw
  tooth of a half cycle: one VIA cycle up to about 650 ns of fillers, two
  from 820 ns. Snow's Mac II, LC and Portable model this.
- VIA2 is the RBV, a flat 6 clocks with no VIA clock synchronisation.
- The other devices have flat bus times over the sweep (within 4 ns), so
  they are timed in CPU clocks with nothing slower behind them: RAM and ROM
  3 clocks (1 wait state), 5380 6, ASC 9; the SCC 8 with a recovery of
  about 160 ns between two accesses.
- Writes overlap the fillers (the 68030 takes the write and goes on), so
  their floor is about 0: with enough fillers a write to the VIA, SCC,
  5380 or ASC costs nothing more than an instruction without one.
- The IWM/SWIM probe read FF FF 97 FF, which it didn't recognise: the
  floppy controller was not measured.

This says nothing about the GLUE of the Mac II, IIx and IIcx, or the V8
of the LC: those need their own runs.
