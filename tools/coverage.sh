#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The coverage gate: 100% line and 100% branch, per docs/PLAN.md §7.3.
#
# --exclude-throw-branches / --exclude-unreachable-branches matter. Raw gcov
# counts compiler-generated exception edges from nearly every call, which are
# unreachable by design; without excluding them 100% branch coverage would be
# simultaneously unattainable and meaningless. With them excluded, the number
# describes the code's own decisions.
#
# Coverage is a MINIMUM COMPLETENESS CONDITION, not evidence the tests are good.
# That evidence comes from the oracle, fault-injection, determinism and
# supervision tiers, plus mutation testing.
#
# There is deliberately no --gcov-ignore-parse-errors here. It was present, set
# to negative_hits.warn, and it is exactly the fail-open shape F20 is about: a
# gate told to shrug at data it could not parse and report a number anyway.
# Nothing needs it -- GCC 13.3 and gcovr 8.6, the pinned combination CI uses,
# produce a clean parse. If gcov output ever stops parsing, this must fail and
# be looked at, not average over the parts it understood.
set -euo pipefail

BUILD_DIR="${1:-build/coverage}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

gcovr \
  --root . \
  --filter 'src/' \
  --exclude-throw-branches \
  --exclude-unreachable-branches \
  --exclude-noncode-lines \
  --print-summary \
  --html-details "$BUILD_DIR/coverage.html" \
  --xml "$BUILD_DIR/coverage.xml" \
  --fail-under-line 100 \
  --fail-under-branch 100 \
  "$BUILD_DIR"

# EVERY SOURCE FILE MUST APPEAR IN THE REPORT
# -------------------------------------------
# gcovr reports on the .gcda files it finds. A source file that was never
# compiled into the measured build produces none, so it is not reported as
# uncovered -- it is not reported at all, and 100% of what remains is still
# 100%. The percentage is then true and worthless.
#
# This is not hypothetical. It happened while adding exit_status.cpp: the gate
# printed "lines: 100.0% (301 out of 301)" over a build directory that predated
# the file, and the real figure once the file was actually compiled in was 99.7%
# with an uncovered branch. The gate said the new code was perfectly covered
# without ever having seen it.
#
# CI never hits this, because it configures and builds from a fresh checkout
# every time -- which is exactly why it survived: the one place the hazard does
# not exist is the one place the gate runs automatically. Locally, where a
# contributor forms their confidence before pushing, a stale build directory is
# the normal state.
#
# So this is the F20 rule applied to the gate's own inputs: evidence that is
# missing must fail, not pass quietly. The check lives in its own script so that
# it can be tested against a report with a file removed -- a gate whose failing
# case is never exercised is decoration.
"$ROOT/tools/check-coverage-complete.sh" "$BUILD_DIR/coverage.xml"
