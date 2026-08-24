#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The mutation gate: ZERO UNEXPLAINED SURVIVING MUTANTS (docs/PLAN.md §7.3).
#
# Not a percentage. A mutation-score threshold licenses a standing population of
# unexamined survivors, which is the same defect the 100% coverage rule avoids.
# Every survivor is either killed by a better test, or listed in
# tools/mutation-allowlist.txt with a written justification.
#
# Two things this script must get right, both learned the hard way:
#
#   1. Mull exits NON-ZERO whenever any mutant survives (since 0.20). Under
#      'set -e' with a pipeline, that killed this script before it ever reached
#      the allowlist logic -- so the reviewed-equivalent mechanism was dead code
#      that happened to look correct because the allowlist was empty. Mull is
#      therefore run with --allow-surviving, and THIS script decides the verdict.
#      A genuine Mull failure (crash, bad arguments) is still fatal.
#
#   2. A survivor key of path:line:column is too weak: different mutation
#      operators can fire at the same location, so one allowlist entry could
#      silently excuse a second, unrelated mutant. The key includes the mutator.
set -euo pipefail

TEST_BINARY="${1:?usage: mutation-gate.sh <test-binary> [report-file]}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ALLOWLIST="$ROOT/tools/mutation-allowlist.txt"
REPORT="${2:-}"

if [ -z "$REPORT" ]; then
  command -v mull-runner-18 >/dev/null || {
    echo "mutation gate: mull-runner-18 not found -- refusing to report success" >&2
    exit 1
  }
  REPORT="$(mktemp)"
  # --allow-surviving: survivors are this script's business, not an exit code.
  if ! mull-runner-18 "$TEST_BINARY" --allow-surviving --reporters IDEReporter >"$REPORT" 2>&1; then
    echo "mutation gate: mull-runner-18 failed to run" >&2
    cat "$REPORT" >&2
    exit 1
  fi
  cat "$REPORT"
fi

# Survivor lines look like:  path:line:col: warning: Survived: <Mutator> ...
# The key is path:line:col:mutator, so two operators at one site stay distinct.
survivors="$(sed -n 's/^\(.*:[0-9]\+:[0-9]\+\): warning: Survived: \([A-Za-z_][A-Za-z0-9_]*\).*/\1:\2/p' \
             "$REPORT" | sort -u || true)"

# Allowlist entries: one key per line, '#' comments ignored.
mapfile -t allowed < <(grep -vE '^\s*(#|$)' "$ALLOWLIST" 2>/dev/null || true)

unexplained=0
used=()
while IFS= read -r key; do
  [ -z "$key" ] && continue
  if printf '%s\n' "${allowed[@]:-}" | grep -qxF "$key"; then
    echo "  allowed (reviewed): $key"
    used+=("$key")
  else
    echo "  UNEXPLAINED SURVIVOR: $key" >&2
    unexplained=$((unexplained + 1))
  fi
done <<< "$survivors"

# Stale entries are a defect too: an allowlist that outlives its mutant quietly
# grants permission for something that no longer exists.
stale=0
for key in "${allowed[@]:-}"; do
  [ -z "$key" ] && continue
  printf '%s\n' "${used[@]:-}" | grep -qxF "$key" || {
    echo "  STALE ALLOWLIST ENTRY (mutant no longer survives): $key" >&2
    stale=$((stale + 1))
  }
done

echo "mutation gate: ${unexplained} unexplained, ${#allowed[@]} allowed, ${stale} stale"

if [ "$unexplained" -ne 0 ] || [ "$stale" -ne 0 ]; then
  echo "mutation gate: FAILED. Kill each survivor with a better test, or add its key" >&2
  echo "  to tools/mutation-allowlist.txt with a written justification. Remove stale" >&2
  echo "  entries -- an allowlist that outlives its mutant is a permission nobody reviewed." >&2
  exit 1
fi
