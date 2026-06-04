#!/bin/sh
set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PAK_SDCARD_ROOT=$(CDPATH= cd -- "$APP_DIR/../.." && pwd)
MLP1_DEFAULT_SDCARD_PATH=/mnt/sdcard

if [ -n "${UMRK_ENV_FILE:-}" ] && [ -f "$UMRK_ENV_FILE" ]; then
    . "$UMRK_ENV_FILE"
elif [ -n "${SDCARD_PATH:-}" ] && [ -f "$SDCARD_PATH/.system/leaf/launcher/env.sh" ]; then
    . "$SDCARD_PATH/.system/leaf/launcher/env.sh"
elif [ -f "$PAK_SDCARD_ROOT/.system/leaf/launcher/env.sh" ]; then
    . "$PAK_SDCARD_ROOT/.system/leaf/launcher/env.sh"
fi

if [ -z "${PLATFORM:-}" ]; then
    case "$PAK_SDCARD_ROOT" in
        "$MLP1_DEFAULT_SDCARD_PATH") PLATFORM=mlp1 ;;
        *) PLATFORM=mac ;;
    esac
fi
export PLATFORM
export SDCARD_PATH="${SDCARD_PATH:-${JAWAKA_SDCARD_ROOT:-$PAK_SDCARD_ROOT}}"
export UMRK_SSH_APP_ROOT="$APP_DIR"

exec "$APP_DIR/bin/ssh-server"
