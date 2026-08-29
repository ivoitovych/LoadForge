#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every source file must appear in the coverage report.
#
# WHY THIS EXISTS
# ---------------
# gcovr reports on the .gcda files it finds. A source file that was never
# compiled into the measured build produces none, so it is not reported as
# uncovered -- it is not reported at all, and 100% of what remains is still
# 100%. The percentage is then true and worthless.
#
# This is not hypothetical. It happened while adding src/platform/exit_status.cpp:
# the gate printed "lines: 100.0% (301 out of 301)" over a build directory that
# predated the file. Once the file was actually compiled in, the real figure was
# 99.7% with an uncovered branch. The gate certified the new code as perfectly
# covered without ever having seen it.
#
# CI never hits this, because it configures and builds from a fresh checkout on
# every run -- which is exactly why it survived so long: the one place the hazard
# does not exist is the one place the gate runs automatically. Locally, where a
# contributor forms their confidence before pushing, a stale build directory is
# the normal state rather than the exception.
#
# The check compares against the SOURCE TREE rather than against a checked-in
# list, so a newly added file is protected on the day it is added and there is
# nothing to remember to update. A list would have needed updating by the same
# person who forgot to rebuild.
set -euo pipefail

REPORT="${1:?usage: check-coverage-complete.sh <coverage.xml> [source-dir]}"
SOURCE_DIR="${2:-src}"

[ -f "$REPORT" ] || {
  echo "check-coverage-complete: '$REPORT' does not exist." >&2
  echo "  No report was read, so nothing is certified. A missing report is a" >&2
  echo "  failure, not a pass (docs/PLAN.md F20)." >&2
  exit 1
}

[ -d "$SOURCE_DIR" ] || {
  echo "check-coverage-complete: '$SOURCE_DIR' is not a directory." >&2
  echo "  With no sources to compare against, this check would pass vacuously." >&2
  exit 1
}

# gcovr writes filenames relative to its --root, which is the repository root, so
# the comparison key must be "src/platform/fs.cpp" and never an absolute path.
# find echoes back whatever prefix it was given, so the directory is split and
# entered first: that makes the key relative to SOURCE_DIR's parent whether the
# caller passed `src` or /abs/path/to/src, instead of quietly matching nothing
# and reporting every file as missing.
REPORT="$(cd "$(dirname "$REPORT")" && pwd)/$(basename "$REPORT")"
source_parent="$(cd "$(dirname "$SOURCE_DIR")" && pwd)"
source_name="$(basename "$SOURCE_DIR")"
cd "$source_parent"

mapfile -t sources < <(find "$source_name" -name '*.cpp' | sort)

# A source tree with no .cpp files means the caller pointed this somewhere wrong.
# Reporting "all 0 files present" would be the vacuous pass this gate exists to
# prevent, so it is a failure instead.
if [ "${#sources[@]}" -eq 0 ]; then
  echo "check-coverage-complete: no .cpp files under '$SOURCE_DIR'." >&2
  echo "  An empty comparison set makes this check vacuous, so it fails." >&2
  exit 1
fi

missing=()
for source in "${sources[@]}"; do
  grep -qF "filename=\"${source}\"" "$REPORT" || missing+=("$source")
done

if [ "${#missing[@]}" -ne 0 ]; then
  echo "" >&2
  echo "coverage: ${#missing[@]} source file(s) absent from the report:" >&2
  printf '    %s\n' "${missing[@]}" >&2
  echo "" >&2
  echo "They were not measured, so the coverage percentage describes only the" >&2
  echo "files that were. Usually the build directory is stale:" >&2
  echo "" >&2
  echo "    cmake --preset coverage && cmake --build --preset coverage" >&2
  echo "    find build/coverage -name '*.gcda' -delete && ctest --preset coverage" >&2
  echo "" >&2
  exit 1
fi

echo "coverage report covers all ${#sources[@]} source file(s) under $SOURCE_DIR/"
