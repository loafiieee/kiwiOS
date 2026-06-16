#!/bin/bash
set -e

WIN_IMAGE=$(wslpath -w "$(readlink -f kiwiOS.iso)")
WIN_QEMU_LOG=$(wslpath -w "$(readlink -f qemu.log)")
DISK_ARGS=()
USB_ARGS=()
AHCI_ENABLED=0
NEXT_AHCI_PORT=0
USB_ENABLED=0
NEXT_USB_DISK=0

ensure_ahci_controller() {
  if [ "$AHCI_ENABLED" = "0" ]; then
    DISK_ARGS+=(-device ich9-ahci,id=ahci0)
    AHCI_ENABLED=1
  fi
}

attach_disk_image() {
  local img="$1"
  local port="$2"
  local disk_id="d${port}"
  local win_path=""

  if [ ! -f "$img" ]; then
    echo "[!] disk image not found: $img"
    return 1
  fi

  ensure_ahci_controller
  win_path=$(wslpath -w "$(readlink -f "$img")")
  DISK_ARGS+=(
    -drive id="$disk_id",file="$win_path",if=none,format=raw
    -device ide-hd,drive="$disk_id",bus=ahci0."$port"
  )
  return 0
}

ensure_usb_controller() {
  if [ "$USB_ENABLED" = "0" ]; then
    USB_ARGS+=(-device piix3-usb-uhci,id=usb0)
    USB_ENABLED=1
  fi
}

attach_usb_disk_image() {
  local img="$1"
  local disk_id="usbdrv${NEXT_USB_DISK}"
  local win_path=""

  if [ ! -f "$img" ]; then
    echo "[!] USB disk image not found: $img"
    return 1
  fi

  ensure_usb_controller
  win_path=$(wslpath -w "$(readlink -f "$img")")
  USB_ARGS+=(
    -drive id="$disk_id",file="$win_path",if=none,format=raw
    -device usb-storage,drive="$disk_id",bus=usb0.0
  )
  NEXT_USB_DISK=$((NEXT_USB_DISK + 1))
  return 0
}

if [ -f "disk.img" ]; then
  attach_disk_image "disk.img" "$NEXT_AHCI_PORT"
  NEXT_AHCI_PORT=$((NEXT_AHCI_PORT + 1))
else
  echo "[*] disk.img not found; booting without an attached hard disk"
fi

if [ -n "${QEMU_EXTRA_DISKS:-}" ]; then
  for img in ${QEMU_EXTRA_DISKS}; do
    if [ "$NEXT_AHCI_PORT" -ge 6 ]; then
      echo "[!] ignoring extra disk $img: AHCI port limit reached"
      continue
    fi
    if attach_disk_image "$img" "$NEXT_AHCI_PORT"; then
      echo "[*] attached extra disk $img on AHCI port $NEXT_AHCI_PORT"
      NEXT_AHCI_PORT=$((NEXT_AHCI_PORT + 1))
    fi
  done
fi

if [ "${QEMU_USB:-0}" = "1" ] || [ -n "${QEMU_USB_DISKS:-}" ]; then
  ensure_usb_controller
fi

if [ -n "${QEMU_USB_DISKS:-}" ]; then
  for img in ${QEMU_USB_DISKS}; do
    if attach_usb_disk_image "$img"; then
      echo "[*] attached USB storage disk $img"
    fi
  done
fi

QEMU_ARGS=()
if [ "${QEMU_DEBUG:-0}" = "1" ]; then
  echo "[*] QEMU debug mode enabled"
  echo "[*] Writing interrupt/guest-error log to qemu.log"
  QEMU_ARGS+=(
    -no-reboot
    -no-shutdown
    -d int,guest_errors
    -D "$WIN_QEMU_LOG"
  )
fi

if [ "${QEMU_MONITOR:-0}" = "1" ]; then
  echo "[*] QEMU monitor enabled on tcp:127.0.0.1:4444"
  QEMU_ARGS+=(-monitor tcp:127.0.0.1:4444,server,nowait)
fi

cd "/mnt/c/Program Files/qemu/"

./qemu-system-x86_64.exe \
  -M q35 \
  -serial stdio \
  "${DISK_ARGS[@]}" \
  "${USB_ARGS[@]}" \
  -m 2048M \
  -cdrom "$WIN_IMAGE" \
  -boot order=d \
  "${QEMU_ARGS[@]}"
