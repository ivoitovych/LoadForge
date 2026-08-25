#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# End-to-end test of the mutation gate against the REAL Mull binary.
#
# WHY THIS EXISTS
# ---------------
# tests/tools/test_tooling.sh drives the gate with fixture reports. Every one of
# those tests passed while the gate was fundamentally broken: it read the mutator
# as the first word after "Survived:", which on real Mull output is the word
# "Replaced". Two different operators at one source location therefore collapsed
# to one key, and a single reviewed allowlist entry would have excused both --
# the precise failure the key was widened to prevent.
#
# The fixtures could not catch it because they were written from the same wrong
# idea of Mull's output as the parser. A model of an external tool cannot falsify
# itself. Only the tool can.
#
# So this test compiles a program with a known-surviving mutant, runs real Mull,
# and asserts:
#
#   1. Mull generates the expected survivor;
#   2. the gate parses its exact mutator identifier;
#   3. the gate fails on it;
#   4. the exact reviewed key permits that mutant and only that mutant;
#   5. a key naming a DIFFERENT operator at the SAME line and column does not
#      excuse it.
#
# Point 5 is the one that was silently broken, and it is why this file exists.
#
# It is not part of the ordinary CI run because Mull is not pinned yet (see
# docs/PLAN.md §11). The mutation workflow runs it once Mull is installed, before
# the gate is used on the project's own tests. It hard-fails rather than skips
# when Mull is absent: a test that reports success without running is the same
# defect this whole pass is about.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE="$ROOT/tools/mutation-gate.py"
LLVM="${LLVM_VERSION:-18}"
RUNNER="${MULL_RUNNER:-mull-runner-$LLVM}"
FRONTEND="${MULL_FRONTEND:-/usr/lib/mull-ir-frontend-$LLVM}"
CXX="${CXX:-clang++-$LLVM}"
pass=0; fail=0

check() { # description, expected-exit, actual-exit, needle, output
  if [ "$2" -eq "$3" ] && grep -q -- "$4" <<< "$5"; then
    echo "  PASS  $1"; pass=$((pass + 1))
  else
    echo "  FAIL  $1 (expected exit $2, got $3, looking for '$4')"; fail=$((fail + 1))
    echo "$5" | sed 's/^/          /'
  fi
}

for tool in "$RUNNER" "$CXX"; do
  command -v "$tool" >/dev/null || {
    echo "mutation integration: $tool not found -- refusing to report success" >&2
    echo "  Install the pinned Mull package and the matching clang, or set" >&2
    echo "  MULL_RUNNER / CXX / LLVM_VERSION. A skipped test is not a passed test." >&2
    exit 1
  }
done
[ -e "$FRONTEND" ] || {
  echo "mutation integration: Mull IR frontend not found at $FRONTEND" >&2
  echo "  Set MULL_FRONTEND to the plugin shipped with the pinned Mull package." >&2
  exit 1
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work" || exit 1

# Restricting the mutator set keeps the expected outcome exact. Both operators
# below rewrite the SAME '>=' at the same line and column, which is what makes
# this a real test of the survivor key rather than of the happy path.
cat > mull.yml <<'EOF'
mutators:
  - cxx_ge_to_gt
  - cxx_ge_to_lt
quiet: false
EOF

# classify(5) returns 0, and main asserts that.
#   cxx_ge_to_gt  ->  (n > 10)  ->  still 0  ->  test still passes  ->  SURVIVES
#   cxx_ge_to_lt  ->  (n < 10)  ->  now 1    ->  test fails         ->  KILLED
# One survivor, one killed, both at the same source position.
cat > subject.cpp <<'EOF'
int classify(int n) {
  return (n >= 10) ? 1 : 0;
}

int main() {
  return classify(5) == 0 ? 0 : 1;
}
EOF

export MULL_CONFIG="$work/mull.yml"
"$CXX" -O0 -g -fpass-plugin="$FRONTEND" subject.cpp -o subject || {
  echo "mutation integration: could not build the subject with the Mull plugin" >&2
  exit 1
}

echo "mutation integration (Mull: $("$RUNNER" --version 2>&1 | head -n 1))"

# The first call runs Mull and leaves both reports under build/mutation; every
# later call re-judges those same reports, so the four assertions below are all
# about one real mutation run rather than four separate ones.
OUT="$(python3 "$GATE" ./subject --runner "$RUNNER" --allowlist /dev/null --root "$work" 2>&1)"
rc=$?
check "real Mull output parses and the survivor is unexplained" 1 $rc "UNEXPLAINED SURVIVOR" "$OUT"
check "the exact mutator identifier is parsed, not the word 'Replaced'" 1 $rc "cxx_ge_to_gt" "$OUT"

key="$(grep -oE 'subject\.cpp:[0-9]+:[0-9]+:cxx_ge_to_gt' <<< "$OUT" | head -n 1)"
if [ -z "$key" ]; then
  echo "  FAIL  could not read the survivor key back from the gate's output"; fail=$((fail + 1))
else
  echo "  ...   survivor key: $key"
  location="${key%:*}"

  printf '%s\n' "$key" > allow-exact.txt
  OUT="$(python3 "$GATE" --report-dir build/mutation --allowlist allow-exact.txt --root "$work" 2>&1)"
  check "the exact reviewed key permits the mutant" 0 $? "allowed (reviewed)" "$OUT"

  # The whole point of putting the mutator in the key: a reviewed decision about
  # one operator must not silently cover a different operator at the same place.
  printf '%s\n' "$location:cxx_ge_to_lt" > allow-wrong.txt
  OUT="$(python3 "$GATE" --report-dir build/mutation --allowlist allow-wrong.txt --root "$work" 2>&1)"
  check "a different operator at the same location does NOT excuse it" 1 $? "UNEXPLAINED SURVIVOR" "$OUT"
  check "and the wrong entry is reported as stale" 1 $? "STALE ALLOWLIST ENTRY" "$OUT"
fi

echo; echo "  mutation integration: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
