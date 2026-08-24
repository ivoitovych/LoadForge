#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The mutation gate: ZERO UNEXPLAINED SURVIVING MUTANTS (docs/PLAN.md §7.3).
#
# Not a percentage. A mutation-score threshold licenses a standing population of
# unexamined survivors, which is the same defect the 100% coverage rule exists to
# avoid. Every survivor is either killed by a better test, or listed in
# tools/mutation-allowlist.txt with a written justification.
#
# Fails if Mull cannot run. A gate that degrades to a no-op when its tool is
# missing is not a gate -- it is a green tick with nothing behind it.
set -euo pipefail

TEST_BINARY="${1:?usage: mutation-gate.sh <test-binary>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ALLOWLIST="$ROOT/tools/mutation-allowlist.txt"
REPORT=/tmp/mutation-report.txt

command -v mull-runner-18 >/dev/null || {
  echo "mutation gate: mull-runner-18 not found -- refusing to report success" >&2
  exit 1
}

mull-runner-18 "$TEST_BINARY" --reporters IDEReporter 2>&1 | tee "$REPORT"

# Survivors are the lines Mull reports as surviving mutants.
survivors=$(grep -E '^\S+:[0-9]+:[0-9]+: warning: Survived' "$REPORT" | sed 's/: warning:.*//' | sort -u || true)

unexplained=0
while IFS= read -r s; do
  [ -z "$s" ] && continue
  if grep -qxF "$s" "$ALLOWLIST" 2>/dev/null; then
    echo "  allowed (reviewed): $s"
  else
    echo "  UNEXPLAINED SURVIVOR: $s" >&2
    unexplained=$((unexplained + 1))
  fi
done <<< "$survivors"

allowed=$(grep -cvE '^\s*(#|$)' "$ALLOWLIST" 2>/dev/null || echo 0)
echo "mutation gate: $unexplained unexplained survivor(s), $allowed reviewed-equivalent entries"

if [ "$unexplained" -ne 0 ]; then
  echo "mutation gate: FAILED. Kill each survivor with a better test, or add it to" >&2
  echo "  tools/mutation-allowlist.txt with a written justification." >&2
  exit 1
fi
