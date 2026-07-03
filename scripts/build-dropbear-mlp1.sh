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
BUILD_LOG="$ROOT_DIR/$BUILD_DIR/third_party/dropbear-build.log"
STAMP="$RUNTIME_BIN_DIR/.dropbear-${DROPBEAR_VERSION}.stamp"
SCRIPT_CKSUM=$(cksum "$0" | awk '{print $1 ":" $2}')
BUILD_ID="version=${DROPBEAR_VERSION} tag=${DROPBEAR_TAG} image=${TOOLCHAIN_IMAGE} script=${SCRIPT_CKSUM}"

mkdir -p "$(dirname "$SOURCE_ARCHIVE")" "$RUNTIME_BIN_DIR"

# Skip only when the binaries exist AND a stamp proves they were built from this
# exact version/tag/image/script. A missing or mismatched stamp means we can't
# trust the on-disk binaries (e.g. left over from a different version), so we
# rebuild rather than adopt them.
if [ "${DROPBEAR_FORCE_REBUILD:-0}" != "1" ] &&
    [ -x "$RUNTIME_BIN_DIR/dropbear" ] &&
    [ -x "$RUNTIME_BIN_DIR/dropbearkey" ] &&
    [ -f "$STAMP" ] && grep -qxF "$BUILD_ID" "$STAMP"; then
    echo "Dropbear ${DROPBEAR_VERSION} already built at $RUNTIME_BIN_DIR"
    exit 0
fi

if [ ! -f "$SOURCE_ARCHIVE" ]; then
    echo "Fetching Dropbear ${DROPBEAR_VERSION} source"
    curl -L --fail --silent --show-error \
        "https://github.com/mkj/dropbear/archive/refs/tags/${DROPBEAR_TAG}.tar.gz" \
        -o "$SOURCE_ARCHIVE"
fi

echo "Building Dropbear ${DROPBEAR_VERSION} for MLP1 (log: $BUILD_LOG)"
rm -rf "$SOURCE_DIR"
mkdir -p "$SOURCE_DIR"
tar -xzf "$SOURCE_ARCHIVE" -C "$SOURCE_DIR" --strip-components=1

cat > "$BUILD_SCRIPT" <<'SH'
set -eu
if [ "${DROPBEAR_VERBOSE:-0}" = "1" ]; then
    set -x
fi
cd "$DROPBEAR_SOURCE_DIR"
export PATH=/opt/mlp1-toolchain/bin:$PATH
export CC=aarch64-buildroot-linux-gnu-gcc
export AR=aarch64-buildroot-linux-gnu-ar
export RANLIB=aarch64-buildroot-linux-gnu-ranlib
export STRIP=aarch64-buildroot-linux-gnu-strip
if [ -f /opt/mlp1-toolchain/umrk/mlp1-build-flags.env ]; then
    MLP1_BUILD_PROFILE="${DROPBEAR_MLP1_BUILD_PROFILE:-size}"
    export MLP1_BUILD_PROFILE
    . /opt/mlp1-toolchain/umrk/mlp1-build-flags.env
else
    UMRK_MLP1_PROFILE_CFLAGS='-Os -mcpu=cortex-a55 -mtune=cortex-a55 -ffunction-sections -fdata-sections -DNDEBUG'
fi
export CFLAGS="${DROPBEAR_CFLAGS:-$UMRK_MLP1_PROFILE_CFLAGS}"
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

if [ "${DROPBEAR_VERBOSE:-0}" = "1" ]; then
    docker run --rm \
        -e DROPBEAR_SOURCE_DIR="/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-${DROPBEAR_VERSION}" \
        -e DROPBEAR_OUTPUT_DIR="/workspace/ssh-server/${BUILD_DIR}/runtime/bin" \
        -e DROPBEAR_VERBOSE=1 \
        -v "$(dirname "$ROOT_DIR")":/workspace \
        -w /workspace/ssh-server \
        "$TOOLCHAIN_IMAGE" \
        sh "/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-build.sh" | tee "$BUILD_LOG"
else
    docker run --rm \
        -e DROPBEAR_SOURCE_DIR="/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-${DROPBEAR_VERSION}" \
        -e DROPBEAR_OUTPUT_DIR="/workspace/ssh-server/${BUILD_DIR}/runtime/bin" \
        -v "$(dirname "$ROOT_DIR")":/workspace \
        -w /workspace/ssh-server \
        "$TOOLCHAIN_IMAGE" \
        sh "/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-build.sh" > "$BUILD_LOG" 2>&1 || {
            echo "Dropbear build failed. Last log lines from $BUILD_LOG:" >&2
            tail -n 80 "$BUILD_LOG" >&2
            exit 1
        }
fi

printf '%s\n' "$BUILD_ID" > "$STAMP"
echo "Built Dropbear ${DROPBEAR_VERSION} at $RUNTIME_BIN_DIR"
