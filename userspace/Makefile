.SUFFIXES:

.PHONY: all clean
.DEFAULT_GOAL := all

CC ?= cc
LD ?= ld
AR ?= ar
CFLAGS ?= -g -O2

override CFLAGS += \
	-Wall \
	-Wextra \
	-std=gnu11 \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-fno-pic \
	-fno-pie \
	-fno-builtin \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-m64 \
	-march=x86-64 \
	-mabi=sysv \
	-mno-red-zone

override CPPFLAGS := \
	-I ../src \
	-I kiwilib/include \
	-MMD \
	-MP

override LDFLAGS += \
	-m elf_x86_64 \
	-nostdlib \
	-static \
	-T user.lds

KIWILIB_C := $(wildcard kiwilib/src/*.c)
KIWILIB_S := $(wildcard kiwilib/src/*.S)
KIWILIB_OBJ := $(addprefix build/,$(KIWILIB_C:.c=.c.o) $(KIWILIB_S:.S=.S.o))
KIWILIB_DEPS := $(addprefix build/,$(KIWILIB_C:.c=.c.d) $(KIWILIB_S:.S=.S.d))
KIWILIB_ARCHIVE := bin/libkiwi.a

PROGRAMS := hello badptr filetest readtest writetest init shell preempt_a preempt_b preempttest argtest termtest alloctest cwdtest libctest edit cursestest \
	echo clear pwd cat ls stat which touch mkdir rmdir rm cp mv mount rescan date poweroff reboot shutdown
PROGRAM_OBJ := $(addprefix build/programs/,$(addsuffix /.main.o,$(PROGRAMS)))
PROGRAM_ELF := $(addprefix bin/,$(addsuffix .elf,$(PROGRAMS)))
PROGRAM_KXE := $(addprefix bin/,$(PROGRAMS))
PROGRAM_DEPS := $(addprefix build/programs/,$(addsuffix /.main.d,$(PROGRAMS)))

define PROGRAM_RULES
build/programs/$(1)/.main.o: programs/$(1)/$(1).c
	mkdir -p "$$(dir $$@)"
	$$(CC) $$(CFLAGS) $$(CPPFLAGS) -c $$< -o $$@

bin/$(1).elf: build/programs/$(1)/.main.o $$(KIWILIB_OBJ) user.lds
	mkdir -p "$$(dir $$@)"
	$$(LD) $$(LDFLAGS) $$(KIWILIB_OBJ) $$< -o $$@

bin/$(1): bin/$(1).elf ../tools/elf2kxe
	mkdir -p "$$(dir $$@)"
	../tools/elf2kxe $$< $$@
endef

$(foreach p,$(PROGRAMS),$(eval $(call PROGRAM_RULES,$(p))))

all: $(PROGRAM_KXE)

$(KIWILIB_ARCHIVE): $(KIWILIB_OBJ)
	mkdir -p "$(dir $@)"
	rm -f "$@"
	$(AR) rcs "$@" $(KIWILIB_OBJ)

-include $(KIWILIB_DEPS) $(PROGRAM_DEPS)

../tools/elf2kxe: ../tools/elf2kxe.c ../tools/Makefile
	$(MAKE) -C ../tools elf2kxe

build/kiwilib/src/%.c.o: kiwilib/src/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/kiwilib/src/%.S.o: kiwilib/src/%.S
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -rf build bin
