# KiwiOS Implementation Plan — From Kernel to Full Userspace

_Last updated: 2026-04-15 (repo-verified)_

## Where You Are Now (repo-verified)

Your kernel boots via Limine, has framebuffer console + keyboard input, AHCI disk, GPT partitions, a 4 KiB block cache, VFS path resolution, KiFS mount + read support (`ls`, `stat`, `cat`), and an in-kernel shell. `mkfs.kifs` exists and can format/populate a test KiFS partition.

Everything still runs in ring 0. There are no syscalls, no ring 3 processes, and no scheduler/process model yet.

## Immediate correction to keep in this plan

Before SYSCALL/SYSRET work, keep **Phase 1** as mandatory:
- Current GDT has **user code at 0x18** and **user data at 0x20**.
- SYSRET-friendly layout needs **user data at 0x18** and **user code at 0x20**.

## Build order (authoritative)

1. **Phase 1 — GDT reorder**
2. **Phase 2 — MSR helpers + SYSCALL setup**
3. **Phase 3 — syscall entry stub (asm)**
4. **Phase 4 — syscall dispatcher + `sys_write`/`sys_exit`**
5. **Phase 5 — first ring 3 hardcoded user test**
   - Milestone: _"Hello from userspace!"_
6. **Phase 6 — process struct/lifecycle**
7. **Phase 7 — KXE format + loader**
8. **Phase 8 — elf2kxe + userspace toolchain**
   - Milestone: `exec /bin/hello` from disk
9. **Phase 9 — user pointer validation**
10. **Phase 10 — more syscalls (`read/open/close/stat/brk/exec/yield/...`)**
11. **Phase 11 — cooperative scheduler**
12. **Phase 12 — userspace shell + `/bin/init` boot path**
13. **Phase 13 — preemptive scheduler (timer-driven)**
14. **Phase 14 — KiFS write support**
15. **Phase 15 — host-side KiFS tooling (`mkfs.kifs`, `kifs_cp`)**
16. **Phase 16 — cleanup/polish**

## Phase details (kept from current plan)

### Phase 1 — GDT Reorder
Swap user code/user data entries so selectors become:
- 0x18 = user data
- 0x20 = user code

### Phase 2 — SYSCALL plumbing
Add `rdmsr/wrmsr`, configure `EFER`, `STAR`, `LSTAR`, `SFMASK`, and call `syscall_init()` from `kmain` after GDT/TSS setup.

### Phase 3 — syscall_entry asm
Implement register save/restore + stack switch + C dispatch + SYSRET path.

### Phase 4 — syscall dispatcher
Introduce `syscall_dispatch`, `sys_write` (console fd 1/2 first), and basic `sys_exit`.

### Phase 5 — first ring 3 execution
Hardcoded userspace payload + user page table + user stack + `iretq` entry test.

### Phase 6+ (unchanged direction)
Process model -> KXE loader -> userspace toolchain -> pointer validation -> syscall expansion -> cooperative scheduler -> userspace shell -> preemption -> KiFS writes -> host tooling -> cleanup.

## Quick reality checklist for the next coding session

- [ ] GDT user segments reordered (0x18 data / 0x20 code)
- [ ] `syscall_init()` implemented and called
- [ ] `syscall_entry.asm` wired into build
- [ ] C dispatcher handles syscall numbers 0/1
- [ ] Hardcoded ring 3 user test prints via syscall path

