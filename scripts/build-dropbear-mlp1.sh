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
PATCH_DIR="$ROOT_DIR/patches/dropbear"
SCRIPT_CKSUM=$(cksum "$0" | awk '{print $1 ":" $2}')
PATCH_CKSUM=$(
    for patch_file in "$PATCH_DIR"/*.patch; do
        cksum <"$patch_file"
    done | cksum | awk '{print $1 ":" $2}'
)
BUILD_ID="version=${DROPBEAR_VERSION} tag=${DROPBEAR_TAG} image=${TOOLCHAIN_IMAGE} script=${SCRIPT_CKSUM} patches=${PATCH_CKSUM}"

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
for patch_file in "$PATCH_DIR"/*.patch; do
    echo "Applying $(basename "$patch_file")"
    patch --batch --forward --fuzz=0 -d "$SOURCE_DIR" -p1 < "$patch_file"
done

# These assertions bind each security-sensitive hunk to the intended function.
# A pinned tarball and content hash prevent silent source drift; these checks
# additionally make a misplaced-but-applicable patch fail before compilation.
test "$(grep -c '^int umrk_service_managed(void);$' "$SOURCE_DIR/src/dbutil.h")" -eq 1
test "$(grep -c '^void umrk_service_arm_child(pid_t expected_parent);$' "$SOURCE_DIR/src/dbutil.h")" -eq 1
test "$(grep -c '^void umrk_service_arm_guardian(pid_t expected_parent);$' "$SOURCE_DIR/src/dbutil.h")" -eq 1
test "$(grep -c '^void umrk_service_rearm_child(void);$' "$SOURCE_DIR/src/dbutil.h")" -eq 1
test "$(grep -c '^void umrk_service_cleanup_descendants(void);$' "$SOURCE_DIR/src/dbutil.h")" -eq 1
test "$(grep -c 'umrk_service_arm_child(expected_parent);' "$SOURCE_DIR/src/dbutil.c")" -eq 1
test "$(grep -c 'umrk_service_arm_guardian(expected_parent);' "$SOURCE_DIR/src/svr-main.c")" -eq 1
test "$(grep -c 'umrk_service_arm_child(expected_parent);' "$SOURCE_DIR/src/svr-chansession.c")" -eq 1
test "$(grep -c 'umrk_service_rearm_child();' "$SOURCE_DIR/src/svr-chansession.c")" -eq 1
test "$(grep -c 'umrk_service_cleanup_descendants();' "$SOURCE_DIR/src/svr-session.c")" -eq 1
grep -Fq 'PR_SET_CHILD_SUBREAPER' "$SOURCE_DIR/src/dbutil.c"
grep -Fq 'if (!umrk_service_managed() && setsid() < 0)' "$SOURCE_DIR/src/svr-main.c"
awk '
    /void umrk_service_arm_child\(pid_t expected_parent\)/ { in_child = 1 }
    in_child && /umrk_connection_guardian = 0;/ { reset_guardian = 1 }
    in_child && /^}/ { exit !reset_guardian }
    END { if (!in_child) exit 1 }
' "$SOURCE_DIR/src/dbutil.c"
if grep -Fq 'addnewvar("UMRK_SERVICE_LEASE_FD"' "$SOURCE_DIR/src/svr-chansession.c"; then
    echo "patched Dropbear must not export the generation lease to login shells" >&2
    exit 1
fi
awk '
    /void run_command\(const char\* argv0/ { in_run = 1 }
    in_run && /for \(i = 3; i <= maxfd; i\+\+\)/ { saw_loop = 1 }
    in_run && saw_loop && /m_close\(i\)/ { saw_close = 1 }
    in_run && /execv\(argv0, args\)/ { exit !(saw_loop && saw_close) }
    END { if (!in_run) exit 1 }
' "$SOURCE_DIR/src/dbutil.c"

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
        -v "$ROOT_DIR":/workspace/ssh-server \
        -w /workspace/ssh-server \
        "$TOOLCHAIN_IMAGE" \
        sh "/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-build.sh" | tee "$BUILD_LOG"
else
    docker run --rm \
        -e DROPBEAR_SOURCE_DIR="/workspace/ssh-server/${BUILD_DIR}/third_party/dropbear-${DROPBEAR_VERSION}" \
        -e DROPBEAR_OUTPUT_DIR="/workspace/ssh-server/${BUILD_DIR}/runtime/bin" \
        -v "$ROOT_DIR":/workspace/ssh-server \
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
