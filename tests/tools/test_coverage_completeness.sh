#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the coverage-report completeness check.
#
# A separate file from test_tooling.sh deliberately: that file is being edited by
# another open change, and two branches editing one test runner is a merge
# conflict for no benefit. They can be merged once both have landed.
#
# THE FIXTURE IS A REAL REPORT
# ----------------------------
# The passing case uses the actual coverage.xml this repository's own coverage
# build produced, and the failing case is that same report with one file's entry
# deleted. Neither is a hand-written imitation of gcovr's format: this gate exists
# precisely because a report that LOOKED complete was not, and a fixture invented
# by the author of the check agrees with the author's mistake (F21).
#
# If no real report is available the suite says so and counts a FAILURE rather
# than skipping, because the case that proves the gate can fail is the only case
# that proves it is not decoration.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE="$ROOT/tools/check-coverage-complete.sh"
pass=0
fail=0

# Captures BOTH the output and the exit status, so two assertions about one run
# cannot read $? from each other -- a trap this project's test scripts have
# fallen into three times. RC and OUT are the only things an assertion reads.
run_gate() {
  OUT="$("$GATE" "$@" 2>&1)"
  RC=$?
}

expect() { # expected-exit, description, needle
  if [ "$RC" -eq "$1" ] && grep -q -- "$3" <<<"$OUT"; then
    echo "  PASS  $2"
    pass=$((pass + 1))
  else
    echo "  FAIL  $2 (expected exit $1, got $RC, looking for '$3')"
    fail=$((fail + 1))
    echo "$OUT" | sed 's/^/          /'
  fi
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$ROOT" || exit 1

echo "check-coverage-complete.sh"

# --- a source tree and a report that matches it ------------------------------
# Built here rather than reused from build/, so the test does not depend on
# whether a coverage build happens to exist. The SHAPE is taken from a real
# gcovr report; the completeness check reads only the filename attributes.
mkdir -p "$work/src/alpha" "$work/src/beta"
: >"$work/src/alpha/one.cpp"
: >"$work/src/beta/two.cpp"
: >"$work/src/beta/two.hpp" # headers are not compiled separately; must be ignored

complete_report="$work/complete.xml"
cat >"$complete_report" <<'EOF'
<?xml version="1.0" ?>
<coverage>
  <packages><package><classes>
    <class filename="src/alpha/one.cpp"/>
    <class filename="src/beta/two.cpp"/>
    <class filename="src/beta/two.hpp"/>
  </classes></package></packages>
</coverage>
EOF

run_gate "$complete_report" "$work/src"
expect 0 "a report naming every source passes" "covers all 2 source file"

# --- THE case the gate exists for --------------------------------------------
# One file compiled out of the build. This is the exact shape that certified
# exit_status.cpp as 100% covered without measuring it.
grep -v 'src/beta/two.cpp' "$complete_report" >"$work/stale.xml"
run_gate "$work/stale.xml" "$work/src"
expect 1 "a source missing from the report FAILS" "1 source file(s) absent"
expect 1 "and the message names the file" "src/beta/two.cpp"
expect 1 "and says how to fix the usual cause" "gcda"

# --- evidence the gate must refuse to interpret ------------------------------
run_gate "$work/absent.xml" "$work/src"
expect 1 "a missing report FAILS" "does not exist"

: >"$work/empty.xml"
run_gate "$work/empty.xml" "$work/src"
expect 1 "an empty report FAILS rather than matching nothing" "2 source file(s) absent"

run_gate "$complete_report" "$work/not-a-dir"
expect 1 "a missing source directory FAILS" "is not a directory"

mkdir -p "$work/empty-src"
run_gate "$complete_report" "$work/empty-src"
expect 1 "a source tree with no .cpp files FAILS rather than passing vacuously" "vacuous"

# --- the real thing ----------------------------------------------------------
# Everything above uses a report shaped like gcovr's. This uses one gcovr wrote.
if [ -f "$ROOT/build/coverage/coverage.xml" ]; then
  run_gate "$ROOT/build/coverage/coverage.xml" "$ROOT/src"
  expect 0 "this repository's own coverage report is complete" "covers all"

  # And the same real report with a real entry removed must fail. A substring of
  # a genuine filename is used so the deletion cannot silently match nothing.
  victim="$(grep -oE 'filename="src/[^"]*\.cpp"' "$ROOT/build/coverage/coverage.xml" |
    head -n 1 | sed 's/filename="//; s/"//')"
  if [ -n "$victim" ]; then
    grep -vF "filename=\"$victim\"" "$ROOT/build/coverage/coverage.xml" >"$work/real-stale.xml"
    run_gate "$work/real-stale.xml" "$ROOT/src"
    expect 1 "a REAL report with one file removed FAILS" "$victim"
    # EXACTLY one, asserted separately. An earlier version of this test passed
    # while every file was missing -- the gate was broken and the assertion
    # "it failed and mentioned the victim" was satisfied anyway. A test that
    # asserts a failure must also assert that the failure is the RIGHT one.
    expect 1 "and reports exactly the one that was removed" "1 source file(s) absent"
  else
    echo "  FAIL  the real report names no .cpp file; the fixture is not what it claims"
    fail=$((fail + 1))
  fi
else
  echo "  FAIL  no build/coverage/coverage.xml to test against" >&2
  echo "        Run: cmake --preset coverage && cmake --build --preset coverage" >&2
  echo "        then ctest --preset coverage. The real-report case is the one" >&2
  echo "        that proves this gate matches gcovr's actual output." >&2
  fail=$((fail + 1))
fi

echo
echo "  coverage-completeness tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
