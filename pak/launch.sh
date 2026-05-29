#!/bin/sh
set -eu

APP_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export UMRK_SSH_APP_ROOT="$APP_DIR"

exec "$APP_DIR/bin/ssh-server"
