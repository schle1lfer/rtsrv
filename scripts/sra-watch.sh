#!/usr/bin/env bash
# Watch kernel IPv4 /32 route events and forward them to the srmd server
# defined in config/config.json.  Runs until Ctrl-C.
# Usage: sra-watch.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${SRA_CONFIG:-${REPO_ROOT}/config/config.json}"

exec "${SRA_BIN:-sra}" --config "$CONFIG" watch
