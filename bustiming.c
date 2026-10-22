/*
 * BusTiming: measures how long CPU accesses to the I/O devices of a
 * Macintosh take, against RAM, ROM and a register-only instruction.
 *
 * VIA1 timer 2 is the stopwatch. It belongs to the Time Manager, so it is
 * only borrowed with interrupts masked, one short window at a time, and
 * given back with the time used taken off its count.
 *
 * The timed code is generated at run time: the timer start, a block of N
 * copies of the access each followed by k filler instructions, looped L
 * times, and the timer read. See README-method.md.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <Gestalt.h>
#include <Files.h>
#include <Memory.h>
#include <OSUtils.h>
#include <Traps.h>

#define VERSION "1"

/* Low memory globals */
#define LM_TICKS		0x16A
#define LM_VIA			0x1D4
#define LM_SCCRD		0x1D8
#define LM_SCCWR		0x1DC
#define LM_IWM			0x1E0
#define LM_TIME			0x20C
#define LM_ROMBASE		0x2AE
#define LM_SCSIBASE		0xC00
#define LM_ASCBASE		0xCC0
#define LM_VIA2			0xCEC
#define LM_ADDRMAPFLAGS	0xDD0

/* VIA registers, $200 apart */
#define VIA_DDRB	0x0400
#define VIA_T2CL	0x1000
#define VIA_T2CH	0x1200
#define VIA_IFR		0x1A00
#define VIA_IER		0x1C00
#define VIA_IRQ_T2	0x20
#define VIA_IRQ_CA1	0x02

/* VIA clock of all these Macs: C16M / 20, or the 68000's clock / 10 */
#define VIA_HZ		783360

/* Generated instructions */
#define OP_START_LO	0x1487	/* move.b d7,(a2) */
#define OP_START_HI	0x1687	/* move.b d7,(a3) */
#define OP_READ		0x1010	/* move.b (a0),d0 */
#define OP_WRITE	0x1081	/* move.b d1,(a0) */
#define OP_REG		0x1001	/* move.b d1,d0 */
#define OP_FILL		0x2A01	/* move.l d1,d5 */
#define OP_DBRA_D6	0x51CE	/* dbra d6,<disp> */
#define OP_STOP_1	0x1413	/* move.b (a3),d2 */
#define OP_STOP_2	0x1612	/* move.b (a2),d3 */
#define OP_STOP_3	0x1813	/* move.b (a3),d4 */
#define OP_RTS		0x4E75
/* Bytes around the block: start, dbra, stop, rts */
#define WRAPPER_BYTES	16

/* Gestalt machine types used here */
enum
{
	MACH_512KE = 3, MACH_PLUS = 4, MACH_SE = 5, MACH_II = 6, MACH_IIX = 7,
	MACH_IICX = 8, MACH_SE30 = 9, MACH_PORTABLE = 10, MACH_IICI = 11,
	MACH_IIFX = 13, MACH_CLASSIC = 17, MACH_IISI = 18, MACH_LC = 19,
	MACH_Q900 = 20, MACH_PB170 = 21, MACH_Q700 = 22, MACH_CLASSICII = 23,
	MACH_PB100 = 24, MACH_PB140 = 25, MACH_Q950 = 26, MACH_LCIII = 27,
	MACH_DUO210 = 29, MACH_C650 = 30, MACH_DUO230 = 32, MACH_Q800 = 35, MACH_Q650 = 36,
	MACH_LCII = 37, MACH_DUO250 = 38, MACH_CC = 49, MACH_C610 = 52, MACH_Q610 = 53,
	MACH_Q660AV = 60, MACH_DUO270C = 77, MACH_Q840AV = 78, MACH_LC475 = 89, MACH_LC575 = 92,
	MACH_Q605 = 94, MACH_Q630 = 98, MACH_DUO280 = 102, MACH_DUO280C = 103, MACH_PB150 = 115,
};

static const struct { int16_t type; const char *name; uint32_t clock; } machines[] =
{
	{ MACH_512KE, "Macintosh 512Ke", 7833600 },
	{ MACH_PLUS, "Macintosh Plus", 7833600 },
	{ MACH_SE, "Macintosh SE", 7833600 },
	{ MACH_II, "Macintosh II", 15667200 },
	{ MACH_IIX, "Macintosh IIx", 15667200 },
	{ MACH_IICX, "Macintosh IIcx", 15667200 },
	{ MACH_SE30, "Macintosh SE/30", 15667200 },
	{ MACH_PORTABLE, "Macintosh Portable", 15667200 },
	{ MACH_IICI, "Macintosh IIci", 25000000 },
	{ MACH_IIFX, "Macintosh IIfx", 40000000 },
	{ MACH_CLASSIC, "Macintosh Classic", 7833600 },
	{ MACH_IISI, "Macintosh IIsi", 20000000 },
	{ MACH_LC, "Macintosh LC", 15667200 },
	{ MACH_Q900, "Quadra 900", 25000000 },
	{ MACH_PB170, "PowerBook 170", 25000000 },
	{ MACH_Q700, "Quadra 700", 25000000 },
	{ MACH_CLASSICII, "Classic II", 15667200 },
	{ MACH_PB100, "PowerBook 100", 15667200 },
	{ MACH_PB140, "PowerBook 140", 16000000 },
	{ MACH_Q950, "Quadra 950", 33000000 },
	{ MACH_LCIII, "Macintosh LC III", 25000000 },
	{ MACH_LCII, "Macintosh LC II", 15667200 },
	{ MACH_CC, "Color Classic", 15667200 },
	{ MACH_C650, "Centris 650", 25000000 },
	{ MACH_Q800, "Quadra 800", 33000000 },
	{ MACH_Q650, "Quadra 650", 33000000 },
	{ MACH_C610, "Centris 610", 20000000 },
	{ MACH_Q610, "Quadra 610", 25000000 },
	{ MACH_Q660AV, "Quadra 660AV", 25000000 },
	{ MACH_Q840AV, "Quadra 840AV", 40000000 },
	{ MACH_LC475, "LC 475", 25000000 },
	{ MACH_LC575, "LC 575", 33000000 },
	{ MACH_Q605, "Quadra 605", 25000000 },
	{ MACH_Q630, "Quadra 630", 33000000 },
};

static inline uint32_t lm32(uint32_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline uint32_t ticks(void)
{
	return *(volatile uint32_t *)LM_TICKS;
}

static inline uint32_t rtcTime(void)
{
	return *(volatile uint32_t *)LM_TIME;
}

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

/* Privileged 68020+ instructions, as opcodes so the file builds for the 68000 */
static uint32_t cacrGet(void)
{
	uint32_t v;
	__asm__ volatile(".short 0x4e7a,0x0002\n\tmove.l %%d0,%0" : "=d"(v) : : "d0");
	return v;
}

static void cacrSet(uint32_t v)
{
	__asm__ volatile("move.l %0,%%d0\n\t.short 0x4e7b,0x0002" : : "d"(v) : "d0", "memory");
}

/* cpusha dc: pushes the dirty lines of the 68040 data cache and invalidates it */
static void cpushaDC(void)
{
	__asm__ volatile(".short 0xf478" : : : "memory");
}

/* cpusha bc: the same for both caches */
static void cpushaBC(void)
{
	__asm__ volatile(".short 0xf4f8" : : : "memory");
}

/* Microseconds trap: high long in a0, low long in d0 */
static uint32_t microseconds(void)
{
	uint32_t lo;
	__asm__ volatile(".short 0xa193\n\tmove.l %%d0,%0" : "=g"(lo) : : "d0", "d1", "a0", "a1", "memory");
	return lo;
}

struct bt_args
{
	void *code;					/* 0 */
	volatile uint8_t *target;	/* 4 */
	volatile uint8_t *t2l;		/* 8 */
	volatile uint8_t *t2h;		/* 12 */
	uint16_t loops;				/* 16: loop count - 1 */
	uint8_t wval;				/* 18 */
	uint8_t hi1, lo, hi2;		/* 19, 20, 21 */
	uint8_t pad[2];
};
_Static_assert(offsetof(struct bt_args, loops) == 16, "bt_args layout");
_Static_assert(offsetof(struct bt_args, hi2) == 21, "bt_args layout");

extern void bt_call(struct bt_args *args);

/* Machine */
static long machType, cpuType, sysVersion, clockHz, tmgrVersion;
static const char *machName = "unknown";
static bool clockFromGestalt;
static bool compact68k;		/* Plus, SE, Classic: video shares the RAM, VBL on CA1 */
static uint32_t addrMapFlags;
static bool addrMapFlagsValid;
static uint16_t romVersion;

static volatile uint8_t *via;

/* Device bases and what was found */
static volatile uint8_t *via2, *sccRd, *sccWr, *iwm, *scsi, *asc, *romAddr;
static uint32_t lmScsiBase;
static int scsiWriteOffset;
static const char *scsiNote = "";
static bool hasVia2, hasScc, hasIwm, hasScsi, hasAsc;
static bool scsiIsEsp;
static uint8_t ascVersion;
static bool ascWritable;
static const char *iwmMode = "absent";
static uint8_t iwmProbe[4];
static uint8_t viaIER;

/* Measurement settings */
static bool vblSync;
static uint32_t runBudget;		/* VIA ticks per timed run */
static int repeats;
static int codeBudget;			/* Bytes of the block of N accesses */
static uint32_t cacrSaved;
static uint32_t spinPerMs;
static uint32_t vblMisses;
static uint16_t t0;				/* Timer overhead: the code with an empty block */

static uint16_t *codeBuf;
static uint8_t *ramBuf;

/* Ticks at the start and the end */
static uint32_t startTicks, endTicks;

/* Measured VIA clock */
static long viaHzMeasured;
static long ticksPerS100;

/*
 * Output, kept to save it to a file at the end
 */
static char outText[65536];
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
	static const unsigned char name[] = "\pBusTiming Results";
	short refNum;

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

/*
 * VIA1 timer 2
 */
static void timerLoad(uint16_t count)
{
	via[VIA_T2CL] = count & 0xFF;
	via[VIA_T2CH] = count >> 8;
}

/* Reads the counter. This clears its interrupt flag. */
static uint16_t timerCount(void)
{
	uint8_t hi, lo;

	do
	{
		hi = via[VIA_T2CH];
		lo = via[VIA_T2CL];
	} while (hi != via[VIA_T2CH]);
	return (hi << 8) | lo;
}

/* What the Time Manager had in the timer when we borrowed it */
static bool borrowedPending;
static uint16_t borrowedCount;

/* Masks the timer 2 interrupt and saves its state. Interrupts must be masked. */
static void BorrowTimer(void)
{
	via[VIA_IER] = VIA_IRQ_T2;
	borrowedPending = via[VIA_IFR] & VIA_IRQ_T2;
	borrowedCount = timerCount();
}

/*
 * Gives the timer back with the count it had, less the ticks we used, so
 * the Time Manager gets its interrupt about when it expects it, or now if
 * that has passed. Interrupts must be masked.
 */
static void ReturnTimer(uint32_t used)
{
	if (borrowedPending || borrowedCount <= used + 1)
	{
		timerLoad(1);
	}
	else
	{
		timerLoad(borrowedCount - used);
	}
	if (viaIER & VIA_IRQ_T2)
	{
		via[VIA_IER] = 0x80 | VIA_IRQ_T2;
	}
}

/* Elapsed ticks of a run started at $FFFF, from the high, low, high reads */
static uint16_t runTicks(const struct bt_args *a)
{
	uint16_t count;

	if (a->hi1 == a->hi2 || a->lo < 0x80)
	{
		count = (a->hi1 << 8) | a->lo;
	}
	else
	{
		count = (a->hi2 << 8) | a->lo;
	}
	return 0xFFFF - count;
}

/*
 * Busy loop, calibrated against the timer, to wait with interrupts enabled
 */
static void __attribute__((noinline)) spin(uint32_t n)
{
	while (n--)
	{
		__asm__ volatile("");
	}
}

/*
 * Windows with interrupts masked and the timer borrowed
 */
static uint16_t windowSR;
static uint32_t windowUsed;

/*
 * Waits for the start of the vertical blanking of a compact Mac: CA1 is its
 * VBL interrupt. We wait for a VBL interrupt to have been handled (Ticks
 * changes), then most of a frame with interrupts enabled, then poll CA1
 * with interrupts masked. The flag is left set: the VBL interrupt is only
 * served late, after the window.
 */
static uint16_t vblWindowBegin(void)
{
	for (int tries = 0; tries < 8; tries++)
	{
		uint32_t t = ticks();
		while (ticks() == t)
		{
		}
		t = ticks();
		spin(spinPerMs * 12);
		uint16_t sr = DisableIRQ();
		if (ticks() == t && !(via[VIA_IFR] & VIA_IRQ_CA1))
		{
			uint32_t guard = 20000;
			while (!(via[VIA_IFR] & VIA_IRQ_CA1) && --guard)
			{
			}
			if (guard)
			{
				return sr;
			}
		}
		RestoreIRQ(sr);
	}
	vblMisses++;
	return DisableIRQ();
}

static void WindowBegin(void)
{
	windowSR = vblSync ? vblWindowBegin() : DisableIRQ();
	windowUsed = 0;
	BorrowTimer();
	/* With the data cache off, every access goes to the bus */
	if (cpuType >= 3)
	{
		cacrSaved = cacrGet();
	}
	if (cpuType == 4)
	{
		cacrSet(cacrSaved & ~0x0100UL);
	}
	else if (cpuType == 5)
	{
		cpushaDC();
		cacrSet(cacrSaved & ~0x80000000UL);
	}
}

static void WindowEnd(void)
{
	if (cpuType == 4)
	{
		/* Also clears the data cache, which writes did not update while it was off */
		cacrSet(cacrSaved | 0x0800);
	}
	else if (cpuType == 5)
	{
		cacrSet(cacrSaved);
	}
	ReturnTimer(windowUsed + 4);
	RestoreIRQ(windowSR);
}

/* Runs the code once in a window */
static uint16_t Run(struct bt_args *a)
{
	bt_call(a);
	uint16_t t = runTicks(a);
	windowUsed += t;
	return t;
}

/*
 * Code generation
 */
static void FlushCode(void)
{
	uint16_t sr = DisableIRQ();
	if (cpuType == 3 || cpuType == 4)
	{
		cacrSet(cacrGet() | 0x08);
	}
	else if (cpuType == 5)
	{
		cpushaBC();
	}
	RestoreIRQ(sr);
}

/* Generates: start the timer; loop: n x (op, k x filler); dbra; read the timer */
static void Generate(uint16_t op, int n, int k)
{
	uint16_t *p = codeBuf;

	*p++ = OP_START_LO;
	*p++ = OP_START_HI;
	uint16_t *loop = p;
	for (int i = 0; i < n; i++)
	{
		*p++ = op;
		for (int j = 0; j < k; j++)
		{
			*p++ = OP_FILL;
		}
	}
	*p++ = OP_DBRA_D6;
	*p = (uint16_t)((loop - p) * 2);
	p++;
	*p++ = OP_STOP_1;
	*p++ = OP_STOP_2;
	*p++ = OP_STOP_3;
	*p++ = OP_RTS;
	FlushCode();
}

/* Accesses per block for k fillers, so the code stays in the I-cache */
static int BlockSize(int k)
{
	int n = codeBudget / (2 + 2 * k);
	return n > 64 ? 64 : n;
}

/*
 * Targets
 */
enum { KIND_REG, KIND_READ, KIND_WRITE };
enum { PRE_NONE, PRE_IWM, PRE_WSRC };

#define MAX_TARGETS		20
#define MAX_K			40

static const int16_t kList[MAX_K] =
{
	0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40,
	48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256,
};
#define GAP_LIMIT_DNS	45000	/* Stop the sweep at 4.5 us of filler */

struct result
{
	bool ok;
	uint16_t n, loops;
	int32_t minDns, medDns;		/* Per access, in tenths of ns */
	uint16_t spread;			/* Max - min ticks over the repeats */
};

struct target
{
	char name[28];
	volatile uint8_t *addr;
	uint8_t kind, pre;
	volatile uint8_t *wsrc;		/* PRE_WSRC: register read for the value to write */
	uint8_t wval;
	struct result r[MAX_K];
};

static struct target targets[MAX_TARGETS];
static int nTargets;
static int nK;
static struct target *regTarget, *ramTarget;

static struct target *AddTarget(const char *name, volatile uint8_t *addr, uint8_t kind, uint8_t pre,
		volatile uint8_t *wsrc, uint8_t wval)
{
	struct target *t = &targets[nTargets++];
	snprintf(t->name, sizeof(t->name), "%s", name);
	t->addr = addr;
	t->kind = kind;
	t->pre = pre;
	t->wsrc = wsrc;
	t->wval = wval;
	return t;
}

static void TargetPre(struct target *t, struct bt_args *a)
{
	switch (t->pre)
	{
	case PRE_IWM:
		/* IWM: Q7L then Q6H selects the status register. ISM: reads the mode and setup registers. */
		(void)iwm[0x1C00];
		(void)iwm[0x1A00];
		break;
	case PRE_WSRC:
		a->wval = *t->wsrc;
		break;
	}
}

static void TargetPost(struct target *t)
{
	if (t->pre == PRE_IWM)
	{
		/* IWM: Q6L, back to Q6 and Q7 low. ISM: reads the phases. */
		(void)iwm[0x1800];
	}
}

static uint16_t OpFor(const struct target *t)
{
	switch (t->kind)
	{
	case KIND_READ: return OP_READ;
	case KIND_WRITE: return OP_WRITE;
	default: return OP_REG;
	}
}

/* One window: optional warm-up run, then the timed run */
static uint16_t TimedWindow(struct target *t, struct bt_args *a)
{
	WindowBegin();
	TargetPre(t, a);
	if (cpuType != 1)
	{
		(void)Run(a);
	}
	uint16_t ticks = Run(a);
	TargetPost(t);
	WindowEnd();
	return ticks;
}

static void SortU16(uint16_t *v, int n)
{
	for (int i = 1; i < n; i++)
	{
		uint16_t x = v[i];
		int j = i - 1;
		while (j >= 0 && v[j] > x)
		{
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = x;
	}
}

static int32_t PerAccessDns(int32_t ticks, uint32_t accesses)
{
	return (int32_t)((long long)ticks * 10000000000LL / ((long long)VIA_HZ * accesses));
}

static void Measure(struct target *t, int ki)
{
	const int k = kList[ki];
	struct result *r = &t->r[ki];
	const int n = BlockSize(k);
	struct bt_args a = {0};

	r->ok = false;
	if (n < 1)
	{
		return;
	}
	Generate(OpFor(t), n, k);
	a.code = codeBuf;
	a.target = t->addr;
	a.t2l = &via[VIA_T2CL];
	a.t2h = &via[VIA_T2CH];
	a.wval = t->wval;

	/* Trial with one loop to choose the loop count */
	a.loops = 0;
	int32_t one = (int32_t)TimedWindow(t, &a) - t0;
	if (one < 1)
	{
		one = 1;
	}
	uint32_t loops = runBudget / one;
	if (loops < 1)
	{
		loops = 1;
	}
	if (loops > 16384)
	{
		loops = 16384;
	}
	a.loops = loops - 1;

	uint16_t v[16];
	for (int i = 0; i < repeats; i++)
	{
		v[i] = TimedWindow(t, &a);
	}
	SortU16(v, repeats);
	r->n = n;
	r->loops = loops;
	r->minDns = PerAccessDns((int32_t)v[0] - t0, n * loops);
	r->medDns = PerAccessDns((int32_t)v[repeats / 2] - t0, n * loops);
	r->spread = v[repeats - 1] - v[0];
	r->ok = true;
}

/* Timer overhead, with an empty block and one loop */
static void MeasureT0(void)
{
	struct bt_args a = {0};
	uint16_t v[16];

	Generate(OP_REG, 0, 0);
	a.code = codeBuf;
	a.target = ramBuf;
	a.t2l = &via[VIA_T2CL];
	a.t2h = &via[VIA_T2CH];
	for (int i = 0; i < repeats; i++)
	{
		WindowBegin();
		if (cpuType != 1)
		{
			(void)Run(&a);
		}
		v[i] = Run(&a);
		WindowEnd();
	}
	SortU16(v, repeats);
	t0 = v[0];
}

/*
 * Setup
 */
static long GestaltOr(OSType selector, long otherwise)
{
	long v;
	return Gestalt(selector, &v) == noErr ? v : otherwise;
}

static void DetectMachine(void)
{
	machType = GestaltOr('mach', 0);
	cpuType = GestaltOr('proc', 1);
	sysVersion = GestaltOr('sysv', 0);
	tmgrVersion = GestaltOr('tmgr', 0);
	clockHz = GestaltOr('pclk', 0);
	clockFromGestalt = clockHz != 0;
	for (size_t i = 0; i < sizeof(machines) / sizeof(machines[0]); i++)
	{
		if (machines[i].type == machType)
		{
			machName = machines[i].name;
			if (!clockHz)
			{
				clockHz = machines[i].clock;
			}
		}
	}
	compact68k = cpuType == 1 && (machType == MACH_512KE || machType == MACH_PLUS ||
			machType == MACH_SE || machType == MACH_CLASSIC);
	/* Not set by the ROMs of the compacts and of the Mac II, which leave $FFFFFFFF */
	addrMapFlags = compact68k ? 0 : lm32(LM_ADDRMAPFLAGS);
	addrMapFlagsValid = addrMapFlags != 0 && addrMapFlags != 0xFFFFFFFFUL;
	romAddr = (volatile uint8_t *)lm32(LM_ROMBASE);
	romVersion = *(volatile uint16_t *)(romAddr + 8);
	via = (volatile uint8_t *)lm32(LM_VIA);
	viaIER = via[VIA_IER];
}

/* The IIfx and the Quadra 900/950: SCC and SWIM behind IOPs, SCSI not tried for now */
static bool IsIopMachine(void)
{
	return machType == MACH_IIFX || machType == MACH_Q900 || machType == MACH_Q950;
}

static bool IsDuo(void)
{
	return machType == MACH_DUO210 || machType == MACH_DUO230 || machType == MACH_DUO250 ||
			machType == MACH_DUO270C || machType == MACH_DUO280 || machType == MACH_DUO280C;
}

/*
 * IWM or SWIM: the accesses below are harmless in both modes. In IWM mode
 * every access sets or clears a state line: $1C00 is Q7L, $1A00 Q6H and
 * $1800 Q6L, so this reads the status register twice and leaves Q6 and Q7
 * low. In ISM mode they read the mode, setup, mode and phases registers.
 * The ISM mode register has bit 6 set, the IWM status register has it clear.
 */
static bool ProbeIwm(bool *busy)
{
	uint16_t sr = DisableIRQ();
	iwmProbe[0] = iwm[0x1C00];
	iwmProbe[1] = iwm[0x1A00];
	iwmProbe[2] = iwm[0x1C00];
	iwmProbe[3] = iwm[0x1800];
	RestoreIRQ(sr);

	if ((iwmProbe[2] & 0x40) && iwmProbe[0] == iwmProbe[2])
	{
		iwmMode = "ISM";
		/* Motor on, action or write */
		*busy = iwmProbe[2] & 0x98;
		return true;
	}
	if ((iwmProbe[1] & 0x7F) == (iwmProbe[2] & 0x7F))
	{
		iwmMode = "IWM";
		/* Drive enabled */
		*busy = iwmProbe[2] & 0x20;
		return true;
	}
	iwmMode = "unknown";
	return false;
}

/*
 * A device pointer from low memory, if it points to I/O space: $F00000 to
 * $FFFFFF in 24-bit terms ($50F00000 and up in 32-bit mode) on all the
 * Macs since the Mac II and the Portable.
 */
static volatile uint8_t *IoPointer(uint32_t lm)
{
	uint32_t p = lm32(lm);
	return (p & 0x00F00000UL) == 0x00F00000UL ? (volatile uint8_t *)p : NULL;
}

static bool IsMacIIFamily(void)
{
	return machType == MACH_II || machType == MACH_IIX || machType == MACH_IICX || machType == MACH_SE30;
}

/*
 * Where the 53C96 is, in the low 28 bits: next to DAFB on the Quadra
 * 700, in the IOSB or PrimeTime on the Centris/Quadra 610/650/800
 * and the LC 475/575/Quadra 605 (MAME macquadra700.cpp, iosb.cpp)
 */
static uint32_t EspBase(void)
{
	switch (machType)
	{
	case MACH_Q700:
		return 0x00F0F000UL;
	case MACH_C610: case MACH_C650: case MACH_Q610: case MACH_Q650: case MACH_Q800:
	case MACH_LC475: case MACH_LC575: case MACH_Q605:
		return 0x00F10000UL;
	default:
		return 0xFFFFFFFFUL;
	}
}

static void DetectDevices(void)
{
	lmScsiBase = lm32(LM_SCSIBASE);
	if (compact68k)
	{
		/* Fixed addresses (Guide to the Macintosh Family Hardware). SCSIBase is not the 5380 here. */
		sccRd = (volatile uint8_t *)lm32(LM_SCCRD);
		sccWr = (volatile uint8_t *)lm32(LM_SCCWR);
		hasScc = sccRd == (volatile uint8_t *)0x9FFFF8 && sccWr == (volatile uint8_t *)0xBFFFF9;
		scsi = (volatile uint8_t *)0x580000;
		scsiWriteOffset = 1;
		hasScsi = machType != MACH_512KE;
	}
	else
	{
		sccRd = IoPointer(LM_SCCRD);
		sccWr = IoPointer(LM_SCCWR);
		hasScc = sccRd && sccWr && !IsIopMachine();
		scsi = IoPointer(LM_SCSIBASE);
		/* Not on the IOP machines for now */
		hasScsi = scsi != NULL && !IsIopMachine();
	}

	/* The 68040 Macs have a 53C96, except the Duo 280s */
	scsiIsEsp = cpuType == 5 && machType != MACH_DUO280 && machType != MACH_DUO280C;
	if (scsiIsEsp && hasScsi && ((uint32_t)scsi & 0x0FFFFFFFUL) != EspBase())
	{
		/* Next to the 53C96 is its pseudo-DMA port, where a read takes a byte: only known bases */
		hasScsi = false;
		scsiNote = ", base not verified for this machine, skipped";
	}

	if (addrMapFlagsValid)
	{
		via2 = IoPointer(LM_VIA2);
		hasVia2 = via2 && (addrMapFlags & ((1UL << 11) | (1UL << 13)));
		asc = IoPointer(LM_ASCBASE);
		hasAsc = asc && (addrMapFlags & (1UL << 12));
	}
	else if (IsMacIIFamily())
	{
		/* Their ROM does not set AddrMapFlags. They all have a VIA2 and an ASC. */
		via2 = IoPointer(LM_VIA2);
		hasVia2 = via2 != NULL;
		asc = IoPointer(LM_ASCBASE);
		hasAsc = asc != NULL;
	}
	if (hasAsc)
	{
		ascVersion = asc[0x800];
		/* Only the original ASC: $806 (volume) reads back what was written */
		ascWritable = ascVersion == 0x00;
	}

	if (compact68k)
	{
		iwm = (volatile uint8_t *)lm32(LM_IWM);
		if (iwm != (volatile uint8_t *)0xDFE1FF)
		{
			iwm = NULL;
		}
	}
	else
	{
		iwm = IoPointer(LM_IWM);
	}
	if (iwm && !IsIopMachine() && !IsDuo())
	{
		bool busy = true;
		for (int i = 0; i < 20; i++)
		{
			if (!ProbeIwm(&busy))
			{
				break;
			}
			if (!busy)
			{
				hasIwm = true;
				break;
			}
			/* Wait for the floppy driver to turn the motor off */
			uint32_t t = ticks();
			while (ticks() - t < 30)
			{
			}
		}
		if (busy && strcmp(iwmMode, "unknown"))
		{
			iwmMode = "busy";
		}
	}
}

static void BuildTargets(void)
{
	char name[28];

	regTarget = AddTarget("register only", ramBuf, KIND_REG, PRE_NONE, NULL, 0);
	ramTarget = AddTarget("RAM read", ramBuf, KIND_READ, PRE_NONE, NULL, 0);
	AddTarget("RAM write", ramBuf + 2, KIND_WRITE, PRE_NONE, NULL, 0x5A);
	AddTarget("ROM read", romAddr + 0x100, KIND_READ, PRE_NONE, NULL, 0);
	AddTarget("VIA1 DDRB read", via + VIA_DDRB, KIND_READ, PRE_NONE, NULL, 0);
	AddTarget("VIA1 IER read", via + VIA_IER, KIND_READ, PRE_NONE, NULL, 0);
	AddTarget("VIA1 DDRB write", via + VIA_DDRB, KIND_WRITE, PRE_WSRC, via + VIA_DDRB, 0);
	if (hasVia2)
	{
		/* IER on a VIA (A9-A12) and on a pseudo-VIA (low address bits) */
		AddTarget("VIA2 IER read", via2 + 0x1C13, KIND_READ, PRE_NONE, NULL, 0);
	}
	if (hasScc)
	{
		/* Channel A control: RR0 with the pointer at 0, WR0 with 0 is a null command */
		AddTarget("SCC A ctl read", sccRd + 2, KIND_READ, PRE_NONE, NULL, 0);
		AddTarget("SCC A ctl write 0", sccWr + 2, KIND_WRITE, PRE_NONE, NULL, 0);
	}
	if (hasIwm)
	{
		snprintf(name, sizeof(name), "%s %s read", iwmMode, iwmMode[1] == 'W' ? "status" : "mode");
		AddTarget(name, iwm + 0x1C00, KIND_READ, PRE_IWM, NULL, 0);
	}
	if (hasScsi)
	{
		/* 5380 Current SCSI Bus Status, or 53C96 Status: no side effects */
		AddTarget(scsiIsEsp ? "53C96 status read" : "5380 bus status rd", scsi + 0x40, KIND_READ,
				PRE_NONE, NULL, 0);
		/* Register 2 is the FIFO of a 53C96: only where the chip is known to be a 5380 */
		if (!scsiIsEsp && cpuType <= 4 && machType != MACH_PB150)
		{
			/* 5380 Mode register, written back. Writes are at odd addresses on the 68000 compacts. */
			AddTarget("5380 mode write", scsi + 0x20 + scsiWriteOffset, KIND_WRITE, PRE_WSRC,
					scsi + 0x20, 0);
		}
	}
	if (hasAsc)
	{
		AddTarget("ASC version read", asc + 0x800, KIND_READ, PRE_NONE, NULL, 0);
		if (ascWritable)
		{
			AddTarget("ASC $806 write", asc + 0x806, KIND_WRITE, PRE_WSRC, asc + 0x806, 0);
		}
	}
}

/*
 * The VIA clock against the real time clock: Microseconds counts VIA ticks
 * (at the nominal 783.36 kHz) and Time counts the seconds of the RTC.
 */
static bool HasMicroseconds(void)
{
	return tmgrVersion >= 3 &&
			NGetTrapAddress(0xA193, kOSTrapType) != NGetTrapAddress(_Unimplemented, kToolboxTrapType);
}

static bool WaitSecond(void)
{
	uint32_t s = rtcTime();
	uint32_t t = ticks();
	while (rtcTime() == s)
	{
		if (ticks() - t > 150)
		{
			return false;
		}
	}
	return true;
}

static void MeasureViaClock(void)
{
	const int seconds = 5;

	if (!HasMicroseconds() || !WaitSecond())
	{
		return;
	}
	uint32_t us1 = microseconds();
	uint32_t t1 = ticks();
	for (int i = 0; i < seconds; i++)
	{
		if (!WaitSecond())
		{
			return;
		}
	}
	uint32_t us2 = microseconds();
	uint32_t t2 = ticks();
	viaHzMeasured = (long)((long long)(us2 - us1) * VIA_HZ / (seconds * 1000000LL));
	ticksPerS100 = (long)(t2 - t1) * 100 / seconds;
}

/* Iterations of spin() per millisecond, with the video stealing RAM cycles */
static void CalibrateSpin(void)
{
	struct bt_args dummy;
	uint16_t sr = DisableIRQ();
	BorrowTimer();
	timerLoad(0xFFFF);
	spin(2000);
	dummy.hi1 = via[VIA_T2CH];
	dummy.lo = via[VIA_T2CL];
	dummy.hi2 = via[VIA_T2CH];
	uint16_t t = runTicks(&dummy);
	ReturnTimer(t);
	RestoreIRQ(sr);
	spinPerMs = t ? 2000UL * 783 / t : 1000;
}

/*
 * Printing
 */
static void FmtNs(char *buf, int32_t dns)
{
	const char *sign = "";
	if (dns < 0)
	{
		sign = "-";
		dns = -dns;
	}
	sprintf(buf, "%s%ld.%ld", sign, (long)(dns / 10), (long)(dns % 10));
}

static void FmtClk(char *buf, int32_t dns)
{
	if (!clockHz)
	{
		strcpy(buf, "?");
		return;
	}
	long c100 = (long)((long long)dns * clockHz / 100000000LL);
	const char *sign = "";
	if (c100 < 0)
	{
		sign = "-";
		c100 = -c100;
	}
	sprintf(buf, "%s%ld.%02ld", sign, c100 / 100, c100 % 100);
}

static int32_t GapDns(int ki)
{
	return regTarget->r[ki].minDns - regTarget->r[0].minDns;
}

static void PrintHeader(void)
{
	out("BusTiming v%s\n", VERSION);
	if (clockHz)
	{
		out("Machine: %s (gestalt %ld), CPU 680%ld0, clock %ld Hz%s\n", machName, machType,
				cpuType - 1, clockHz, clockFromGestalt ? "" : " (assumed)");
	}
	else
	{
		out("Machine: %s (gestalt %ld), CPU 680%ld0, clock unknown\n", machName, machType, cpuType - 1);
	}
	out("System %lx.%lx.%lx, ROM version $%04X, Time Manager %ld\n", (sysVersion >> 8) & 0xFF,
			(sysVersion >> 4) & 0xF, sysVersion & 0xF, romVersion, tmgrVersion);
	if (!compact68k)
	{
		out("AddrMapFlags $%08lX%s\n", addrMapFlags, addrMapFlagsValid ? "" : " (not valid)");
	}
	if (viaHzMeasured)
	{
		out("VIA clock: %ld Hz (Microseconds over 5 RTC seconds), nominal %ld; Ticks about %ld.%02ld/s\n",
				viaHzMeasured, (long)VIA_HZ, ticksPerS100 / 100, ticksPerS100 % 100);
	}
	else
	{
		out("VIA clock: not measured (no Microseconds or no RTC seconds), nominal %ld\n", (long)VIA_HZ);
	}
	out("VIA1 $%08lX IER $%02X\n", (unsigned long)via, viaIER);
	out("VIA2 %s $%08lX\n", hasVia2 ? "yes" : "no", (unsigned long)via2);
	out("SCC %s rd $%08lX wr $%08lX\n", hasScc ? "yes" : "no", (unsigned long)sccRd, (unsigned long)sccWr);
	out("IWM/SWIM $%08lX %s (probe %02X %02X %02X %02X)\n", (unsigned long)iwm, iwmMode,
			iwmProbe[0], iwmProbe[1], iwmProbe[2], iwmProbe[3]);
	out("SCSI %s $%08lX (%s%s), SCSIBase $%08lX\n", hasScsi ? "yes" : "no", (unsigned long)scsi,
			scsiIsEsp ? "53C96 assumed" : "5380 assumed", scsiNote, (unsigned long)lmScsiBase);
	out("ASC %s $%08lX version $%02X\n", hasAsc ? "yes" : "no", (unsigned long)asc, ascVersion);
	if (cpuType >= 3)
	{
		out("CACR $%08lX (data cache off during the runs)\n", cacrSaved);
	}
	out("VBL sync %s (misses %lu), repeats %d, run budget %lu ticks, block budget %d bytes\n",
			vblSync ? "yes" : "no", vblMisses, repeats, runBudget, codeBudget);
	out("Timer overhead t0 = %u ticks, run time %lu s\n", t0, (endTicks - startTicks) / 60);
}

static void PrintTables(void)
{
	char a[16], b[16], c[16], d[16], e[16], f[16];

	out("\nPer access, from the fastest of the repeats. gap: filler time (register only\n"
		"at k less register only at k=0). bus: less register only at the same k.\n"
		"ram: less RAM read at the same k. med: median of the repeats.\n");
	for (int ti = 0; ti < nTargets; ti++)
	{
		struct target *t = &targets[ti];
		out("\n%s @ $%08lX\n", t->name, (unsigned long)t->addr);
		out("   k   gap_ns   raw_ns   med_ns   bus_ns  bus_clk   ram_ns     NxL spr\n");
		for (int ki = 0; ki < nK; ki++)
		{
			const struct result *r = &t->r[ki];
			if (!r->ok || !regTarget->r[ki].ok)
			{
				continue;
			}
			int32_t bus = r->minDns - regTarget->r[ki].minDns;
			FmtNs(a, GapDns(ki));
			FmtNs(b, r->minDns);
			FmtNs(c, r->medDns);
			FmtNs(d, bus);
			FmtClk(e, bus);
			if (ramTarget->r[ki].ok)
			{
				FmtNs(f, r->minDns - ramTarget->r[ki].minDns);
			}
			else
			{
				strcpy(f, "-");
			}
			out("%4d %8s %8s %8s %8s %8s %8s %4ux%-4u %u\n", kList[ki], a, b, c, d, e, f, r->n, r->loops,
					r->spread);
		}
	}
}

/*
 * Summary: back to back cost, the floor of the bus time over the sweep, and
 * the filler time from which it stays within 1/4 clock (or 10 ns) of the floor.
 */
static void PrintSummary(void)
{
	char a[16], b[16], c[16], d[16], e[16];
	const int32_t tol = clockHz ? (int32_t)(2500000000LL / clockHz) : 100;

	out("\nSummary            b2b_ns b2b_via  b2b_bus  b2b_clk floor_ns floor_clk knee_gap_ns\n");
	for (int ti = 0; ti < nTargets; ti++)
	{
		struct target *t = &targets[ti];
		if (t == regTarget || !t->r[0].ok)
		{
			continue;
		}
		int32_t b2b = t->r[0].minDns;
		int32_t b2bBus = b2b - regTarget->r[0].minDns;
		int32_t floor = b2bBus;
		for (int ki = 0; ki < nK; ki++)
		{
			if (t->r[ki].ok)
			{
				int32_t bus = t->r[ki].minDns - regTarget->r[ki].minDns;
				if (bus < floor)
				{
					floor = bus;
				}
			}
		}
		int knee = 0;
		for (int ki = 0; ki < nK; ki++)
		{
			if (t->r[ki].ok && t->r[ki].minDns - regTarget->r[ki].minDns <= floor + tol)
			{
				knee = ki;
				break;
			}
		}
		FmtNs(a, b2b);
		/* In VIA clock cycles, hundredths */
		long via100 = (long)((long long)b2b * VIA_HZ / 100000000LL);
		char v[16];
		sprintf(v, "%ld.%02ld", via100 / 100, via100 % 100);
		FmtNs(b, b2bBus);
		FmtClk(c, b2bBus);
		FmtNs(d, floor);
		FmtClk(e, floor);
		char g[16];
		FmtNs(g, GapDns(knee));
		out("%-18.18s %7s %7s %8s %8s %8s %9s %11s\n", t->name, a, v, b, c, d, e, g);
	}
}

int main(void)
{
	printf("BusTiming v%s: measuring, don't touch the mouse or the keyboard.\n", VERSION);
	printf("This takes a minute or two.\n\n");

	startTicks = ticks();
	DetectMachine();
	if (GestaltOr('vm  ', 0) & 1)
	{
		out("BusTiming v%s\nVirtual memory is on. Turn it off in the Memory control panel, restart and run again.\n",
				VERSION);
		SaveResults();
		getchar();
		return 0;
	}
	if (cpuType >= 3)
	{
		uint16_t sr = DisableIRQ();
		cacrSaved = cacrGet();
		RestoreIRQ(sr);
	}

	codeBuf = (uint16_t *)(((uintptr_t)NewPtr(65536 + 16) + 15) & ~15UL);
	ramBuf = (uint8_t *)NewPtrClear(64);
	if (!codeBuf || !ramBuf)
	{
		printf("Out of memory\n");
		getchar();
		return 0;
	}

	switch (cpuType)
	{
	case 1:
	case 2:
		codeBudget = 64 * 64;
		break;
	case 5:
		codeBudget = 2048;
		break;
	default:
		/* 256 byte I-cache: the whole code must fit */
		codeBudget = 256 - WRAPPER_BYTES;
		break;
	}
	vblSync = compact68k;
	runBudget = vblSync ? 600 : 1200;
	repeats = vblSync ? 3 : 7;

	printf("Measuring the VIA clock (5 s)...\n");
	MeasureViaClock();
	DetectDevices();
	BuildTargets();
	if (vblSync)
	{
		CalibrateSpin();
	}
	MeasureT0();

	for (int ki = 0; ki < MAX_K && kList[ki] >= 0 && (ki == 0 || kList[ki]); ki++)
	{
		if (BlockSize(kList[ki]) < 1)
		{
			break;
		}
		for (int ti = 0; ti < nTargets; ti++)
		{
			Measure(&targets[ti], ki);
		}
		nK = ki + 1;
		int32_t gap = GapDns(ki);
		printf("k=%d gap %ld ns\n", kList[ki], (long)(gap / 10));
		if (gap >= GAP_LIMIT_DNS)
		{
			break;
		}
	}

	endTicks = ticks();
	printf("\n");
	PrintHeader();
	PrintSummary();
	PrintTables();
	SaveResults();
	printf("\nSaved to \"BusTiming Results\". Press Return to quit.\n");
	getchar();
	return 0;
}
