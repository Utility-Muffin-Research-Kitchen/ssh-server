#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-build}
PACKAGE_DIR="$ROOT_DIR/$BUILD_DIR/package/SSHServer.pak"

if [ ! -d "$PACKAGE_DIR" ]; then
    echo "Package not found at $PACKAGE_DIR. Run 'make package' first." >&2
    exit 1
fi

if ! command -v adb >/dev/null 2>&1; then
    echo "adb not found" >&2
    exit 1
fi

SERIAL=${ADB_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')}
if [ -z "$SERIAL" ]; then
    echo "No attached adb device found" >&2
    exit 1
fi

ADB="adb -s $SERIAL"
REMOTE_DIR="/mnt/sdcard/Apps/SSHServer.pak"

$ADB shell "mountpoint -q /mnt/sdcard" >/dev/null || {
    echo "/mnt/sdcard is not mounted on the device." >&2
    exit 1
}

$ADB shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
$ADB push "$PACKAGE_DIR/." "$REMOTE_DIR/" >/dev/null
$ADB shell "find '$REMOTE_DIR' -maxdepth 3 -type f | sort"
