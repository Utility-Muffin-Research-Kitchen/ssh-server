#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-build/mlp1}
DROPBEAR_VERSION=${DROPBEAR_VERSION:-2025.88}
DROPBEAR_TAG=${DROPBEAR_TAG:-DROPBEAR_${DROPBEAR_VERSION}}
TOOLCHAIN_IMAGE=${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local}

SOURCE_ARCHIVE="$ROOT_DIR/$BUILD_DIR/third_party/sources/dropbear-${DROPBEAR_VERSION}.tar.gz"
SOURCE_DIR="$ROOT_DIR/$BUILD_DIR/third_party/dropbear-${DROPBEAR_VERSION}"
RUNTIME_BIN_DIR="$ROOT_DIR/$BUILD_DIR/runtime/bin"
BUILD_SCRIPT="$ROOT_DIR/$BUILD_DIR/third_party/dropbear-build.sh"

mkdir -p "$(dirname "$SOURCE_ARCHIVE")" "$RUNTIME_BIN_DIR"

if [ ! -f "$SOURCE_ARCHIVE" ]; then
    curl -L --fail --silent --show-error \
        "https://github.com/mkj/dropbear/archive/refs/tags/${DROPBEAR_TAG}.tar.gz" \
        -o "$SOURCE_ARCHIVE"
fi

rm -rf "$SOURCE_DIR"
mkdir -p "$SOURCE_DIR"
tar -xzf "$SOURCE_ARCHIVE" -C "$SOURCE_DIR" --strip-components=1

cat > "$BUILD_SCRIPT" <<'SH'
set -eux
cd "$DROPBEAR_SOURCE_DIR"
export PATH=/opt/mlp1-toolchain/bin:$PATH
export CC=aarch64-buildroot-linux-gnu-gcc
export AR=aarch64-buildroot-linux-gnu-ar
export RANLIB=aarch64-buildroot-linux-gnu-ranlib
export STRIP=aarch64-buildroot-linux-gnu-strip
export CFLAGS='-Os'
export LDFLAGS=''
make distclean || true
./configure --host=aarch64-buildroot-linux-gnu \
    --disable-zlib \
    --disable-syslog \
    --disable-lastlog \
    --disable-utmp \
    --disable-utmpx \
    --disable-wtmp \
    --disable-wtmpx
make PROGRAMS='dropbear dropbearkey' -j2
"$STRIP" dropbear dropbearkey
install -m 755 dropbear "$DROPBEAR_OUTPUT_DIR/dropbear"
install -m 755 dropbearkey "$DROPBEAR_OUTPUT_DIR/dropbearkey"
SH

chmod 755 "$BUILD_SCRIPT"

docker run --rm \
    -e DROPBEAR_SOURCE_DIR="/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-${DROPBEAR_VERSION}" \
    -e DROPBEAR_OUTPUT_DIR="/workspace/ssh-server/${BUILD_DIR}/runtime/bin" \
    -v "$(dirname "$ROOT_DIR")":/workspace \
    -w /workspace/ssh-server \
    "$TOOLCHAIN_IMAGE" \
    sh "/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-build.sh"
