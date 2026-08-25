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
# Every exclusion in src/ must carry an ID of the form LF-COV-NNN, and that ID
# must appear in tools/coverage-exclusions.txt with a written justification:
#
#     if (rc < 0) { abort(); }  // GCOVR_EXCL_LINE LF-COV-001 abort() cannot return
#
# WHY AN ID AND NOT THE REASON TEXT
# ---------------------------------
# The first version keyed the reviewed file on path:marker:reason, which had a
# hole: two exclusions in one file, with the same marker and the same wording,
# generate one key, so a single reviewed record silently satisfies both. The
# claim "adding an exclusion requires editing the reviewed file" was therefore
# not quite true, which is the kind of almost-true that this project's gates keep
# getting caught by. An explicit ID is unique by construction; adding an
# exclusion means allocating the next number, which cannot be done by accident.
#
# The count is zero. This is the cheapest possible moment to establish the scheme.
set -uo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SCAN="${2:-src}"
REVIEWED="${3:-$ROOT/tools/coverage-exclusions.txt}"
MARKERS='GCOVR_EXCL_LINE|GCOVR_EXCL_START|GCOVR_EXCL_STOP|GCOV_EXCL_LINE|GCOV_EXCL_START|GCOV_EXCL_STOP|LCOV_EXCL_LINE|LCOV_EXCL_START|LCOV_EXCL_STOP'
ID_RE='LF-COV-[0-9]{3}'

bad=0
count=0
declare -A seen_id_at=()
found=()

if [ -d "$ROOT/$SCAN" ]; then
  while IFS= read -r hit; do
    file="${hit%%:*}"; rest="${hit#*:}"
    line="${rest%%:*}"; text="${rest#*:}"
    marker="$(grep -oE "$MARKERS" <<< "$text" | head -n 1)"
    relative="${file#"$ROOT"/}"
    count=$((count + 1))

    # A STOP closes a region opened elsewhere; the ID and the reason live on the
    # START. Sequence validity is checked below.
    [[ "$marker" == *_STOP ]] && continue

    tail_text="${text#*"$marker"}"
    id="$(grep -oE "$ID_RE" <<< "$tail_text" | head -n 1)"
    if [ -z "$id" ]; then
      echo "  NO EXCLUSION ID: $relative:$line ($marker)" >&2
      echo "    Every exclusion carries an ID: '$marker LF-COV-NNN <reason>'. Allocate the" >&2
      echo "    next free number in tools/coverage-exclusions.txt and record the reason there." >&2
      bad=$((bad + 1)); continue
    fi
    if [ -n "${seen_id_at[$id]:-}" ]; then
      echo "  DUPLICATE EXCLUSION ID: $id at $relative:$line and ${seen_id_at[$id]}" >&2
      echo "    One reviewed record must not cover two exclusions. Allocate a new ID." >&2
      bad=$((bad + 1)); continue
    fi
    seen_id_at[$id]="$relative:$line"

    reason="$(sed -e "s/.*$id//" -e 's|\*/||g' -e 's/^[[:space:]:*-]*//' -e 's/[[:space:]]*$//' \
              <<< "$tail_text")"
    if [ -z "$reason" ]; then
      echo "  NO WRITTEN REASON: $relative:$line ($id)" >&2
      echo "    An exclusion asserts that a line cannot be reached. State why, on the same line." >&2
      bad=$((bad + 1)); continue
    fi
    found+=("$id $relative $marker")
  done < <(grep -rnE "$MARKERS" "$ROOT/$SCAN" 2>/dev/null || true)
fi

# START/STOP must nest correctly, in order. Equal counts are not enough: a STOP
# before its START balances arithmetically while leaving the final region open to
# the end of the file, silently excluding everything after it.
while IFS= read -r file; do
  [ -z "$file" ] && continue
  relative="${file#"$ROOT"/}"
  depth=0; opened_at=0; lineno=0
  while IFS= read -r text; do
    lineno=$((lineno + 1))
    case "$text" in
      *GCOVR_EXCL_START*|*GCOV_EXCL_START*|*LCOV_EXCL_START*)
        if [ "$depth" -ne 0 ]; then
          echo "  NESTED EXCLUSION REGION: $relative:$lineno (already open since line $opened_at)" >&2
          bad=$((bad + 1))
        fi
        depth=$((depth + 1)); opened_at=$lineno ;;
      *GCOVR_EXCL_STOP*|*GCOV_EXCL_STOP*|*LCOV_EXCL_STOP*)
        if [ "$depth" -eq 0 ]; then
          echo "  EXCLUSION STOP WITH NO START: $relative:$lineno" >&2
          bad=$((bad + 1))
        else
          depth=$((depth - 1))
        fi ;;
    esac
  done < "$file"
  if [ "$depth" -ne 0 ]; then
    echo "  UNCLOSED EXCLUSION REGION: $relative (opened at line $opened_at)" >&2
    echo "    An unclosed region excludes everything to the end of the file." >&2
    bad=$((bad + 1))
  fi
done < <(grep -rlE "$MARKERS" "$ROOT/$SCAN" 2>/dev/null || true)

# The reviewed file: 'LF-COV-NNN <path> <marker> -- <justification>'.
declare -A reviewed=()
while IFS= read -r entry; do
  [ -z "$entry" ] && continue
  id="$(grep -oE "^$ID_RE" <<< "$entry" || true)"
  if [ -z "$id" ]; then
    echo "  MALFORMED REVIEWED ENTRY (must start with LF-COV-NNN): $entry" >&2
    bad=$((bad + 1)); continue
  fi
  if [ -n "${reviewed[$id]:-}" ]; then
    echo "  DUPLICATE REVIEWED ID: $id" >&2
    bad=$((bad + 1)); continue
  fi
  reviewed[$id]="$entry"
done < <(grep -vE '^\s*(#|$)' "$REVIEWED" 2>/dev/null || true)

used=()
for record in "${found[@]:-}"; do
  [ -z "$record" ] && continue
  read -r id relative marker <<< "$record"
  entry="${reviewed[$id]:-}"
  if [ -z "$entry" ]; then
    echo "  UNREVIEWED EXCLUSION: $id at $relative ($marker)" >&2
    echo "    Add it to tools/coverage-exclusions.txt in this same change, so the" >&2
    echo "    justification lands in the diff a reviewer is already reading." >&2
    bad=$((bad + 1)); continue
  fi
  # The reviewed record names where the exclusion is; if it moves, that is a
  # change worth seeing rather than one the gate should absorb.
  if ! grep -qF " $relative " <<< " $entry "; then
    echo "  EXCLUSION MOVED: $id is at $relative, reviewed as: $entry" >&2
    bad=$((bad + 1)); continue
  fi
  echo "  reviewed: $id $relative ($marker)"
  used+=("$id")
done

for id in "${!reviewed[@]}"; do
  printf '%s\n' "${used[@]:-}" | grep -qxF "$id" || {
    echo "  STALE EXCLUSION ENTRY (no longer present in $SCAN/): $id" >&2
    bad=$((bad + 1))
  }
done

echo "coverage exclusions in $SCAN/: $count marker(s), ${#reviewed[@]} reviewed, $bad problem(s)"
[ "$bad" -eq 0 ]
