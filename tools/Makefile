.PHONY: all clean

CC ?= cc
CFLAGS ?= -g -O2 -Wall -Wextra -std=gnu11

TOOLS := elf2kxe kifs_cp mkfs_kifs

all: $(TOOLS)

elf2kxe: elf2kxe.c
	$(CC) $(CFLAGS) $< -o $@

kifs_cp: kifs_cp.c
	$(CC) $(CFLAGS) $< -o $@

mkfs_kifs: mkfs_kifs.c
	$(CC) $(CFLAGS) -I ../src $< -o $@

clean:
	rm -f $(TOOLS)
