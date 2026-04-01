#!/usr/bin/env bash
# Send an Echo RPC to the server defined in config/config.json.
# Usage: sra-echo.sh [message]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${SRA_CONFIG:-${REPO_ROOT}/config/config.json}"
MESSAGE="${1:-ping from sra}"

exec "${SRA_BIN:-sra}" --config "$CONFIG" echo "$MESSAGE"
