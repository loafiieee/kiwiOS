#!/bin/bash
set -e

rm -rf bin iso_root obj kiwi.img kiwiOS.iso
bash rmidentifier.sh

make -C tools clean
make -C userspace clean
make

make -C tools all
make -C userspace all

if [ "${KIWI_BUILD_NANO:-0}" = "1" ]; then
  bash ports/nano/build.sh
elif [ -f "ports/nano/build/nano" ]; then
  mkdir -p userspace/bin
  cp -f ports/nano/build/nano userspace/bin/nano
fi

DISK_IMAGE="disk.img"
DISK_ATTACH_OK=0
DISK_INSTALL_OK=0

ensure_disk_image() {
  local img="$1"
  local size="${KIWI_DISK_SIZE:-10G}"

  if [ -f "$img" ]; then
    return 0
  fi

  echo "[*] Creating $img ($size)"
  truncate -s "$size" "$img"
}

ensure_gpt_partition() {
  local img="$1"

  if sgdisk -i 1 "$img" >/dev/null 2>&1; then
    return 0
  fi

  if ! command -v sgdisk >/dev/null 2>&1; then
    echo "[!] $img does not have a usable GPT partition 1, and sgdisk is not installed." >&2
    echo "[!] Boot will continue, but in-guest mkfs.kifs will not work until the disk has a GPT partition." >&2
    return 1
  fi

  echo "[*] Rebuilding GPT partition table on $img"
  sgdisk -Z "$img" >/dev/null 2>&1 || true
  sgdisk -og "$img" >/dev/null
  sgdisk -n 1:2048:0 -t 1:8300 -c 1:KiwiOS "$img" >/dev/null

  if ! sgdisk -i 1 "$img" >/dev/null 2>&1; then
    echo "[!] Failed to create GPT partition 1 on $img" >&2
    return 1
  fi

  return 0
}

prepare_disk_image() {
  local img="$1"

  if ! ensure_disk_image "$img"; then
    echo "[!] Failed to create $img. Boot will continue without a usable disk image." >&2
    return 1
  fi

  DISK_ATTACH_OK=1

  if ! ensure_gpt_partition "$img"; then
    echo "[!] $img is not ready for KiFS installs. Booting anyway for recovery." >&2
    return 0
  fi

  if ./tools/mkfs_kifs --check "$img" 1 >/dev/null 2>&1; then
    DISK_INSTALL_OK=1
    return 0
  fi

  echo "[!] $img partition 1 is not a valid KiFS filesystem." >&2
  echo "[!] Boot will continue so you can recover it from the kernel shell with mkfs.kifs." >&2
  return 0
}

install_program() {
  local src="$1"
  local dst="$2"

  if ./tools/kifs_cp "$DISK_IMAGE" 1 "$src" "$dst"; then
    return 0
  fi

  echo "[!] Failed to install $src to $dst; booting anyway." >&2
  return 1
}

install_program_specs() {
  local failures=0
  local spec src dst

  for spec in "$@"; do
    src="${spec%%:*}"
    dst="${spec#*:}"
    if ! install_program "$src" "$dst"; then
      failures=$((failures + 1))
    fi
  done

  INSTALL_PROGRAM_FAILURES="$failures"
  return "$failures"
}

install_optional_program() {
  local src="$1"
  local dst="$2"

  if [ ! -f "$src" ]; then
    return 0
  fi

  install_program "$src" "$dst" || true
}

prepare_disk_image "$DISK_IMAGE"

if [ "$DISK_INSTALL_OK" = "1" ]; then
  if ! install_program_specs \
    "userspace/bin/init:/bin/init" \
    "userspace/bin/shell:/bin/sh" \
    "userspace/bin/hello:/bin/hello" \
    "userspace/bin/badptr:/bin/badptr" \
    "userspace/bin/filetest:/bin/filetest" \
    "userspace/bin/readtest:/bin/readtest" \
    "userspace/bin/writetest:/bin/writetest" \
    "userspace/bin/preempt_a:/bin/preempt_a" \
    "userspace/bin/preempt_b:/bin/preempt_b" \
    "userspace/bin/preempttest:/bin/preempttest" \
    "userspace/bin/argtest:/bin/argtest" \
    "userspace/bin/termtest:/bin/termtest" \
    "userspace/bin/alloctest:/bin/alloctest" \
    "userspace/bin/cwdtest:/bin/cwdtest" \
    "userspace/bin/libctest:/bin/libctest" \
    "userspace/bin/edit:/bin/edit" \
    "userspace/bin/cursestest:/bin/cursestest" \
    "userspace/bin/echo:/bin/echo" \
    "userspace/bin/clear:/bin/clear" \
    "userspace/bin/pwd:/bin/pwd" \
    "userspace/bin/cat:/bin/cat" \
    "userspace/bin/ls:/bin/ls" \
    "userspace/bin/stat:/bin/stat" \
    "userspace/bin/which:/bin/which" \
    "userspace/bin/touch:/bin/touch" \
    "userspace/bin/mkdir:/bin/mkdir" \
    "userspace/bin/rmdir:/bin/rmdir" \
    "userspace/bin/rm:/bin/rm" \
    "userspace/bin/cp:/bin/cp" \
    "userspace/bin/mv:/bin/mv" \
    "userspace/bin/mount:/bin/mount" \
    "userspace/bin/rescan:/bin/rescan" \
    "userspace/bin/date:/bin/date" \
    "userspace/bin/poweroff:/bin/poweroff" \
    "userspace/bin/reboot:/bin/reboot" \
    "userspace/bin/shutdown:/bin/shutdown"; then
    BIN_INSTALL_FAILURES="$INSTALL_PROGRAM_FAILURES"
    echo "[!] $BIN_INSTALL_FAILURES /bin userspace install(s) failed; trying legacy root-level fallback." >&2

    if ! install_program_specs \
      "userspace/bin/init:/init" \
      "userspace/bin/shell:/shell" \
      "userspace/bin/hello:/hello" \
      "userspace/bin/badptr:/badptr" \
      "userspace/bin/filetest:/filetest" \
      "userspace/bin/readtest:/readtest" \
      "userspace/bin/writetest:/writetest" \
      "userspace/bin/preempt_a:/preempt_a" \
      "userspace/bin/preempt_b:/preempt_b" \
      "userspace/bin/preempttest:/preempttest" \
      "userspace/bin/argtest:/argtest" \
      "userspace/bin/termtest:/termtest" \
      "userspace/bin/alloctest:/alloctest" \
      "userspace/bin/cwdtest:/cwdtest" \
      "userspace/bin/libctest:/libctest" \
      "userspace/bin/edit:/edit" \
      "userspace/bin/cursestest:/cursestest" \
      "userspace/bin/echo:/echo" \
      "userspace/bin/clear:/clear" \
      "userspace/bin/pwd:/pwd" \
      "userspace/bin/cat:/cat" \
      "userspace/bin/ls:/ls" \
      "userspace/bin/stat:/stat" \
      "userspace/bin/which:/which" \
      "userspace/bin/touch:/touch" \
      "userspace/bin/mkdir:/mkdir" \
      "userspace/bin/rmdir:/rmdir" \
      "userspace/bin/rm:/rm" \
      "userspace/bin/cp:/cp" \
      "userspace/bin/mv:/mv" \
      "userspace/bin/mount:/mount" \
      "userspace/bin/rescan:/rescan" \
      "userspace/bin/date:/date" \
      "userspace/bin/poweroff:/poweroff" \
      "userspace/bin/reboot:/reboot" \
      "userspace/bin/shutdown:/shutdown"; then
      LEGACY_INSTALL_FAILURES="$INSTALL_PROGRAM_FAILURES"
      echo "[!] $LEGACY_INSTALL_FAILURES legacy userspace install(s) failed; continuing to boot for recovery." >&2
    fi
  fi

  install_optional_program "userspace/bin/nano" "/bin/nano"
else
  echo "[*] Skipping userspace program install into $DISK_IMAGE"
fi

# Download the latest Limine binary release for the 10.x branch, only if it doesn't exist.

if [ ! -d "limine" ]; then
  echo "[*] Cloning Limine bootloader..."
  git clone https://codeberg.org/Limine/Limine.git limine --branch=v10.3.0-binary --depth=1
fi

# Build "limine" utility.
make -C limine

# Create a directory which will be our ISO root.
mkdir -p iso_root

# Copy the relevant files over.
mkdir -p iso_root/boot
cp -v bin/kiwiOS iso_root/boot/
mkdir -p iso_root/boot/limine
cp -v limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin \
      limine/limine-uefi-cd.bin iso_root/boot/limine/

# Create the EFI boot tree and copy Limine's EFI executables over.
mkdir -p iso_root/EFI/BOOT
cp -v limine/BOOTX64.EFI iso_root/EFI/BOOT/
cp -v limine/BOOTIA32.EFI iso_root/EFI/BOOT/

# Create the bootable ISO.
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
        -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
        -efi-boot-part --efi-boot-image --protective-msdos-label \
        iso_root -o kiwiOS.iso

# Install Limine stage 1 and 2 for legacy BIOS boot.
./limine/limine bios-install kiwiOS.iso

bash launch.sh
