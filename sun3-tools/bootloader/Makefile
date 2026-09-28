# Linux/sun3 boot block for the Sun-3 PROM (see boot.c and mk-bootdisk.py)
CROSS ?= m68k-linux-gnu-
CC = $(CROSS)gcc
OBJCOPY = $(CROSS)objcopy
CFLAGS = -m68020 -msoft-float -Os -ffreestanding -fno-builtin -nostdlib \
	 -fno-pic -fno-stack-protector -Wall -Wextra
all: bootblk.bin
bootblk.elf: entry.o boot.o boot.ld
	$(CC) $(CFLAGS) -Wl,--no-warn-rwx-segments -T boot.ld -o $@ entry.o boot.o
bootblk.bin: bootblk.elf
	$(OBJCOPY) -O binary $< $@
	@ls -l $@
%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
clean:
	rm -f *.o bootblk.elf bootblk.bin
.PHONY: all clean
