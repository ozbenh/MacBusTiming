# export RETRO68=/path/to/Retro68-build/toolchain
RETRO68 ?= $(HOME)/hackplace/macfoo/Retro68-build/toolchain
CC=$(RETRO68)/bin/m68k-apple-macos-gcc
REZ=$(RETRO68)/bin/Rez
CFLAGS=-O2 -Wall -Wno-multichar -Wno-array-bounds
LDFLAGS=-lRetroConsole
RINCLUDES=$(RETRO68)/m68k-apple-macos/RIncludes
REZFLAGS=-I$(RINCLUDES)

all: BusTiming.bin LC475Check.bin BusTiming-1.4MB.img

BusTiming.bin: BusTiming.code.bin
	$(REZ) $(REZFLAGS) \
		--copy "BusTiming.code.bin" \
		"$(RINCLUDES)/Retro68APPL.r" \
		-t "APPL" -c "????" \
		-o BusTiming.bin --cc "._BusTiming.ad" --cc BusTiming.dsk

# 1.44 MB HFS floppy image, for Macs whose drive can't read 800K disks any more
BusTiming-1.4MB.img: BusTiming.bin LC475Check.bin
	rm -f $@
	dd if=/dev/zero of=$@ bs=1024 count=1440 status=none
	hformat -l BusTiming $@ >/dev/null
	hcopy -m BusTiming.bin :
	hcopy -m LC475Check.bin :
	humount >/dev/null

BusTiming.code.bin: bustiming.o bt_call.o
	$(RETRO68)/bin/m68k-apple-macos-g++ $^ -o $@ $(LDFLAGS)

bustiming.o: bustiming.c
	$(CC) $(CFLAGS) -c $< -o $@

bt_call.o: bt_call.s
	$(CC) -c $< -o $@

LC475Check.bin: LC475Check.code.bin
	$(REZ) $(REZFLAGS) \
		--copy "LC475Check.code.bin" \
		"$(RINCLUDES)/Retro68APPL.r" \
		-t "APPL" -c "????" \
		-o LC475Check.bin --cc "._LC475Check.ad" --cc LC475Check.dsk

LC475Check.code.bin: lc475check.o
	$(RETRO68)/bin/m68k-apple-macos-g++ $^ -o $@ $(LDFLAGS)

lc475check.o: lc475check.c
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -f LC475Check.bin LC475Check.code.bin LC475Check.code.bin.gdb LC475Check.ad ._LC475Check.ad LC475Check.dsk BusTiming-1.4MB.img BusTiming.bin BusTiming.code.bin BusTiming.code.bin.gdb *.o BusTiming.ad ._BusTiming.ad BusTiming.dsk
