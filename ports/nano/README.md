# GNU nano Port

This is the KiwiOS source-port track for GNU nano. The goal is not to run
a Linux nano binary; the first target is a static native KXE built from
portable C source against `kiwilib`.

Pinned upstream:
- Package: GNU nano
- Version: 9.1
- Source: `https://www.nano-editor.org/dist/v9/nano-9.1.tar.xz`

Near-term build policy:
- Keep full nano rebuilds outside the default `compile.sh` path because
  configure is slow. If `ports/nano/build/nano` already exists,
  `compile.sh` restages it into `userspace/bin/nano` and installs it as
  `/bin/nano`.
- Rebuild nano explicitly with `bash ports/nano/build.sh`, or set
  `KIWI_BUILD_NANO=1` when running `compile.sh`.
- Disable optional helpers first: spell checking, mouse support, external
  helpers, and dynamic loading.
- Treat configure/build failures as compatibility work items. Do not hide
  missing kernel or libc behavior behind fake success unless the behavior
  is explicitly optional for the first port.
- Produce a static ELF first, then convert it with `tools/elf2kxe`.

Important compatibility areas:
- `fcntl` descriptor flags and close-on-exec behavior
- `termios`, `ioctl(TIOCGWINSZ)`, raw input, and Ctrl-key handling
- curses/terminfo-compatible screen operations
- temp-file, rename, truncate, and durable save workflows
- `errno` values that are more specific than generic failure
- signals and process/job-control behavior for Ctrl+C and Ctrl+Z

Current status:
- `libctest`, `termtest`, `cursestest`, and `/bin/edit` remain the local
  regression targets for the libc/terminal layer.
- `ports/nano/build.sh` fetches the pinned source, builds a static
  Kiwi-target ELF, converts it to KXE at `ports/nano/build/nano`, and
  copies it to `userspace/bin/nano`.
