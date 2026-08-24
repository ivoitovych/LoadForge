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
  --gcov-ignore-parse-errors=negative_hits.warn \
  --print-summary \
  --html-details "$BUILD_DIR/coverage.html" \
  --xml "$BUILD_DIR/coverage.xml" \
  --fail-under-line 100 \
  --fail-under-branch 100 \
  "$BUILD_DIR"
