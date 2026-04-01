#!/usr/bin/env bash
# Run the sra test command against the server defined in config/config.json.
# Usage: sra-test.sh [extra sra options...]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
CONFIG="${SRA_CONFIG:-${REPO_ROOT}/config/config.json}"

exec "${SRA_BIN:-sra}" --config "$CONFIG" "$@" test
