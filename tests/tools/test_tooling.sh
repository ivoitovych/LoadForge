#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the trust-chain tooling.
#
# docs/PLAN.md §7.3 makes mutation testing the metric that decides whether the
# test suite is any good. That makes mutation-gate.sh trust-critical: a bug in
# it silently weakens every other guarantee. It had exactly such a bug -- Mull
# exits non-zero on survivors, so under 'set -e' the script died before reaching
# the allowlist logic, leaving the reviewed-equivalent mechanism as dead code
# that looked correct only because the allowlist was empty.
#
# The machinery enforcing verification belongs inside the verification perimeter.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIX="$ROOT/tests/tools/fixtures"
GATE="$ROOT/tools/mutation-gate.sh"
DEPS="$ROOT/tools/check-dependencies.py"
ALLOWLIST="$ROOT/tools/mutation-allowlist.txt"
pass=0; fail=0

expect() { # description, expected-exit, actual-exit, [needle, output]
  if [ "$2" -eq "$3" ] && { [ $# -lt 5 ] || grep -q "$4" <<< "$5"; }; then
    echo "  PASS  $1"; pass=$((pass + 1))
  else
    echo "  FAIL  $1 (expected exit $2, got $3)"; fail=$((fail + 1))
    [ $# -ge 5 ] && echo "$5" | sed 's/^/          /'
  fi
}

gate() { # report-fixture, allowlist-fixture
  local saved; saved="$(cat "$ALLOWLIST")"
  cp "$FIX/$2" "$ALLOWLIST"
  local out; out="$("$GATE" dummy-binary "$FIX/$1" 2>&1)"; local rc=$?
  printf '%s' "$saved" > "$ALLOWLIST"
  GATE_OUT="$out"; return $rc
}

echo "mutation-gate.sh"
gate clean.txt allow-empty.txt;         expect "clean report passes"                    0 $? "0 unexplained" "$GATE_OUT"
gate two-survivors.txt allow-empty.txt; expect "unexplained survivors fail"             1 $? "UNEXPLAINED SURVIVOR" "$GATE_OUT"
gate two-survivors.txt allow-one.txt;   expect "allowed survivor excused, other fails"  1 $? "allowed (reviewed)" "$GATE_OUT"
gate clean.txt allow-one.txt;           expect "stale allowlist entry fails"            1 $? "STALE ALLOWLIST ENTRY" "$GATE_OUT"
gate malformed.txt allow-empty.txt;     expect "malformed report yields no survivors"   0 $? "0 unexplained" "$GATE_OUT"

# The key must include the mutator: two operators at one site are distinct.
gate two-survivors.txt allow-one.txt
expect "location-only allowlisting cannot excuse a second mutator" 1 $? "cxx_remove_void_call" "$GATE_OUT"

echo "check-dependencies.py"
out="$(python3 "$DEPS" 2>&1)"; expect "real manifest validates" 0 $? "googletest" "$out"

tmp="$(mktemp -d)"; mkdir -p "$tmp/third_party"; touch "$tmp/NOTICE"
cat > "$tmp/third_party/MANIFEST.toml" <<'EOF'
[[component]]
name    = "badlicence"
version = "1.0"
sha     = "b514bdc898e2951020cbdca1304b75f5950d1f59"
source  = "https://example.invalid/x.git"
licence = "Proprietary"
kind    = "fetched"
linked  = "false"
EOF
out="$(cd "$tmp" && python3 "$DEPS" 2>&1)"; expect "non-allowlisted licence rejected" 1 $? "not on the GPL-compatible allowlist" "$out"

sed -i 's|sha     = "b514.*"|sha     = "v1.15.2"|' "$tmp/third_party/MANIFEST.toml"
sed -i 's|Proprietary|MIT|' "$tmp/third_party/MANIFEST.toml"
out="$(cd "$tmp" && python3 "$DEPS" 2>&1)"; expect "movable tag rejected where a SHA is required" 1 $? "40-character commit SHA" "$out"

sed -i '/^linked/d' "$tmp/third_party/MANIFEST.toml"
out="$(cd "$tmp" && python3 "$DEPS" 2>&1)"; expect "missing required field rejected" 1 $? "missing required field" "$out"
rm -rf "$tmp"

echo; echo "  tooling tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
