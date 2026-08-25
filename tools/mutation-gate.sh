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
# Three things this script must get right, all learned the hard way:
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
#
#   3. THE GATE MUST FAIL CLOSED ON A REPORT IT CANNOT VOUCH FOR. "I found no
#      survivor lines" and "I did not understand this report" produce the same
#      empty survivor list, and the earlier version reported success for both.
#      A truncated run, a crashed runner, an empty file, or a future Mull whose
#      output format changed would all have been indistinguishable from a clean
#      sweep -- the gate would have certified a test suite it never measured.
#      So the report must now carry a positive completion marker, every
#      "Survived:" line must parse into a key, and the survivor count must
#      reconcile with the summary's own arithmetic. Any disagreement between
#      the script's reading of the evidence and the evidence's own totals is
#      treated as a failure of the gate, not as a pass.
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

[ -f "$REPORT" ] || {
  echo "mutation gate: report '$REPORT' does not exist -- refusing to report success" >&2
  exit 1
}

# ---------------------------------------------------------------------------
# Step 1: does this file even claim to be a completed mutation run?
#
# Mull's summary line is the completion marker. Without it the run either never
# finished or the output is not a Mull report, and either way there is nothing
# here to certify. Accepting more than one spelling is deliberate: the point is
# to recognise a *finished run*, not to bind the gate to one Mull release. If a
# future Mull emits none of these, this gate fails -- which is the correct
# direction to fail, and a one-line fix once someone has looked at the output.
# ---------------------------------------------------------------------------
summary_re='(Killed|Survived) mutants[:( ]+[0-9]+/[0-9]+|Mutation score: *[0-9]+'
if ! grep -qE "$summary_re" "$REPORT"; then
  echo "mutation gate: no Mull summary line in '$REPORT'." >&2
  echo "  The report carries no evidence that a mutation run completed, so there is" >&2
  echo "  nothing to certify. A truncated, empty, or unrecognised report is a FAILURE," >&2
  echo "  never a clean sweep. Check that mull-runner ran to completion." >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Step 2: parse survivors, and insist that the parse is total.
#
# Survivor lines look like:  path:line:col: warning: Survived: <Mutator> ...
# The key is path:line:col:mutator, so two operators at one site stay distinct.
# Counting the raw "Survived:" lines separately is the check that matters: if
# even one of them fails to yield a key, this script has misread the report and
# must not pronounce on it.
# ---------------------------------------------------------------------------
survivor_lines="$(grep -cE 'warning: Survived:' "$REPORT" || true)"
keys="$(sed -n 's/^\(.*:[0-9]\+:[0-9]\+\): warning: Survived: \([A-Za-z_][A-Za-z0-9_]*\).*/\1:\2/p' \
        "$REPORT" || true)"
parsed_lines=0
[ -n "$keys" ] && parsed_lines="$(printf '%s\n' "$keys" | wc -l)"

if [ "$parsed_lines" -ne "$survivor_lines" ]; then
  echo "mutation gate: could not parse $((survivor_lines - parsed_lines)) of $survivor_lines" >&2
  echo "  survivor line(s) in '$REPORT'. An unparsed survivor is an unexamined survivor," >&2
  echo "  so this is a failure, not a pass. Fix the parser in tools/mutation-gate.sh to" >&2
  echo "  match the report format before trusting any verdict from it." >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# Step 3: reconcile against the report's own arithmetic.
#
# Mull states how many mutants it killed out of how many it generated. If that
# does not agree with the number of survivor lines this script found, then one
# of the two readings is wrong -- and a gate that cannot reconcile its evidence
# has no business returning success.
# ---------------------------------------------------------------------------
killed_summary="$(grep -oE 'Killed mutants[:( ]+[0-9]+/[0-9]+' "$REPORT" | tail -n 1 || true)"
if [ -n "$killed_summary" ]; then
  killed="$(printf '%s' "$killed_summary" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f1)"
  total="$(printf '%s' "$killed_summary" | grep -oE '[0-9]+/[0-9]+' | cut -d/ -f2)"
  if [ "$total" -lt "$killed" ]; then
    echo "mutation gate: summary claims $killed killed of $total generated, which is" >&2
    echo "  arithmetically impossible. Refusing to certify an incoherent report." >&2
    exit 1
  fi
  expected=$((total - killed))
  if [ "$expected" -ne "$survivor_lines" ]; then
    echo "mutation gate: the report disagrees with itself -- the summary implies" >&2
    echo "  $expected survivor(s) ($killed killed of $total), but $survivor_lines survivor" >&2
    echo "  line(s) are present. Either the report is truncated or this script is reading" >&2
    echo "  it wrongly. Both are failures." >&2
    exit 1
  fi
fi

survivors="$(printf '%s' "$keys" | sort -u)"

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
