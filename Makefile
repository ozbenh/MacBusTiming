# export RETRO68=/path/to/Retro68-build/toolchain
RETRO68 ?= $(HOME)/hackplace/macfoo/Retro68-build/toolchain
CC=$(RETRO68)/bin/m68k-apple-macos-gcc
REZ=$(RETRO68)/bin/Rez
CFLAGS=-O2 -Wall -Wno-multichar -Wno-array-bounds
LDFLAGS=-lRetroConsole
RINCLUDES=$(RETRO68)/m68k-apple-macos/RIncludes
REZFLAGS=-I$(RINCLUDES)

BusTiming.bin: BusTiming.code.bin
	$(REZ) $(REZFLAGS) \
		--copy "BusTiming.code.bin" \
		"$(RINCLUDES)/Retro68APPL.r" \
		-t "APPL" -c "????" \
		-o BusTiming.bin --cc "._BusTiming.ad" --cc BusTiming.dsk

BusTiming.code.bin: bustiming.o bt_call.o
	$(RETRO68)/bin/m68k-apple-macos-g++ $^ -o $@ $(LDFLAGS)

bustiming.o: bustiming.c
	$(CC) $(CFLAGS) -c $< -o $@

bt_call.o: bt_call.s
	$(CC) -c $< -o $@

.PHONY: clean
clean:
	rm -f BusTiming.bin BusTiming.code.bin BusTiming.code.bin.gdb *.o BusTiming.ad ._BusTiming.ad BusTiming.dsk
