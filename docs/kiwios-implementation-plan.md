# KiwiOS Implementation Plan — From Kernel to Full Userspace

_Last updated: 2026-04-21 (repo-verified)_

## Where You Are Now (repo-verified)

Your kernel boots via Limine, has framebuffer console + keyboard input, AHCI disk, GPT partitions, a 4 KiB block cache, VFS path resolution, KiFS root mount support, FAT read-only mount support, syscalls, ring 3 processes, a userspace `/init` path, and a timer-driven scheduler. The current boot disk is still KiFS-rooted.

What is still missing is the later storage/device model: KiFS write completion, host-side image tooling, a `/dev` namespace, multiple mountpoints, removable-media plumbing, and FAT write support.

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
12. **Phase 12 — userspace shell + `/init` boot path**
13. **Phase 13 — preemptive scheduler (timer-driven)**
    - After Phase 13: short userspace-shell usability pass for path handling, argv, line editing, and moving core tools into userspace as supporting syscalls land
    - Parallel platform workstream: **ACPI table parsing + MADT/FADT/HPET groundwork**
14. **Phase 14 — FAT support (read-only first)**
15. **Phase 15 — KiFS write support**
16. **Phase 16 — host-side KiFS tooling (`mkfs.kifs`, `kifs_cp`)**
17. **Phase 17 — device namespace + mount table + removable media**
18. **Phase 18 — FAT write support**
19. **Phase 19 — cleanup/polish**
20. **Phase 20 — KXE improvements + tooling maturity**

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

### Phase 6+ (updated direction)
Process model -> KXE loader -> userspace toolchain -> pointer validation -> syscall expansion -> cooperative scheduler -> userspace shell -> preemption -> shell usability pass -> ACPI/platform drivers -> FAT read-only -> KiFS writes -> host tooling -> `/dev` + mount table + removable media -> FAT writes -> cleanup.

### Storage/device model

The intended model is:
- block drivers register disks/partitions with the kernel
- a small devfs exposes nodes like `/dev/console`, `/dev/disk0`, `/dev/disk0p1`
- the VFS keeps a real mount table, not only one root mount
- when a block device is mounted, VFS probes the device and binds the correct filesystem driver instance to that mountpoint

That means a future USB flash drive formatted as KiFS should mount through the KiFS driver, while a FAT-formatted one should mount through the FAT driver. The filesystem choice is per-mounted device, not hardcoded globally.

### Filesystem namespace (target v1)

Keep it very small:
- `/bin` for all normal executables
- `/dev` for devfs
- `/mnt` for extra/removable mounts
- `/root` as the only real home directory at first
- `/home` as the future parent directory for user homes
- `/tmp` for scratch files

Do not add `/usr`, `/var`, `/proc`, `/sys`, `/lib`, `/sbin`, or `/users`
until they solve a real problem.

Migration target once KiFS directory tooling exists:
- boot via `/bin/init`
- userspace shell at `/bin/sh`
- normal utilities and bring-up tools under `/bin/*`

## Remaining from current repo state

- [ ] Phase 15 — KiFS write support and base directory tree creation
- [ ] Phase 16 — host-side tooling that can create/populate directories
- [ ] Phase 17 — devfs, mount table, and removable-media flow
- [ ] Phase 18 — FAT write support
- [ ] Phase 19 — cleanup/polish
- [ ] Phase 20 — KXE/tooling maturity
- [ ] migrate current root-level `/init`, shell, and test programs into `/bin/*`

## Quick reality checklist for the next coding session

- [ ] extend the generic VFS API past `lookup/readdir/read/stat`
- [ ] implement KiFS create/write/mkdir/unlink for Phase 15
- [ ] create the standard KiFS directory tree: `/bin`, `/dev`, `/mnt`, `/root`, `/home`, `/tmp`
- [ ] make host-side tooling able to populate directories, not only root-level entries
- [ ] switch boot/userspace programs from `/init`, `/shell`, `/hello`, etc. to `/bin/*`

