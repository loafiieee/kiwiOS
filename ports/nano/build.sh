#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PORT_DIR="$ROOT/ports/nano"
VERSION="9.1"
TARBALL="nano-${VERSION}.tar.xz"
URL="https://www.nano-editor.org/dist/v9/${TARBALL}"
CACHE_DIR="$PORT_DIR/cache"
SRC_DIR="$PORT_DIR/src/nano-${VERSION}"
BUILD_DIR="$PORT_DIR/build"
NANO_ELF="$SRC_DIR/src/nano"
NANO_KXE="$BUILD_DIR/nano"

mkdir -p "$CACHE_DIR" "$PORT_DIR/src" "$BUILD_DIR"

echo "[*] Building Kiwi userspace support archive"
make -C "$ROOT/userspace" bin/libkiwi.a
ln -sf "$ROOT/userspace/bin/libkiwi.a" "$BUILD_DIR/libkiwi.a"
ln -sf "$ROOT/userspace/bin/libkiwi.a" "$BUILD_DIR/libncurses.a"
ln -sf "$ROOT/userspace/bin/libkiwi.a" "$BUILD_DIR/libncursesw.a"
ln -sf "$ROOT/userspace/bin/libkiwi.a" "$BUILD_DIR/libcurses.a"
ln -sf "$ROOT/userspace/bin/libkiwi.a" "$BUILD_DIR/libtinfo.a"

if [ ! -f "$CACHE_DIR/$TARBALL" ]; then
  echo "[*] Fetching $URL"
  curl -L "$URL" -o "$CACHE_DIR/$TARBALL"
fi

if [ ! -d "$SRC_DIR" ]; then
  echo "[*] Unpacking $TARBALL"
  tar -C "$PORT_DIR/src" -xf "$CACHE_DIR/$TARBALL"
fi

cd "$SRC_DIR"
rm -f config.cache

KIWI_CC=${KIWI_CC:-cc}
KIWI_LD=${KIWI_LD:-ld}
KIWI_HOST=${KIWI_HOST:-x86_64-pc-elf}
KIWI_CFLAGS=${KIWI_CFLAGS:-"-g -O2 -Wall -Wextra -std=gnu11 -ffreestanding -fno-stack-protector -fno-stack-check -fno-pic -fno-pie -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables -m64 -march=x86-64 -mabi=sysv -mno-red-zone"}
KIWI_CPPFLAGS=${KIWI_CPPFLAGS:-"-I$ROOT/userspace/kiwilib/include -I$ROOT/src"}
KIWI_LDFLAGS=${KIWI_LDFLAGS:-"-L$BUILD_DIR -nostdlib -static -T $ROOT/userspace/user.lds"}
KIWI_LIBS=${KIWI_LIBS:-"-lkiwi"}

echo "[*] Configuring nano ${VERSION} for a KiwiOS compatibility probe"
./configure \
  --host="$KIWI_HOST" \
  --build="$(./config.guess)" \
  --prefix=/ \
  --disable-nls \
  --disable-speller \
  --disable-mouse \
  --disable-browser \
  --disable-help \
  CC="$KIWI_CC" \
  LD="$KIWI_LD" \
  CFLAGS="$KIWI_CFLAGS" \
  CPPFLAGS="$KIWI_CPPFLAGS" \
  LDFLAGS="$KIWI_LDFLAGS" \
  LIBS="$KIWI_LIBS" \
  "$@"

echo "[*] Building nano compatibility probe"
make V=1

if [ ! -f "$NANO_ELF" ]; then
  echo "[!] Expected nano ELF was not produced at $NANO_ELF" >&2
  exit 1
fi

echo "[*] Converting nano ELF to KXE"
"$ROOT/tools/elf2kxe" "$NANO_ELF" "$NANO_KXE"
cp -f "$NANO_KXE" "$ROOT/userspace/bin/nano"

echo "[ OK ] Built nano KXE: $NANO_KXE"
echo "[ OK ] Copied nano KXE to userspace/bin/nano for /bin installation"
