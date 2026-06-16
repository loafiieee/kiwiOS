.SUFFIXES:

.PHONY: all clean
.DEFAULT_GOAL := all

CC ?= cc
LD ?= ld
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

PROGRAMS := hello badptr filetest readtest writetest init shell preempt_a preempt_b preempttest
PROGRAM_OBJ := $(addprefix build/programs/,$(addsuffix /.main.o,$(PROGRAMS)))
PROGRAM_ELF := $(addprefix bin/,$(addsuffix .elf,$(PROGRAMS)))
PROGRAM_KXE := $(addprefix bin/,$(PROGRAMS))
PROGRAM_DEPS := $(addprefix build/programs/,$(addsuffix /.main.d,$(PROGRAMS)))

$(foreach p,$(PROGRAMS),$(eval build/programs/$(p)/.main.o: programs/$(p)/$(p).c))

all: $(PROGRAM_KXE)

-include $(KIWILIB_DEPS) $(PROGRAM_DEPS)

bin/%: bin/%.elf ../tools/elf2kxe
	mkdir -p "$(dir $@)"
	../tools/elf2kxe $< $@

bin/%.elf: build/programs/%/.main.o $(KIWILIB_OBJ) user.lds
	mkdir -p "$(dir $@)"
	$(LD) $(LDFLAGS) $(KIWILIB_OBJ) $< -o $@

build/kiwilib/src/%.c.o: kiwilib/src/%.c
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/kiwilib/src/%.S.o: kiwilib/src/%.S
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

build/programs/%/.main.o:
	mkdir -p "$(dir $@)"
	$(CC) $(CFLAGS) $(CPPFLAGS) -c programs/$*/$*.c -o $@

clean:
	rm -rf build bin
