#!/usr/bin/env bash
# List routes from the server defined in config/config.json.
# Usage: sra-list.sh [--active]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${SRA_CONFIG:-${REPO_ROOT}/config/config.json}"

exec "${SRA_BIN:-sra}" --config "$CONFIG" list "$@"
