#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs the repository's git hooks (tools/git-hooks) into the common hooks
# directory, which linked worktrees share. Idempotent. A hook file that does not
# carry the marker below is someone's own, possibly stricter, and is left alone.
# Changes no git configuration: a contributor's identity is theirs.
set -uo pipefail

MARKER="# LOADFORGE-REPO-HOOK"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/tools/git-hooks"

common="$(git -C "$ROOT" rev-parse --path-format=absolute --git-common-dir)" || {
  echo "install-git-hooks: $ROOT is not in a git repository." >&2; exit 1; }
dest="$common/hooks"
mkdir -p "$dest" || { echo "install-git-hooks: cannot create $dest." >&2; exit 1; }

for hook in pre-commit commit-msg pre-push; do
  [ -r "$SRC/$hook" ] || { echo "install-git-hooks: missing $SRC/$hook." >&2; exit 1; }
  if [ -e "$dest/$hook" ] && ! grep -qxF "$MARKER" "$dest/$hook"; then
    echo "install-git-hooks: $dest/$hook exists and is not ours; left untouched."
    continue
  fi
  tmp="$dest/$hook.tmp.$$"
  if ! { cp "$SRC/$hook" "$tmp" && chmod +x "$tmp" && mv -f "$tmp" "$dest/$hook"; }; then
    rm -f "$tmp"
    echo "install-git-hooks: could not install $hook." >&2; exit 1
  fi
done

if hp="$(git -C "$ROOT" config --get core.hooksPath)" && [ -n "$hp" ]; then
  echo "install-git-hooks: core.hooksPath is set ($hp); git will not read $dest."
fi
