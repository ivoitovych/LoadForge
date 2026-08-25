#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The coverage-exclusion gate (docs/PLAN.md §7.3).
#
# A 100% coverage gate is only worth what its exclusion list is worth. Marking a
# line GCOVR_EXCL_LINE removes it from both the numerator and the denominator,
# so an unreviewed exclusion does not lower the number -- it keeps it at 100%
# while quietly shrinking what 100% covers. Enough of them and the gate becomes
# decoration that no longer measures anything.
#
# The rule was written down from the start and then not enforced: CI counted the
# markers and printed the count. Counting is not a ratchet. A reviewer would have
# had to notice the number in a step summary and remember what it was last week.
#
# This script makes it real. Every exclusion in src/ must:
#
#   * carry a written reason on the same line -- "this cannot happen" is a claim,
#     and a claim with no argument next to it is the thing being guarded against;
#   * appear in tools/coverage-exclusions.txt, which is where the reason is
#     reviewed. Adding an exclusion means editing that file in the same change,
#     which puts it in the diff where a reviewer sees it.
#
# And every entry in that file must still correspond to a real exclusion, for
# the same reason the mutation allowlist rejects stale entries: a permission
# that outlives the thing it permitted is a permission nobody is reviewing.
#
# The key is path:marker:reason rather than path:line. Line numbers churn with
# every edit above them, which would turn the reviewed file into merge-conflict
# noise; and keying on the reason means that changing the justification requires
# re-review, which is exactly when re-review is warranted.
set -uo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SCAN="${2:-src}"
REVIEWED="$ROOT/tools/coverage-exclusions.txt"
MARKERS='GCOVR_EXCL_LINE|GCOVR_EXCL_START|GCOVR_EXCL_STOP|GCOV_EXCL_LINE|GCOV_EXCL_START|GCOV_EXCL_STOP|LCOV_EXCL_LINE|LCOV_EXCL_START|LCOV_EXCL_STOP'

bad=0
found=()
count=0

if [ -d "$ROOT/$SCAN" ]; then
  while IFS= read -r hit; do
    file="${hit%%:*}"; rest="${hit#*:}"
    line="${rest%%:*}"; text="${rest#*:}"
    marker="$(printf '%s' "$text" | grep -oE "$MARKERS" | head -n 1)"
    count=$((count + 1))
    relative="${file#"$ROOT"/}"

    # A STOP is bookkeeping, not a claim; it is checked for balance below.
    if [[ "$marker" == *_STOP ]]; then
      continue
    fi

    # Everything after the marker, minus comment punctuation, is the reason.
    reason="$(printf '%s' "${text#*"$marker"}" \
              | sed -e 's|\*/||g' -e 's/^[[:space:]:*-]*//' -e 's/[[:space:]]*$//')"
    if [ -z "$reason" ]; then
      echo "  NO WRITTEN REASON: $relative:$line ($marker)" >&2
      echo "    An exclusion asserts that a line cannot be reached. State why, on the" >&2
      echo "    same line, and record it in tools/coverage-exclusions.txt." >&2
      bad=$((bad + 1))
      continue
    fi
    found+=("$relative:$marker:$reason")
  done < <(grep -rnE "$MARKERS" "$ROOT/$SCAN" 2>/dev/null || true)
fi

# START/STOP must balance per file, or the excluded region runs to end of file
# and silently removes far more than anyone reviewed.
while IFS= read -r file; do
  [ -z "$file" ] && continue
  starts="$(grep -cE 'GCOVR_EXCL_START|GCOV_EXCL_START|LCOV_EXCL_START' "$file" || true)"
  stops="$(grep -cE 'GCOVR_EXCL_STOP|GCOV_EXCL_STOP|LCOV_EXCL_STOP' "$file" || true)"
  if [ "$starts" -ne "$stops" ]; then
    echo "  UNBALANCED EXCLUSION REGION: ${file#"$ROOT"/} ($starts START, $stops STOP)" >&2
    echo "    An unclosed region excludes everything to the end of the file." >&2
    bad=$((bad + 1))
  fi
done < <(grep -rlE "$MARKERS" "$ROOT/$SCAN" 2>/dev/null || true)

mapfile -t reviewed < <(grep -vE '^\s*(#|$)' "$REVIEWED" 2>/dev/null || true)

used=()
for key in "${found[@]:-}"; do
  [ -z "$key" ] && continue
  if printf '%s\n' "${reviewed[@]:-}" | grep -qxF "$key"; then
    echo "  reviewed: $key"
    used+=("$key")
  else
    echo "  UNREVIEWED EXCLUSION: $key" >&2
    echo "    Add it to tools/coverage-exclusions.txt in this same change, so the" >&2
    echo "    justification lands in the diff a reviewer is already reading." >&2
    bad=$((bad + 1))
  fi
done

for key in "${reviewed[@]:-}"; do
  [ -z "$key" ] && continue
  printf '%s\n' "${used[@]:-}" | grep -qxF "$key" || {
    echo "  STALE EXCLUSION ENTRY (no longer present in $SCAN/): $key" >&2
    bad=$((bad + 1))
  }
done

echo "coverage exclusions in $SCAN/: $count marker(s), ${#reviewed[@]} reviewed, $bad problem(s)"
[ "$bad" -eq 0 ]
