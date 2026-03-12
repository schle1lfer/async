#!/usr/bin/env bash
#
# Install project git hooks into .git/hooks/
#
# Usage: ./scripts/install-hooks.sh

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
HOOKS_SRC="${REPO_ROOT}/hooks"
HOOKS_DST="${REPO_ROOT}/.git/hooks"

install_hook() {
    local name="$1"
    local src="${HOOKS_SRC}/${name}"
    local dst="${HOOKS_DST}/${name}"

    if [[ ! -f "$src" ]]; then
        echo "WARNING: hook source not found: $src"
        return
    fi

    cp "$src" "$dst"
    chmod +x "$dst"
    echo "Installed: $dst"
}

install_hook "pre-commit"

echo "Done. Git hooks installed successfully."
