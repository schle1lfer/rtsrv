#!/usr/bin/env bash
# hooks/install-hooks.sh
#
# Installs the project's git hooks into .git/hooks/.
# Run once after cloning:
#
#   ./hooks/install-hooks.sh
#
# Or via the CMake helper:
#
#   cmake --build build --target install-hooks

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
GIT_HOOKS_DIR="${REPO_ROOT}/.git/hooks"

install_hook() {
    local src="${SCRIPT_DIR}/${1}"
    local dst="${GIT_HOOKS_DIR}/${1}"

    if [[ ! -f "${src}" ]]; then
        echo "WARNING: hook source not found: ${src}" >&2
        return
    fi

    cp "${src}" "${dst}"
    chmod +x "${dst}"
    echo "Installed: ${dst}"
}

install_hook "pre-commit"

echo "Git hooks installed successfully."
