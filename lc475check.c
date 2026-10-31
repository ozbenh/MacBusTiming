/*
 * LC475Check: looks at registers of the LC 475 family (PrimeTime and MEMCjr)
 * which nothing documents, for the Snow emulator.
 *
 * - what the old Turbo SCSI and DAFB addresses answer, under a bus error handler
 * - the RAM bank registers of MEMCjr. Bit 10 is only tried on an empty bank, so
 *   this shows if it's stored, not what it does to a bank with RAM.
 * - whether the VIA 2 flags of the 53C96 interrupt and the video interrupt
 *   follow their line or latch an edge. This makes the idle 53C96 raise an
 *   interrupt with an illegal command, with interrupts masked, and clears it.
 *
 * Nothing here should leave a trace once the program has quit, but restart the
 * machine afterwards anyway.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <Files.h>
#include <Gestalt.h>
#include <Memory.h>
#include <OSUtils.h>

#define VERSION "1"

#define CPU_ID 0x5FFFFFFCUL
#define MEMC 0x50F0E000UL
#define MEMC_HOLDING (MEMC + 0x7C)
#define VIA2_IFR 0x50F03A00UL
#define VIA2_PA 0x50F03E00UL
#define ESP_CMD 0x50F10030UL
#define ESP_STATUS 0x50F10040UL
#define ESP_INTR 0x50F10050UL
#define PRIMETIME_REGS 0x50F18000UL

#define IFR_CA1 0x02
#define IFR_CB2 0x08
#define PA_VIDEO 0x40

/* Not a command of the 53C96: answered with an illegal command interrupt */
#define ESP_ILLEGAL 0x7F

#define RAM_BANKS 5
#define BANK_FLAG 0x400

/* Set when the user chose to run on a volume that can't be written */
static bool noSaving;

static char outText[16384];
static size_t outLength;

static void out(const char *format, ...)
{
	char line[256];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);

	fputs(line, stdout);
	for (const char *c = line; *c && outLength < sizeof(outText); c++)
	{
		outText[outLength++] = (*c == '\n') ? '\r' : *c;
	}
}

static void SaveResults(void)
{
	static const unsigned char name[] = "\pLC475Check Results";
	short refNum;

	if (noSaving)
	{
		return;
	}
	(void)FSDelete(name, 0);
	if (Create(name, 0, 'ttxt', 'TEXT') != noErr || FSOpen(name, 0, &refNum) != noErr)
	{
		printf("Could not create the results file\n");
		return;
	}
	long count = outLength;
	(void)FSWrite(refNum, &count, outText);
	(void)FSClose(refNum);
	(void)FlushVol(NULL, 0);
}

/* A locked volume can let a file open, and only fail the write */
static bool DiskWritable(void)
{
	static const unsigned char name[] = "\pLC475Check Test";
	short refNum;
	long count = 1;
	char byte = 0;
	OSErr err;

	(void)FSDelete(name, 0);
	if (Create(name, 0, 'ttxt', 'TEXT') != noErr || FSOpen(name, 0, &refNum) != noErr)
	{
		return false;
	}
	err = FSWrite(refNum, &count, &byte);
	(void)FSClose(refNum);
	(void)FSDelete(name, 0);
	(void)FlushVol(NULL, 0);
	return err == noErr;
}

static bool AskToRunWithoutSaving(void)
{
	printf("!!! THIS DISK CAN'T BE WRITTEN TO !!!\n"
		"The results can't be saved, and if the Mac hangs they are lost.\n"
		"Better to quit now and copy this program to a disk that isn't\n"
		"locked.\n\n"
		"Run it anyway? Type Y and Return to run, or just Return\n"
		"to quit: ");
	fflush(stdout);
	int answer = getchar();
	/* The Return is still to be read, and would answer the last prompt */
	for (int c = answer; c != '\n' && c != '\r' && c != EOF; c = getchar())
	{
	}
	if (answer != 'y' && answer != 'Y')
	{
		return false;
	}
	noSaving = true;
	return true;
}

static long GestaltOr(OSType selector, long fallback)
{
	long value;
	return Gestalt(selector, &value) == noErr ? value : fallback;
}

/*
 * Long read which survives a bus error. The handler drops the frame of the
 * 68040, whatever its format, and goes on at the label with a clean one.
 */
extern uint32_t ProbeRead(uint32_t addr, uint32_t *ok);
extern void ProbeHandler(void);

__asm__(
	".text\n"
	".globl ProbeRead\n"
	"ProbeRead:\n"
	"	movea.l 4(%sp),%a0\n"
	"	movea.l 8(%sp),%a1\n"
	"	clr.l (%a1)\n"
	"	moveq #0,%d0\n"
	"	move.l (%a0),%d0\n"
	"	nop\n"
	"	moveq #1,%d1\n"
	"	move.l %d1,(%a1)\n"
	"ProbeDone:\n"
	"	rts\n"
	".globl ProbeHandler\n"
	"ProbeHandler:\n"
	"	move.w (%sp),%d1\n"
	"	move.b 6(%sp),%d0\n"
	"	lsr.b #4,%d0\n"
	"	lea 8(%sp),%a0\n"
	"	cmpi.b #7,%d0\n"
	"	bne.s 1f\n"
	"	lea 60(%sp),%a0\n"
	"1:\n"
	"	cmpi.b #10,%d0\n"
	"	bne.s 2f\n"
	"	lea 32(%sp),%a0\n"
	"2:\n"
	"	cmpi.b #11,%d0\n"
	"	bne.s 3f\n"
	"	lea 92(%sp),%a0\n"
	"3:\n"
	"	movea.l %a0,%sp\n"
	"	moveq #0,%d0\n"
	"	clr.w -(%sp)\n"
	"	pea ProbeDone(%pc)\n"
	"	move.w %d1,-(%sp)\n"
	"	rte\n");

static inline uint16_t DisableIRQ(void)
{
	uint16_t sr;
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc", "memory");
	return sr;
}

static inline void RestoreIRQ(uint16_t sr)
{
	__asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "cc", "memory");
}

static inline uint32_t GetVBR(void)
{
	/* movec vbr,d0 */
	register uint32_t v __asm__("d0");
	__asm__ volatile(".word 0x4E7A,0x0801" : "=r"(v));
	return v;
}

static volatile uint32_t *berrVector;
static uint32_t berrSaved;

static void HandlerInstall(void)
{
	berrVector = (volatile uint32_t *)(GetVBR() + 8);
	berrSaved = *berrVector;
	*berrVector = (uint32_t)ProbeHandler;
}

static void HandlerRemove(void)
{
	*berrVector = berrSaved;
}

static inline uint8_t r8(uint32_t addr)
{
	return *(volatile uint8_t *)addr;
}

static inline void w8(uint32_t addr, uint8_t val)
{
	*(volatile uint8_t *)addr = val;
	__asm__ volatile("nop");
}

/* All of these with interrupts masked and the handler in place */
static void ShowProbe(const char *what, uint32_t addr)
{
	uint32_t ok;
	uint32_t val = ProbeRead(addr, &ok);

	if (ok)
	{
		out("  %-26s %08lX: %08lX\n", what, (unsigned long)addr, (unsigned long)val);
	}
	else
	{
		out("  %-26s %08lX: bus error\n", what, (unsigned long)addr);
	}
}

/* 12 bits: 6 at the register, then 6 in the holding register */
static bool BankRead(int bank, uint16_t *val)
{
	uint32_t ok;
	uint32_t low = ProbeRead(MEMC + 4 + 4 * bank, &ok);
	if (!ok)
	{
		return false;
	}
	uint32_t high = ProbeRead(MEMC_HOLDING, &ok);
	if (!ok)
	{
		return false;
	}
	*val = ((high & 0x3F) << 6) | (low & 0x3F);
	return true;
}

static void BankWrite(int bank, uint16_t val)
{
	*(volatile uint32_t *)MEMC_HOLDING = (val >> 6) & 0x3F;
	__asm__ volatile("nop");
	*(volatile uint32_t *)(MEMC + 4 + 4 * bank) = val & 0x3F;
	__asm__ volatile("nop");
}

static void CheckAddresses(void)
{
	char name[32];

	out("\nAddresses (only reads):\n");
	ShowProbe("DAFB Turbo SCSI", 0xF9800024UL);
	ShowProbe("DAFB Turbo SCSI, again", 0xF9800024UL);
	ShowProbe("DAFB second Turbo SCSI", 0xF9800028UL);
	ShowProbe("Video base high", 0xF9800000UL);
	ShowProbe("Video stride", 0xF9800008UL);
	ShowProbe("Video test (version)", 0xF980002CUL);
	ShowProbe("IOSB ID address", 0x5FFF0000UL);
	ShowProbe("Past the I/O space", 0x54000000UL);

	out("\nMEMCjr registers:\n");
	for (int i = 0; i < 0x80; i += 4)
	{
		snprintf(name, sizeof(name), "MEMCjr +$%02X", i);
		ShowProbe(name, MEMC + i);
	}

	out("\nPrimeTime registers:\n");
	for (int i = 0; i < 8; i++)
	{
		snprintf(name, sizeof(name), "PrimeTime reg %d", i);
		ShowProbe(name, PRIMETIME_REGS + 0x100 * i);
	}
}

static void CheckBanks(void)
{
	uint16_t bank[RAM_BANKS];
	uint32_t ramMB = ((uint32_t)GestaltOr('ram ', 0x7FFFFFFF) + 0xFFFFF) >> 20;

	out("\nRAM banks (%lu MB by Gestalt), base in MB in bits 9-0:\n", (unsigned long)ramMB);
	for (int i = 0; i < RAM_BANKS; i++)
	{
		if (!BankRead(i, &bank[i]))
		{
			out("  bank %d: bus error\n", i);
			return;
		}
		out("  bank %d: $%03X, base %d MB, bit 10 %s\n", i, bank[i], bank[i] & 0x3FF,
			(bank[i] & BANK_FLAG) ? "set" : "clear");
	}

	/* The last bank is empty if it starts at the top of RAM */
	int b = RAM_BANKS - 1;
	if ((bank[b] & 0x3FF) < ramMB)
	{
		out("  bank %d has RAM: not writing to it\n", b);
		return;
	}
	uint16_t val = 0;
	BankWrite(b, bank[b] | BANK_FLAG);
	bool ok = BankRead(b, &val);
	BankWrite(b, bank[b]);
	uint16_t after = 0;
	bool ok2 = BankRead(b, &after);
	out("  bank %d, empty, with bit 10 written: %s $%03X, restored: %s $%03X\n", b,
		ok ? "reads" : "error", val, ok2 ? "reads" : "error", after);
}

/* Raises the interrupt. Returns false if nothing happened or it wasn't idle. */
static bool EspRaise(void)
{
	w8(ESP_CMD, ESP_ILLEGAL);
	for (long i = 0; i < 100000; i++)
	{
		if (r8(ESP_STATUS) & 0x80)
		{
			return true;
		}
	}
	return false;
}

static void CheckScsiFlag(void)
{
	out("\nVIA 2 flag of the 53C96 interrupt (IFR bit 3):\n");
	if (r8(ESP_STATUS) & 0x80)
	{
		out("  53C96 has an interrupt pending: skipped\n");
		return;
	}
	uint8_t idle = r8(VIA2_IFR);
	if (!EspRaise())
	{
		out("  the illegal command raised no interrupt: skipped (IFR %02X)\n", idle);
		w8(ESP_CMD, 0x01);
		return;
	}
	uint8_t raised = r8(VIA2_IFR);
	w8(VIA2_IFR, IFR_CB2);
	uint8_t cleared = r8(VIA2_IFR);
	uint8_t intr = r8(ESP_INTR);
	uint8_t afterIntr = r8(VIA2_IFR);
	w8(VIA2_IFR, IFR_CB2);

	bool again = EspRaise();
	uint8_t raised2 = r8(VIA2_IFR);
	(void)r8(ESP_INTR);
	uint8_t afterIntr2 = r8(VIA2_IFR);
	w8(VIA2_IFR, IFR_CB2);
	uint8_t end = r8(VIA2_IFR);
	w8(ESP_CMD, 0x01);

	out("  idle %02X, raised %02X, IFR written %02X, 53C96 interrupt register %02X, then %02X\n",
		idle, raised, cleared, intr, afterIntr);
	out("  raised again%s %02X, 53C96 interrupt register read %02X, IFR written %02X\n",
		again ? "" : " (no)", raised2, afterIntr2, end);
	out("  Stays set when the IFR is written and drops with the 53C96: follows the line.\n");
	out("  Drops when the IFR is written and stays after the 53C96: latches an edge.\n");
}

static void CheckVideoFlag(void)
{
	out("\nVIA 2 flag of the video interrupt (IFR bit 1, port A bit 6):\n");
	uint8_t pa = 0;
	bool asserted = false;
	for (long i = 0; i < 3000000; i++)
	{
		pa = r8(VIA2_PA);
		if (!(pa & PA_VIDEO))
		{
			asserted = true;
			break;
		}
	}
	if (!asserted)
	{
		out("  port A bit 6 never went low (port A %02X): skipped\n", pa);
		return;
	}
	uint8_t raised = r8(VIA2_IFR);
	w8(VIA2_IFR, IFR_CA1);
	uint8_t cleared = r8(VIA2_IFR);
	uint8_t paAfter = r8(VIA2_PA);
	out("  raised %02X (port A %02X), IFR written %02X (port A %02X)\n", raised, pa, cleared, paAfter);
	out("  Stays set while port A bit 6 is low: follows the line.\n");
}

int main(void)
{
	long machine = 0;
	long cpu = 0;

	if (!DiskWritable() && !AskToRunWithoutSaving())
	{
		return 0;
	}

	(void)Gestalt('mach', &machine);
	(void)Gestalt('proc', &cpu);
	out("LC475Check v%s\nGestalt machine %ld, processor %ld\n", VERSION, machine, cpu);

	if (GestaltOr('vm  ', 0) & 1)
	{
		out("Virtual memory is on. Turn it off in the Memory control panel and restart.\n");
		goto done;
	}
	/* Bit 0: the addresses here don't exist in the 24-bit map */
	if (!(GestaltOr('addr', 0) & 1))
	{
		out("24-bit addressing is on. Turn on 32-bit addressing in the Memory control panel\n"
			"and restart.\n");
		goto done;
	}

	/* LC 475 and its 33 MHz strapping, LC 575, Quadra 605 and its strapping */
	if (cpu != 5 || (machine != 89 && machine != 90 && machine != 92 && machine != 94 && machine != 95))
	{
		out("Not the LC 475 family: nothing was tried.\n");
		goto done;
	}

	uint16_t sr = DisableIRQ();
	HandlerInstall();

	/* The ROM's MMU tables may not map this, which is a bus error here too */
	ShowProbe("ID", CPU_ID);
	CheckAddresses();
	CheckBanks();
	CheckScsiFlag();
	CheckVideoFlag();

	HandlerRemove();
	RestoreIRQ(sr);

done:
	SaveResults();
	printf(noSaving ? "\nNot saved. Copy this down, then press Return to quit.\n"
			: "\nSaved to \"LC475Check Results\". Press Return to quit.\n");
	getchar();
	return 0;
}
