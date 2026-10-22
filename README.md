# BusTiming

BusTiming measures how long the processor of a classic 68k Macintosh
takes to read and write the registers of the chips on the logic board:
the VIAs, the SCC (serial), the IWM or SWIM (floppy), the SCSI chip and
the Apple Sound Chip. It compares them with reads and writes of RAM and
ROM.

The results help the [Snow](https://github.com/twvd/snow) emulator get the
speed of these accesses right. The program changes nothing on your Mac:
it only reads registers, and only writes back values that a register
already holds.

## Running it

1. Restart the Mac, and don't start any other application.
2. Turn **virtual memory off** (Memory control panel), then restart. With
   virtual memory on, BusTiming stops with a message.
3. Leave the floppy drive empty.
4. Run BusTiming, from the 800K disk image `BusTiming.dsk` or from the
   MacBinary file `BusTiming.bin`.
5. **Don't touch the mouse or the keyboard** until it says it is done. It
   takes about half a minute. It first waits 5 seconds to measure the VIA
   clock against the clock of the Mac.
6. When it says "Saved", press Return to quit.

The mouse pointer may stutter while it runs: the program masks interrupts
for a few milliseconds at a time.

It needs Gestalt (System 6.0.4 or later) and is meant for any 68000 to
68040 Mac; so far it has only run on emulators, under System 7.1 and
7.5.5. Under System 6 it can't check the VIA clock (that needs the System
7 Time Manager). It skips what it doesn't know how to measure safely on a
given model.

## What to send back

- The file **"BusTiming Results"**, which the program writes next to
  itself. It is a text file.
- The model of the Mac, and anything that changes its speed or its bus:
  accelerator card, cache card (IIci, IIsi, SE/30 PDS), CPU or clock
  upgrades, the amount and kind of RAM.
- The settings of the Cache Switch or Memory control panels (68030/68040
  caches, 24/32-bit addressing).

## Building

BusTiming is built with [Retro68](https://github.com/autc04/Retro68):

    export RETRO68=/path/to/Retro68-build/toolchain
    make

It uses the RetroConsole library. With the Retro68 of April 2025 and
later, a console application is bigger than the 1 MB of the default SIZE
resource and crashes under System 7, unless RetroConsole is built without
`std::istringstream` (Retro68 Console.cc, parseArguments).

How the measurements are made and what the numbers mean is in
[README-method.md](README-method.md).
