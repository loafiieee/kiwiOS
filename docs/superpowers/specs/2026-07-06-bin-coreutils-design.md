# Design: Port shell built-ins to standalone /bin coreutils

Date: 2026-07-06
Status: Approved (design), implementation in progress

## Goal

Turn the KiwiOS userspace shell's built-in commands into real, standalone
`/bin` programs, moving toward a `/bin`-primary command model. Keep the
built-ins as a fallback until each `/bin` program is runtime-confirmed, then
remove the built-ins in a follow-up.

This is Phase 21a of `KiwiOS_Implementation_Plan.md` ("Build a real Kiwi
command set"). It builds directly on the userland surface validated by the
nano port (argv, dirent, stat, stdio, POSIX wrappers in `kiwilib`).

## Scope

**Port to `/bin` (15 programs):**
`echo`, `pwd`, `ls`, `stat`, `cat`, `touch`, `mkdir`, `rmdir` (new; pairs with
mkdir), `rm`, `cp`, `mv`, `mount`, `rescan`, `which`, `clear`.

**Stay built-in permanently** (change shell state / shell-specific):
`cd`, `exit`, `help`.

**Deferred — require new kernel syscalls, out of scope here:**
- `sleep` — `nanosleep`/`sleep` in kiwilib are no-op stubs; there is no kernel
  timer/sleep syscall yet.
- `ps` — no process-list syscall.
- `kill` — no signals.
- `dmesg` — no kernel-log-access syscall.

These are recorded so they are not forgotten, but building them is kernel work,
not a built-in port.

## Approach

### Program structure — each utility fully standalone (chosen)

Each program is `userspace/programs/<cmd>/<cmd>.c` with
`int main(int argc, char** argv)`, using only `kiwilib` POSIX wrappers:
`opendir`/`readdir`/`closedir`, `stat`, `open`/`read`/`write`/`close`,
`printf`/`fputs`, `mkdir`, `unlink`, `rmdir`, `rename`, `getcwd`. No new shared
library unit. This matches the existing one-`.c`-per-program pattern
(`programs/hello/hello.c`, etc.) and keeps each program independently
understandable and testable. Duplication is minimal because the programs are
tiny.

Rejected: a shared `common.c` helper lib (adds a build dependency for little
gain at this size); a busybox-style multicall binary (the plan explicitly marks
this "optional later, not a requirement").

Relative paths in argv resolve against the process cwd, which is inherited from
the shell on spawn (already implemented). So programs pass argv paths straight
to `open`/`stat`/`opendir`.

### Shell routing — prefer /bin, fall back to built-in (chosen)

Restructure the shell dispatch (`userspace/programs/shell/shell.c`):

1. Handle `cd` / `exit` / `help` first — these can never be external.
2. Otherwise call `run_program(cmd, cursor)`, modified to *return* a status:
   - success (program found, spawned, waited) → return 0 (do not fall back)
   - not found (`resolve_program_path` fails) → return a distinct
     `CMD_NOT_FOUND` code, and do NOT print "command not found" itself
3. On `CMD_NOT_FOUND`, consult a built-in fallback table (the existing `cmd_*`
   functions: `cmd_ls`, `cmd_cat`, ...). If one matches, run it.
4. If neither an external program nor a built-in matches, print
   "shell: command not found".

This makes `/bin` the primary path (typing `ls` runs `/bin/ls`) while the
built-ins remain a safety net. When the user confirms all `/bin` programs work,
the follow-up simply deletes the fallback table and the `cmd_*` functions;
`run_program`'s not-found path then prints "command not found" as before.

`clear` as a program emits ANSI `\x1b[H\x1b[2J` (the console already supports
these), rather than needing a console-clear syscall.

## Behavior parity

Each program mirrors its current built-in's output and usage messages so that
nothing visibly changes when routing flips to `/bin`. Reference implementations
are the existing `cmd_*` functions in `shell.c`. Exit codes: 0 on success,
non-zero on error (usage error, failed syscall).

## Build & install wiring

- Add each program name to `PROGRAMS` in `userspace/Makefile` (the
  `PROGRAM_RULES` template compiles `programs/<name>/<name>.c` → ELF → KXE).
- Add each `userspace/bin/<name>:/bin/<name>` to the install list in
  `compile.sh` (both the `/bin` list and the legacy root-level fallback list).

## Testing / verification

- **Build:** `make -C userspace all` must build every new program to a KXE
  (compile-verified here).
- **Runtime (user):** boot with `./compile.sh`, run each command, and confirm
  it matches the old built-in behavior (both via `<cmd>` routing to `/bin` and
  via explicit `/bin/<cmd>`). Test relative and absolute paths, and usage/error
  cases.
- Once all `/bin` programs are confirmed, a follow-up change removes the shell
  built-ins and their fallback table.

## Out of scope / follow-ups

- Removing the built-ins (after user confirmation).
- Pipes/redirection, globbing, and multi-arg operations beyond current built-in
  behavior (e.g. `cp` of multiple sources) unless the built-in already did it.
- The deferred commands (`sleep`, `ps`, `kill`, `dmesg`) and their kernel
  syscalls.
