#!/usr/bin/env bash
# Run sra (Switch Route Application) from a local install/ directory.
# Usage: ./scripts/run-client.sh [sra options] <command> [args...]
# Example: ./scripts/run-client.sh test
#          ./scripts/run-client.sh --server localhost:50051 list

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
INSTALL_DIR="$REPO_ROOT/install"

SRA="$INSTALL_DIR/bin/sra"

if [[ ! -x "$SRA" ]]; then
    echo "error: sra not found at $SRA" >&2
    echo "       Run: cmake --install <build-dir> --prefix install" >&2
    exit 1
fi

exec "$SRA" "$@"
