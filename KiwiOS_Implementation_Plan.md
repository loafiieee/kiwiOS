# KiwiOS Implementation Plan — From Kernel to Full Userspace

## Where You Are Now

Your kernel boots via Limine, has a framebuffer console, PS/2 keyboard,
AHCI disk driver, block cache, GPT partition support, a VFS layer, and
KiFS read-only (ls, cat, stat work). You have a working in-kernel shell.
mkfs.kifs can format a partition and create test files. FAT is present
only as a stub in the VFS driver registry right now.

Everything runs in ring 0. There are no processes, no syscalls, no ring 3.

## Where We're Going

The kernel boots, mounts KiFS as root, loads `/bin/init` as a ring 3
process. Init launches a shell. The shell launches programs. Everything
in userspace communicates with the kernel through syscalls. A preemptive
scheduler time-slices between processes. FAT support is added as a
secondary compatibility filesystem for removable media and easy file
exchange with the host. KXE starts as a small, fixed-address native
executable format, then grows features like relocations, integrity
metadata, and better tooling once the basic userspace pipeline works.

---

## Phase 1 — GDT Reorder

**What:** Swap user code and user data entries in the GDT.

**Why:** The SYSRET instruction does arithmetic on a value in the STAR MSR
to compute CS and SS. It needs user data at selector 0x18 and user code
at selector 0x20. You currently have them reversed.

**Files to change:**
- `src/arch/x86/gdt.c` — swap entries 3 and 4

**New GDT layout:**
```
Slot 0: Null         (0x00)
Slot 1: Kernel code  (0x08)  access=0x9A, gran=0xAF  — unchanged
Slot 2: Kernel data  (0x10)  access=0x92, gran=0xCF  — unchanged
Slot 3: User data    (0x18)  access=0xF2, gran=0xCF  — was slot 4
Slot 4: User code    (0x20)  access=0xFA, gran=0xAF  — was slot 3
Slot 5-6: TSS        (0x28)                          — unchanged
```

**How to verify:** Build and boot. Everything works exactly as before.
Nothing references user segments yet.

---

## Phase 2 — MSR Helpers + SYSCALL Setup

**What:** Configure the CPU to handle the SYSCALL instruction.

**Why:** When a user program executes `SYSCALL`, the CPU needs to know:
where to jump (LSTAR), which segments to load (STAR), which flags to
clear (SFMASK), and that the instruction is even enabled (EFER).

**Files to create/change:**

### 2a. Add rdmsr/wrmsr to io.h

```c
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "c"(msr),
                 "a"((uint32_t)value),
                 "d"((uint32_t)(value >> 32)));
}
```

### 2b. Create `src/arch/x86/syscall.c`

This function configures four MSRs:

- **EFER** (0xC0000080): Set bit 0 (SCE) to enable SYSCALL/SYSRET
- **STAR** (0xC0000081): Kernel CS base in bits 47:32, SYSRET base in bits 63:48
  - STAR[47:32] = 0x08 (kernel code). SYSCALL loads CS=0x08, SS=0x10.
  - STAR[63:48] = 0x10 (sysret base). SYSRET loads SS=0x10+8=0x18, CS=0x10+16=0x20.
- **LSTAR** (0xC0000082): Address of syscall_entry (the asm stub)
- **SFMASK** (0xC0000084): Bits to clear in RFLAGS on SYSCALL.
  Clear IF (bit 9) so interrupts are disabled on entry.
  Clear DF (bit 10) so string ops go forward.
  Clear TF (bit 8) so single-stepping doesn't fire.

### 2c. Create `src/arch/x86/syscall.h`

```c
void syscall_init(void);
```

### 2d. Call syscall_init() from kmain

After GDT/TSS setup, before interrupts are enabled.

**How to verify:** Boot. Nothing visible changes — SYSCALL is configured
but nobody is calling it yet. If it triple-faults, the MSR values are wrong.

---

## Phase 3 — Syscall Entry Stub (Assembly)

**What:** The assembly function that the CPU jumps to when SYSCALL executes.

**Why:** SYSCALL does very little automatically. It saves the return address
in RCX, saves RFLAGS in R11, loads kernel CS/SS, and jumps to LSTAR. That's
it. It does NOT switch stacks. You must do that yourself.

**Files to create:**

### 3a. `src/arch/x86/syscall_entry.asm`

This stub must:
1. Save the user's RSP somewhere (a global variable for now — single CPU)
2. Load the kernel stack pointer (from another global)
3. Push all registers onto the kernel stack as a frame
4. Call the C dispatcher with a pointer to that frame
5. Restore registers from the frame
6. Execute SYSRET to return to ring 3

Key details:
- User RSP → saved in `[user_rsp_tmp]` global
- Kernel RSP ← loaded from `[kernel_rsp_current]` global
- The frame on the kernel stack holds: syscall number (RAX), all argument
  registers, callee-saved registers, user RIP (RCX), user RFLAGS (R11),
  user RSP
- The C function's return value in RAX becomes the syscall return value
- SYSRET loads RIP from RCX and RFLAGS from R11

Globals exported from asm (set by kernel before entering userspace):
```
kernel_rsp_current: dq 0   ; top of current process's kernel stack
user_rsp_tmp:       dq 0   ; scratch space for user RSP during entry
```

### 3b. Update GNUmakefile

Your Makefile already handles .asm files with nasm. Just make sure the
new file is in the src tree and it'll be found by the `find` command.

**How to verify:** Builds without errors. Not called yet.

---

## Phase 4 — Syscall Dispatcher (C)

**What:** The C function that the asm stub calls to handle syscalls.

**Why:** The asm stub is just plumbing. The actual "what does sys_write do"
logic lives in C.

**Files to create:**

### 4a. `src/core/syscall.c`

Define the frame struct matching what the asm stub pushes:
```c
typedef struct {
    uint64_t syscall_num;   // RAX
    uint64_t rdi, rsi, rdx, r10, r8, r9;  // args
    uint64_t r15, r14, r13, r12, rbx, rbp; // callee-saved
    uint64_t user_rip;      // from RCX
    uint64_t user_rflags;   // from R11
    uint64_t user_rsp;      // saved manually
} syscall_frame_t;
```

Implement the dispatcher:
```c
int64_t syscall_dispatch(syscall_frame_t *f) {
    switch (f->syscall_num) {
        case 0: sys_exit((int)f->rdi); return 0;
        case 1: return sys_write((int)f->rdi, (void*)f->rsi, f->rdx);
        default: return -1; // ENOSYS
    }
}
```

### 4b. Implement sys_write (v0.1 — console only)

For now, fd 1 and 2 both write to the kernel console:
```c
int64_t sys_write(int fd, const void* buf, uint64_t len) {
    if (fd != 1 && fd != 2) return -1;
    // buf is a user pointer — must validate it's in user address space
    // For the first test, we can skip validation and just print
    const char* s = (const char*)buf;
    for (uint64_t i = 0; i < len; i++) {
        putc_fb(NULL, s[i]);
    }
    return (int64_t)len;
}
```

### 4c. Implement sys_exit (v0.1 — just halt)

For the first test, sys_exit can just print "Process exited" and halt
or return to the kernel shell. We'll make it proper later.

### 4d. `src/core/syscall.h`

Declare syscall_dispatch, sys_write, sys_exit.

**How to verify:** Builds. Not called yet.

---

## Phase 5 — First Ring 3 Test (Hardcoded)

**What:** Embed a tiny program in the kernel, set up a user page table
and stack, and IRETQ into it.

**Why:** This tests the entire ring transition without needing the disk,
KXE format, or any toolchain. If "Hello from userspace!" appears on
screen, everything works.

**Approach:**

### 5a. Write the test program as raw machine code

A tiny x86-64 program that does:
```
mov rax, 1          ; SYS_WRITE
mov rdi, 1          ; fd = stdout
lea rsi, [rel msg]  ; pointer to string
mov rdx, 22         ; length
syscall
mov rax, 0          ; SYS_EXIT
xor rdi, rdi        ; status = 0
syscall

msg: db "Hello from userspace!", 10
```

Assemble this into a byte array and embed it in a C file, or load it as
a Limine module. The Limine module approach is cleaner — add it to
limine.conf and read it via boot_module_response().

### 5b. Create a user page table

```c
page_table_t* user_pt = vmm_create_page_table();
// This already copies the kernel's upper-half mappings
```

### 5c. Map the test program at USER_BASE (0x400000)

Allocate a physical page, copy the test program bytes into it (via HHDM),
then map it into the user page table:
```c
uint64_t code_phys = (uint64_t)pmm_alloc();
void* code_virt = phys_to_virt(code_phys);
memcpy(code_virt, test_program_bytes, test_program_size);
vmm_map_page(user_pt, 0x400000, code_phys, PAGE_PRESENT | PAGE_USER);
```

### 5d. Map a user stack

```c
uint64_t stack_phys = (uint64_t)pmm_alloc_pages(16); // 64 KiB
for (int i = 0; i < 16; i++) {
    vmm_map_page(user_pt, USER_STACK_TOP - (16-i)*PAGE_SIZE,
                 stack_phys + i*PAGE_SIZE,
                 PAGE_PRESENT | PAGE_USER | PAGE_WRITE);
}
```

### 5e. Set up kernel stack for this "process"

Allocate 2 pages for a kernel stack. Set TSS.RSP0 to its top.
Set the `kernel_rsp_current` global (used by the syscall entry stub).

### 5f. Switch page table and IRETQ to ring 3

```c
vmm_switch_page_table(user_pt);

// Build fake interrupt frame and iretq
asm volatile(
    "mov %0, %%rsp\n"       // temporary stack for the frame
    "pushq $0x1b\n"         // SS = user data (0x18 | RPL3)
    "pushq %1\n"            // RSP = user stack top
    "pushq $0x202\n"        // RFLAGS = IF enabled
    "pushq $0x23\n"         // CS = user code (0x20 | RPL3)
    "pushq %2\n"            // RIP = entry point
    "iretq\n"
    :
    : "r"(some_temporary_stack),
      "r"(USER_STACK_TOP),
      "r"(0x400000ULL)
    : "memory"
);
```

Wait — the segment selectors for IRETQ:
- CS = 0x20 | 3 = 0x23 (user code, RPL 3)
- SS = 0x18 | 3 = 0x1B (user data, RPL 3)

### 5g. What happens

1. IRETQ pops the frame, CPU switches to ring 3, jumps to 0x400000
2. Test program executes SYSCALL
3. CPU jumps to syscall_entry (LSTAR), switches to ring 0
4. Stub saves registers, calls syscall_dispatch
5. sys_write prints "Hello from userspace!"
6. Stub does SYSRET, back to ring 3
7. Test program calls sys_exit
8. Kernel prints "Process exited" and halts (or returns to shell)

**How to verify:** You see "Hello from userspace!" on the screen.
This is the most important milestone. Everything after this is incremental.

---

## Phase 6 — Process Structure

**What:** Define the process_t struct and basic lifecycle functions.

**Why:** Phase 5 was a hack — one hardcoded "process" with no structure.
Now we formalize it.

**Files to create:**

### 6a. `src/core/process.h`

```c
#define PROC_MAX 64
#define PROC_NAME_MAX 64

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE,
} proc_state_t;

typedef struct process {
    uint32_t pid;
    uint32_t ppid;
    char name[PROC_NAME_MAX];
    proc_state_t state;
    int32_t exit_code;

    page_table_t* page_table;

    // Saved CPU context
    uint64_t saved_rip, saved_rsp, saved_rflags;
    uint64_t saved_rax, saved_rbx, saved_rcx, saved_rdx;
    uint64_t saved_rsi, saved_rdi, saved_rbp;
    uint64_t saved_r8, saved_r9, saved_r10, saved_r11;
    uint64_t saved_r12, saved_r13, saved_r14, saved_r15;

    // Kernel stack
    uint64_t kernel_stack_phys;
    uint64_t kernel_stack_top;

    // Heap management
    uint64_t brk_base;
    uint64_t brk_current;

    // Scheduling
    struct process* next; // run queue linked list
} process_t;

process_t* process_create(const char* name);
void process_destroy(process_t* proc);
process_t* process_current(void);
process_t* process_by_pid(uint32_t pid);
```

### 6b. `src/core/process.c`

Implement the process table as a simple array:
```c
static process_t g_procs[PROC_MAX];
static process_t* g_current = NULL;
static uint32_t g_next_pid = 1;
```

`process_create`:
- Find unused slot in g_procs
- Assign PID
- Create page table (vmm_create_page_table)
- Allocate kernel stack (2 pages)
- Initialize state to PROC_READY

`process_destroy`:
- Free all user pages (walk page table, free anything in user range)
- Free kernel stack
- Free page table
- Mark slot PROC_UNUSED

**How to verify:** Refactor Phase 5 to use process_create instead of
ad-hoc allocation. Same test, cleaner code.

---

## Phase 7 — KXE Format + Loader

**What:** Define the KXE binary format and write a kernel loader.

**Why:** Instead of embedding test programs as byte arrays, load real
compiled programs from disk.

**Files to create:**

### 7a. `src/core/kxe.h`

Define the KXE header struct, section table entry, flags, magic.
See `kxe.txt` for the full format definition and future growth path.

Key structures:
- `kxe_header_t` (64 bytes): magic, version, flags, section count, entry point
- `kxe_section_t` (48 bytes): name, vaddr, vsize, file offset, file size, flags

### 7b. `src/core/kxe.c`

`kxe_validate(const uint8_t* header_page)`:
- Check magic "KXE\0"
- Check version
- Check flags (reject unknown bits)
- Check section count ≤ 16
- Verify entry point falls in an EXEC section
- Verify no section overlaps
- Verify all vaddrs in user range

`kxe_load(const char* path)`:
- vfs_resolve the path, get vnode
- Read first 4K page (header + section table)
- Validate
- process_create
- For each section:
  - Allocate physical pages for ceil(vsize / PAGE_SIZE)
  - Map into process page table with correct flags (R/W/X → PAGE_USER etc.)
  - Read file data into those pages via HHDM
  - Zero-fill any remainder (BSS)
- Set up user stack (same as Phase 5)
- Set brk_base to first page after last section
- Set entry point from header
- Return the process (ready to run)

### 7c. Add "exec" shell command

```c
// In shell.c
static void cmd_exec(struct limine_framebuffer* fb, const char* args) {
    const char* path = trim_spaces(args);
    if (!path || !*path) { print(fb, "Usage: exec <path>\n"); return; }

    process_t* proc = kxe_load(path);
    if (!proc) { print(fb, "exec: load failed\n"); return; }

    // For now: directly run it (no scheduler yet)
    run_process(proc);
}
```

This `exec` command is a temporary bring-up/debug path for the in-kernel
shell. It is not meant to be the final normal way users launch programs.

Once the real userspace shell exists, normal command execution should
work like a Unix-like shell:
- `hello` searches a PATH such as `/bin`
- `./hello` runs a relative path from the current directory
- `/bin/hello` runs an absolute path directly

`exec` may still remain as a low-level debug tool, but it should not be
the required long-term interface for launching programs.

**How to verify:** Can't test yet — need KXE files on disk. Move to Phase 8.

---

## Phase 8 — elf2kxe + Userspace Toolchain

**What:** Build the host-side converter and minimal C library so you can
compile real programs.

**Why:** You need actual KXE binaries to test the loader.

**Files to create (host-side tools, NOT kernel code):**

### 8a. `tools/elf2kxe.c`

A Linux program that reads an ELF64 executable and outputs a KXE file.

Steps:
1. Read ELF header, verify ELF64 / little-endian / x86-64 / ET_EXEC
2. Read program headers, find PT_LOAD segments
3. Map ELF segments to KXE sections based on flags:
   - PF_R | PF_X → .text (READ | EXEC)
   - PF_R → .rodata (READ)
   - PF_R | PF_W → .data (READ | WRITE), plus .bss if memsz > filesz
4. Write KXE header + section table (pad to 4096)
5. Write section data (copy from ELF, align each to 16 bytes)
6. Compute and write header checksum

Build with: `gcc -o tools/elf2kxe tools/elf2kxe.c -Wall`

Keep this first version intentionally simple. The spec in `kxe.txt`
already reserves room for future features, but Phase 8 should only build
what is needed to get static user programs onto disk and loaded.

### 8b. `userspace/kiwilib/src/crt0.asm`

Entry point for all user programs:
```asm
global _start
extern main

_start:
    xor rbp, rbp        ; clear frame pointer
    xor edi, edi         ; argc = 0
    xor esi, esi         ; argv = NULL
    call main
    mov edi, eax         ; exit code = main's return value
    mov eax, 0           ; SYS_EXIT
    syscall
    ud2                  ; should never reach here
```

### 8c. `userspace/kiwilib/src/syscall.asm`

Generic syscall wrapper:
```asm
global kiwi_syscall
kiwi_syscall:
    mov rax, rdi     ; syscall number
    mov rdi, rsi     ; arg1
    mov rsi, rdx     ; arg2
    mov rdx, rcx     ; arg3
    mov r10, r8      ; arg4
    mov r8,  r9      ; arg5
    mov r9, [rsp+8]  ; arg6
    syscall
    ret
```

### 8d. `userspace/kiwilib/include/kiwi_syscall.h`

```c
#include <stdint.h>
int64_t kiwi_syscall(uint64_t num, ...);

// Convenience wrappers
static inline int64_t sys_write(int fd, const void* buf, uint64_t len) {
    return kiwi_syscall(1, fd, buf, len);
}
static inline void sys_exit(int code) {
    kiwi_syscall(0, code);
    __builtin_unreachable();
}
```

### 8e. `userspace/kiwilib/src/stdio.c`

Minimal puts / putchar using sys_write.

### 8f. `userspace/kiwilib/src/string.c`

memcpy, memset, strlen — same as kernel versions but for userspace.

### 8g. `userspace/user.lds`

Linker script targeting fixed user addresses:
```ld
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment*) }
}
```

### 8h. `userspace/programs/hello/hello.c`

```c
#include "kiwi_syscall.h"

int main(void) {
    const char* msg = "Hello from userspace!\n";
    sys_write(1, msg, 22);
    return 0;
}
```

### 8i. Build pipeline

```bash
# Compile kiwilib
cc -ffreestanding -nostdlib -mno-red-zone -c crt0.asm -o crt0.o
cc -ffreestanding -nostdlib -mno-red-zone -c syscall.asm -o syscall.o
cc -ffreestanding -nostdlib -mno-red-zone -c stdio.c -o stdio.o

# Compile hello
cc -ffreestanding -nostdlib -mno-red-zone -Ikiwilib/include \
   -c hello.c -o hello.o

# Link to ELF
ld -T user.lds -nostdlib crt0.o syscall.o hello.o -o hello.elf

# Convert to KXE
./tools/elf2kxe hello.elf hello.kxe
```

### 8j. Get hello.kxe onto the disk image

Option A: Modify kifs_mkfs to embed it (like it does with hello.txt).
Option B: Write a host-side kifs_cp tool that injects files.
Option C: Load it as a Limine module for now, bypass disk entirely.

For the first test, Option C (Limine module) or Option A (embed in mkfs)
is easiest.

**How to verify:** `exec /bin/hello` in the shell prints
"Hello from userspace!" and returns cleanly.

---

## Phase 9 — User Pointer Validation

**What:** Before accessing any pointer from userspace, verify it's valid.

**Why:** A malicious or buggy program could pass a kernel address as a
buffer pointer. Without validation, sys_write would happily read kernel
memory and print it.

**Implementation:**

```c
bool validate_user_buffer(uint64_t addr, uint64_t len, bool needs_write) {
    if (len == 0) return true;
    if (addr < 0x1000) return false;                    // null page guard
    if (addr > 0x00007FFFFFFFFFFFULL) return false;     // above user range
    if (addr + len < addr) return false;                // overflow
    if (addr + len > 0x00007FFFFFFFFFFFULL) return false;

    // Optionally: walk page table to verify pages are mapped
    // For v0.1, the address range check is sufficient — page faults
    // will catch unmapped pages, and the kernel fault handler can
    // kill the process instead of panicking.

    return true;
}
```

Add this check to every syscall that takes a user pointer.

---

## Phase 10 — More Syscalls

**What:** Implement the syscalls needed for real programs.

### sys_brk (syscall 5) — heap management

```c
int64_t sys_brk(uint64_t addr) {
    process_t* p = process_current();
    if (addr == 0) return p->brk_current;  // query

    if (addr < p->brk_base) return p->brk_current;  // can't shrink below base
    if (addr > USER_HEAP_MAX) return p->brk_current; // hard limit

    // Map new pages if growing
    uint64_t old_top = PAGE_ALIGN_UP(p->brk_current);
    uint64_t new_top = PAGE_ALIGN_UP(addr);

    for (uint64_t va = old_top; va < new_top; va += PAGE_SIZE) {
        uint64_t phys = (uint64_t)pmm_alloc();
        if (!phys) return p->brk_current; // out of memory
        vmm_map_page(p->page_table, va, phys, PAGE_PRESENT | PAGE_USER | PAGE_WRITE);
    }

    p->brk_current = addr;
    return addr;
}
```

### sys_read (syscall 2) — keyboard input

For fd 0 (stdin): read from keyboard buffer. If no data, block the
process (mark BLOCKED, schedule another). When keyboard interrupt
puts data in the buffer, unblock.

For fd pointing to a file: read via VFS.

### sys_open (syscall 3)

Resolve path via VFS. Allocate an FD slot in the process. Store the
vnode and initial offset.

### sys_close (syscall 4)

Free the FD slot, release the vnode.

### sys_seek (syscall 11)

Update the offset in the FD entry.

### sys_stat (syscall 10)

Resolve path, call vnode stat, copy results to user buffer.

### sys_getpid (syscall 6)

Return current process PID. Trivial.

### sys_exec (syscall 7)

Replace current process's address space:
1. Validate path from userspace
2. Load KXE (similar to kxe_load but reuse existing process)
3. Free old user pages
4. Map new sections
5. Reset stack, brk
6. Set RIP to new entry point
7. Return to userspace (SYSRET with new RIP)

### sys_yield (syscall 12)

Voluntarily give up CPU. Calls schedule().

---

## Phase 11 — Cooperative Scheduler

**What:** Multiple processes exist and take turns running.

**Why:** You need this so the shell can launch a program, wait for it,
and resume.

**Implementation:**

### 11a. Run queue

```c
static process_t* g_run_queue_head = NULL;
static process_t* g_run_queue_tail = NULL;

void scheduler_add(process_t* p) {
    p->state = PROC_READY;
    p->next = NULL;
    if (g_run_queue_tail) g_run_queue_tail->next = p;
    else g_run_queue_head = p;
    g_run_queue_tail = p;
}
```

### 11b. schedule()

Called from sys_yield, sys_exit, or when a process blocks:

```c
void schedule(void) {
    process_t* prev = g_current;

    // Put current process back in queue if still runnable
    if (prev && prev->state == PROC_RUNNING) {
        prev->state = PROC_READY;
        scheduler_add(prev);
    }

    // Pick next
    g_current = g_run_queue_head;
    if (g_current) {
        g_run_queue_head = g_current->next;
        if (!g_run_queue_head) g_run_queue_tail = NULL;
        g_current->next = NULL;
        g_current->state = PROC_RUNNING;
    }

    if (!g_current) {
        // No processes — idle loop
        while (!g_run_queue_head) { asm volatile("hlt"); }
        // Retry
        schedule();
        return;
    }

    // Context switch
    context_switch_to(g_current);
}
```

### 11c. Context switch

Save all registers of current process into its process_t.
Load all registers of new process from its process_t.
Switch page table (write CR3).
Update TSS.RSP0 to new process's kernel stack.
Update kernel_rsp_current global.

### 11d. sys_exit becomes real

```c
void sys_exit(int code) {
    process_t* p = process_current();
    p->state = PROC_ZOMBIE;
    p->exit_code = code;
    // TODO: wake parent if waiting
    schedule(); // never returns to this process
}
```

### 11e. Blocking I/O

When sys_read on stdin has no data:
- Set process state to PROC_BLOCKED
- Add to keyboard wait queue
- Call schedule()

When keyboard interrupt delivers data:
- Move process from wait queue to run queue
- It will be scheduled eventually

**How to verify:** Launch two programs. They take turns via sys_yield.
Shell launches a program, program exits, shell resumes.

---

## Phase 12 — Userspace Shell

**What:** Move the shell from kernel space to a user program.

**Why:** This is the goal — the shell is just another program.

### 12a. Port shell logic to userspace

The userspace shell is a C program using kiwilib:
- Read keyboard input via sys_read(0, ...)
- Print output via sys_write(1, ...)
- Parse commands
- Built-in commands (cd, exit) handled directly
- Non-built-ins run by normal shell rules:
  - bare names search a PATH like `/bin`
  - `./name` and `../name` use relative paths
  - `/path/name` uses the absolute path directly

The old kernel-shell `exec` command remains optional debug tooling, not
the normal user-facing command syntax.

### 12b. Modify kmain

Instead of calling shell_loop(), kmain does:
```c
process_t* init = kxe_load("/bin/init");
// init launches /bin/shell
run_first_process(init);
scheduler_idle_loop(); // never returns
```

### 12c. Init program

/bin/init is dead simple for v0.1:
```c
int main(void) {
    sys_exec("/bin/shell", NULL);
    // If exec fails:
    sys_write(1, "init: failed to start shell\n", 28);
    return 1;
}
```

**How to verify:** Kernel boots, you see the shell prompt, type commands,
everything works — but now it's all in ring 3.

---

## Phase 13 — Preemptive Scheduling

**What:** The timer interrupt forces context switches.

**Why:** A program that runs forever or has an infinite loop shouldn't
freeze the system.

### 13a. Add time slice to process_t

```c
uint64_t ticks_remaining; // decremented each timer tick
```

### 13b. Modify IRQ0 handler

Your current irq0_handler sends EOI and returns. Upgrade it:

```c
void irq0_tick(void) {
    if (!g_current) return;

    g_current->ticks_remaining--;
    if (g_current->ticks_remaining == 0) {
        // Save current registers (they're on the interrupt stack frame)
        save_context_from_interrupt_frame(g_current);
        g_current->ticks_remaining = DEFAULT_TIME_SLICE;
        schedule();
        // schedule will context-switch; we don't return here normally
    }
}
```

### 13c. Save/restore from interrupt frame

The tricky part: when the timer fires during ring 3 execution, the CPU
pushes SS, RSP, RFLAGS, CS, RIP automatically. Your handler pushes the
GPRs. You need to save all of this into the current process's context,
then load the next process's context and IRETQ to it.

This is the same register save/restore as the syscall path, but triggered
involuntarily by the timer instead of voluntarily by SYSCALL.

### 13d. Configure PIT frequency

Your PIC is set up but you're not configuring the PIT (Programmable
Interval Timer) frequency. Set it to ~100 Hz (10ms per tick):

```c
#define PIT_FREQ 100
outb(0x43, 0x36);
uint16_t div = 1193180 / PIT_FREQ;
outb(0x40, div & 0xFF);
outb(0x40, (div >> 8) & 0xFF);
```

**How to verify:** Run a program with an infinite loop. The shell still
gets CPU time. Ctrl+C (once implemented) can kill the stuck program.

---

## Phase 14 — FAT Support (Read-Only First)

**What:** Implement FAT mount, directory walking, and file reads through
the existing VFS.

**Why:** KiFS is your native filesystem, but FAT is the easiest
compatibility filesystem for USB drives, shared disk images, and moving
test files to and from the host. Read-only FAT gives you immediate
practical value without taking on the complexity of FAT writes yet.

**Files to create/change:**
- `src/fs/fat/fat.c` — implement probe, mount, lookup, readdir, read, stat
- `src/fs/fat/fat.h` — optional header if the implementation gets large
- `src/vfs/vfs.c` — keep FAT in the probe order after KiFS

### 14a. FAT probe + mount

Read the boot sector and validate:
- BPB fields look sane
- sector size and cluster size are supported
- FAT count is non-zero
- total sector count is sane
- FAT type (12/16/32) can be derived correctly

Build an in-memory `fat_fs_t` with:
- bytes per sector
- sectors per cluster
- FAT start
- root directory location
- data region start
- FAT type

### 14b. Directory walking

Implement:
- root directory iteration
- `lookup(dir, name)`
- `readdir(dir, cb, user)`

For v0.1, short 8.3 names are sufficient. Long file name support can be
added later if needed.

### 14c. File reads

Implement file reads by:
1. translating file offset -> cluster index
2. following the FAT chain
3. reading sectors/clusters through the block layer
4. returning data through the VFS `read` hook

Also implement `stat` so FAT files and directories report type and size.

### 14d. Wire into shell/VFS behavior

Once FAT mount works, your existing shell commands should work without
filesystem-specific code:
- `mount`
- `ls`
- `stat`
- `cat`

That is the main payoff of having the VFS abstraction.

**How to verify:** Create or attach a FAT partition, mount it, and confirm
that `ls /`, `stat /FILE.TXT`, and `cat /FILE.TXT` work. KiFS should still
remain the preferred root filesystem when both probes are available.

---

## Phase 15 — KiFS Write Support

**What:** Create files and directories, write data.

**Why:** User programs need to create and modify files.

This follows your KiFS implementation checklist Phase 6:

### 15a. Allocation primitives

Block bitmap alloc: scan for free bit, set it, return block number.
Inode bitmap alloc: same for inodes.

### 15b. File operations

- Create file: alloc inode, init it, add dirent to parent directory
- Write to file: alloc data blocks, update extents, update size
- mkdir: create inode, alloc dir block with . and .., add dirent to parent
- unlink: remove dirent, decrement link count, orphan if zero

### 15c. Write ordering

Follow your spec's required ordering:
- Bitmap → data → metadata → dirent
- Flush at end of each operation

### 15d. Wire into VFS

Add write/create/mkdir/unlink to vnode_ops.
Add sys_write for file FDs, sys_mkdir, sys_unlink syscalls.

---

## Phase 16 — Host-Side Disk Tooling

**What:** Create a proper disk image build pipeline.

### 16a. `tools/mkfs.kifs` (host-side, runs on Linux)

A standalone C program that formats a partition in a disk image.
Similar to your in-kernel mkfs but operates on a raw file.

### 16b. `tools/kifs_cp` (host-side)

Copies files into a KiFS partition in a disk image:
```bash
./tools/kifs_cp disk.img --partition=1 hello.kxe /bin/hello
./tools/kifs_cp disk.img --partition=1 shell.kxe /bin/shell
./tools/kifs_cp disk.img --partition=1 init.kxe /bin/init
```

### 16c. Build script integration

```bash
# compile.sh additions:
make userspace          # builds all .kxe files
tools/mkfs.kifs disk_gpt.img --partition=1 --inodes=1024
tools/kifs_cp disk_gpt.img 1 userspace/bin/init /bin/init
tools/kifs_cp disk_gpt.img 1 userspace/bin/shell /bin/shell
tools/kifs_cp disk_gpt.img 1 userspace/bin/hello /bin/hello
```

---

## Phase 17 — Cleanup + Polish

- Remove the in-kernel shell (or keep it as a debug fallback)
- Proper sys_exit with parent notification
- sys_waitpid so the shell can wait for child processes
- Signal-like mechanism for Ctrl+C (kill foreground process)
- Error numbers (ENOENT, ENOMEM, etc.) instead of -1
- /dev/console or similar for stdin/stdout

---

## Phase 18 — KXE Improvements + Tooling Maturity

**What:** Evolve KXE from a minimal static image format into a stronger
long-term executable format for KiwiOS.

**Why:** Phase 7 and Phase 8 only need the smallest loader-friendly
design that gets `/bin/hello` and `/bin/init` running. Once that works,
KXE should grow carefully instead of staying permanently "good enough for
bring-up."

Use `kxe.txt` as the design source of truth. The high-value future work
items are:

- Relocations and PIE so KXE can become ASLR-ready
- Better compatibility/version metadata and clearer loader refusal rules
- Build IDs, debug-link support, symbols, unwind info, and host-side inspection tools
- Integrity features such as per-section hashes, signatures, or measured-boot hooks
- Capability / manifest metadata such as minimum ABI version or sandbox hints
- TLS and thread-aware process startup once threading exists
- Shared-library groundwork only when the rest of userspace is ready
- Optional compression or demand-paging hints if they actually help

**Rule:** Do not bloat the kernel loader just because a feature sounds
fancy. KXE should become better than generic formats for KiwiOS by being
smaller, stricter, and more OS-aware, not by copying every historical
feature from ELF or PE.

---

## Summary: What To Build In Order

```
Phase  1: GDT reorder                          (5 min)
Phase  2: MSR helpers + SYSCALL config          (30 min)
Phase  3: Syscall entry stub (asm)              (1-2 hours, careful)
Phase  4: Syscall dispatcher + sys_write/exit   (30 min)
Phase  5: Hardcoded ring 3 test                 (1-2 hours)
          ──── MILESTONE: "Hello from userspace!" ────
Phase  6: Process structure                     (1-2 hours)
Phase  7: KXE format + loader                   (2-3 hours)
Phase  8: elf2kxe + userspace toolchain         (2-3 hours)
          ──── MILESTONE: exec /bin/hello works from disk ────
Phase  9: User pointer validation               (30 min)
Phase 10: More syscalls (brk, open, read, etc.) (3-4 hours)
Phase 11: Cooperative scheduler                 (2-3 hours)
          ──── MILESTONE: multiple processes, shell launches programs ────
Phase 12: Userspace shell                       (2-3 hours)
          ──── MILESTONE: kernel only does setup, everything else is userspace ────
Phase 13: Preemptive scheduling                 (2-3 hours)
Phase 14: FAT support (read-only first)         (2-3 hours)
Phase 15: KiFS write support                    (4-6 hours)
Phase 16: Host-side disk tools                  (3-4 hours)
Phase 17: Cleanup + polish                      (ongoing)
Phase 18: KXE improvements + tooling maturity   (incremental, later)
```

The first real milestone is Phase 5. Everything before it is plumbing.
Everything after it builds on a proven foundation.
