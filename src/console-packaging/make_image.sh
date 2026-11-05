#!/bin/sh
# Stage ordinary directories only. Never partition, format, mount, or flash a card.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$SCRIPT_DIR/package.py" stage "$@"
