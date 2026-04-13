#!/usr/bin/env bash
# Run srmd (Switch Route Manager Daemon) from a local install/ directory.
# Usage: ./scripts/run-server.sh [extra srmd options]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname ".")" #$SCRIPT_DIR
INSTALL_DIR="$REPO_ROOT/server-local"

SRMD="$INSTALL_DIR/sbin/srmd"
CONFIG="$INSTALL_DIR/etc/srmd/srmd.json"
CONFIG_SOT="$INSTALL_DIR/etc/srmd/route_sot_v2.json"

if [[ ! -x "$SRMD" ]]; then
    echo "error: srmd not found at $SRMD" >&2
    echo "       Run: cmake --install <build-dir> --prefix install" >&2
    exit 1
fi

if [[ ! -f "$CONFIG" ]]; then
    echo "error: config not found at $CONFIG" >&2
    exit 1
fi

exec "$SRMD" --foreground --config "$CONFIG" --sot "$CONFIG_SOT" "$@"
