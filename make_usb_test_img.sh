#!/bin/bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash make_usb_test_img.sh [--kifs|--fat] [--force] [--size SIZE] [IMAGE]

Defaults:
  filesystem: kifs
  image:      usb-kifs.img or usb-fat.img
  size:       64M

Examples:
  bash make_usb_test_img.sh --kifs
  bash make_usb_test_img.sh --fat usb-fat.img
  bash make_usb_test_img.sh --force --size 128M usb-kifs.img
EOF
}

fs="kifs"
size="64M"
force=0
image=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --kifs)
      fs="kifs"
      shift
      ;;
    --fat)
      fs="fat"
      shift
      ;;
    --force)
      force=1
      shift
      ;;
    --size)
      if [ "$#" -lt 2 ]; then
        usage >&2
        exit 2
      fi
      size="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      if [ -n "$image" ]; then
        echo "only one image path is supported" >&2
        usage >&2
        exit 2
      fi
      image="$1"
      shift
      ;;
  esac
done

if [ -z "$image" ]; then
  image="usb-${fs}.img"
fi

if [ -e "$image" ] && [ "$force" != "1" ]; then
  echo "$image already exists; pass --force to replace it." >&2
  exit 1
fi

need_tool() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing required host tool: $1" >&2
    exit 1
  fi
}

need_tool truncate
need_tool sgdisk
need_tool dd

if [ "$fs" = "kifs" ]; then
  make -C tools all
else
  need_tool mkfs.fat
fi

if [ -e "$image" ]; then
  rm -f "$image"
fi

truncate -s "$size" "$image"
sgdisk -og "$image" >/dev/null
sgdisk -n 1:2048:0 -t 1:8300 -c 1:KIWIUSB "$image" >/dev/null
part_start="$(sgdisk -i 1 "$image" | awk '/First sector/ { print $3 }')"
part_end="$(sgdisk -i 1 "$image" | awk '/Last sector/ { print $3 }')"
if [ -z "$part_start" ] || [ -z "$part_end" ] || [ "$part_end" -lt "$part_start" ]; then
  echo "failed to determine GPT partition bounds for $image" >&2
  exit 1
fi
part_sectors=$((part_end - part_start + 1))

case "$fs" in
  kifs)
    tools/mkfs_kifs "$image" 1

    tmpfile="$(mktemp)"
    trap 'rm -f "$tmpfile"' EXIT
    cat > "$tmpfile" <<'EOF'
Hello from an emulated KiwiOS USB disk.
If you can cat this file after hotplug + mount, USB storage and VFS routing worked.
EOF
    tools/kifs_cp "$image" 1 "$tmpfile" /usb-test.txt
    ;;

  fat)
    part_image="$(mktemp)"
    trap 'rm -f "$part_image"' EXIT

    truncate -s "$((part_sectors * 512))" "$part_image"
    mkfs.fat -F 16 -n KIWIUSB "$part_image" >/dev/null

    if command -v mcopy >/dev/null 2>&1; then
      tmpfile="$(mktemp)"
      trap 'rm -f "$part_image" "$tmpfile"' EXIT
      cat > "$tmpfile" <<'EOF'
Hello from an emulated FAT USB disk.
If you can cat this file after hotplug + mount, FAT reads over USB worked.
EOF
      mcopy -i "$part_image" "$tmpfile" ::/USBTEST.TXT
    else
      echo "[*] mcopy not found; FAT image is formatted but no test file was copied."
    fi

    dd if="$part_image" of="$image" bs=512 seek="$part_start" conv=notrunc status=none
    ;;
esac

echo "[ OK ] Created $fs USB test image: $image"
if command -v wslpath >/dev/null 2>&1; then
  win_path="$(wslpath -w "$(readlink -f "$image")" | sed 's#\\#/#g')"
  echo "[INFO] QEMU monitor file path: $win_path"
fi
