#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the trust-chain tooling.
#
# docs/PLAN.md §7.3 makes mutation testing the metric that decides whether the
# test suite is any good, and tools/check-dependencies.py is what stands between
# the build and an unreviewed supply chain. Both are trust-critical: a bug in
# either silently weakens every other guarantee, and both have had exactly such
# a bug.
#
# The class of bug worth naming, because both instances were of it: FAILING
# OPEN. Not "the tool said pass when it should have said fail" but "the tool
# could not understand its evidence, and reported pass anyway". The mutation
# gate certified a report reading 'this is not a mull report at all'. The
# dependency checker certified an empty manifest while the build fetched an
# undeclared dependency. Neither had a broken rule -- each simply never reached
# its rules and defaulted to success.
#
# So most of the cases below feed these tools evidence they should refuse to
# interpret, and assert refusal. The machinery enforcing verification belongs
# inside the verification perimeter.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIX="$ROOT/tests/tools/fixtures"
GATE="$ROOT/tools/mutation-gate.sh"
DEPS="$ROOT/tools/check-dependencies.py"
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
  # The allowlist is passed as an argument rather than copied over the real one
  # and copied back: a test run must not edit a tracked file, and the restore
  # never happens if the run is interrupted.
  local out; out="$("$GATE" dummy-binary "$FIX/$1" "$FIX/$2" 2>&1)"; local rc=$?
  GATE_OUT="$out"; return $rc
}

echo "mutation-gate.sh"
gate clean.txt allow-empty.txt;         expect "clean report passes"                    0 $? "0 unexplained" "$GATE_OUT"
gate two-survivors.txt allow-empty.txt; expect "unexplained survivors fail"             1 $? "UNEXPLAINED SURVIVOR" "$GATE_OUT"
gate two-survivors.txt allow-one.txt;   expect "allowed survivor excused, other fails"  1 $? "allowed (reviewed)" "$GATE_OUT"
gate clean.txt allow-one.txt;           expect "stale allowlist entry fails"            1 $? "STALE ALLOWLIST ENTRY" "$GATE_OUT"

# The fail-closed cases. Each of these previously produced "0 unexplained, ok".
gate malformed.txt allow-empty.txt
expect "unrecognisable report FAILS (no completion marker)"    1 $? "no Mull summary line" "$GATE_OUT"
gate empty.txt allow-empty.txt
expect "empty report FAILS rather than reading as a clean run" 1 $? "no Mull summary line" "$GATE_OUT"
gate unparsed-survivor.txt allow-empty.txt
expect "survivor line the parser cannot read FAILS"            1 $? "could not parse 1 of 1" "$GATE_OUT"
gate count-mismatch.txt allow-empty.txt
expect "report that contradicts its own totals FAILS"          1 $? "disagrees with itself" "$GATE_OUT"
gate score-without-survivors.txt allow-empty.txt
expect "score below 100% with no survivors listed FAILS"       1 $? "lists no" "$GATE_OUT"
gate score-perfect-with-survivors.txt allow-empty.txt
expect "score of 100% alongside a survivor FAILS"              1 $? "incoherent report" "$GATE_OUT"

out="$("$GATE" dummy-binary "$FIX/no-such-report.txt" 2>&1)"
expect "missing report file FAILS" 1 $? "does not exist" "$out"

# The key must include the mutator: two operators at one site are distinct.
gate two-survivors.txt allow-one.txt
expect "location-only allowlisting cannot excuse a second mutator" 1 $? "cxx_remove_void_call" "$GATE_OUT"

echo "check-dependencies.py"
out="$(python3 "$DEPS" 2>&1)"; expect "real manifest validates" 0 $? "googletest" "$out"

# A throwaway root that mirrors the real one: NOTICE, third_party/, and the
# actual cmake/Dependencies.cmake, so the manifest/build cross-check is live in
# every case below rather than quietly skipped.
tmp="$(mktemp -d)"
mkdir -p "$tmp/third_party" "$tmp/cmake"
touch "$tmp/NOTICE"
cp "$ROOT/cmake/Dependencies.cmake" "$tmp/cmake/Dependencies.cmake"
GTEST_SHA="b514bdc898e2951020cbdca1304b75f5950d1f59"

manifest() { cat > "$tmp/third_party/MANIFEST.toml"; }

good_manifest() {
  manifest <<EOF
[[component]]
name      = "googletest"
version   = "v1.15.2"
sha       = "$GTEST_SHA"
source    = "https://github.com/google/googletest.git"
licence   = "BSD-3-Clause"
kind      = "fetched"
linked    = false
pinned_by = "cmake/Dependencies.cmake"
EOF
}

deps() { out="$(cd "$tmp" && python3 "$DEPS" 2>&1)"; return $?; }

good_manifest; deps
expect "reconstructed good manifest validates" 0 $? "pin matches" "$out"

# --- the fail-open case: nothing declared, yet the build still fetches --------
manifest <<'EOF'
# every component was deleted, or the file was truncated to comments
EOF
deps; expect "empty manifest FAILS while the build still fetches" 1 $? "no MANIFEST.toml component declares" "$out"

: | manifest
deps; expect "zero-byte manifest FAILS"                           1 $? "no MANIFEST.toml component declares" "$out"

manifest <<'EOF'
[[component]
name = "broken
EOF
deps; expect "unparseable manifest FAILS"                         1 $? "does not parse as TOML" "$out"

# --- the pin cross-check, in both directions ---------------------------------
good_manifest
sed -i "s/$GTEST_SHA/0000000000000000000000000000000000000000/" "$tmp/third_party/MANIFEST.toml"
deps; expect "manifest SHA that the build does not pin FAILS"     1 $? "does not appear in" "$out"

good_manifest
sed -i '/^pinned_by/d' "$tmp/third_party/MANIFEST.toml"
deps; expect "fetched component with no pinned_by FAILS"          1 $? "must name the file that pins it" "$out"

good_manifest
sed -i 's|https://github.com/google/googletest.git|https://example.invalid/x.git|' \
       "$tmp/third_party/MANIFEST.toml"
deps; expect "source that disagrees with the build FAILS"         1 $? "which no MANIFEST.toml component declares" "$out"

# --- field-level validation --------------------------------------------------
good_manifest
sed -i 's|BSD-3-Clause|Proprietary|' "$tmp/third_party/MANIFEST.toml"
deps; expect "non-allowlisted licence rejected"                   1 $? "not on the GPL-compatible allowlist" "$out"

good_manifest
sed -i "s/$GTEST_SHA/v1.15.2/" "$tmp/third_party/MANIFEST.toml"
deps; expect "movable tag rejected where a SHA is required"       1 $? "40-character commit SHA" "$out"

good_manifest
sed -i '/^linked/d' "$tmp/third_party/MANIFEST.toml"
deps; expect "missing required field rejected"                    1 $? "missing required field" "$out"

good_manifest
sed -i 's/^linked    = false/linked    = "false"/' "$tmp/third_party/MANIFEST.toml"
deps; expect "quoted boolean rejected (a string is always truthy)" 1 $? "must be a TOML boolean" "$out"

good_manifest
sed -i "s|^sha       = \"$GTEST_SHA\"|sha       = 12345|" "$tmp/third_party/MANIFEST.toml"
deps; expect "non-string sha rejected"                            1 $? "must be a non-empty string" "$out"

good_manifest
sed -i 's/^kind      = "fetched"/kind      = "borrowed"/' "$tmp/third_party/MANIFEST.toml"
deps; expect "unknown kind rejected"                              1 $? "it must be one of" "$out"

good_manifest
sed -i 's/^licence   =/license   =/' "$tmp/third_party/MANIFEST.toml"
deps; expect "misspelt field rejected rather than ignored"        1 $? "unknown field" "$out"

good_manifest
{ printf '\n'; sed -n '/\[\[component\]\]/,$p' "$tmp/third_party/MANIFEST.toml"; } \
  >> "$tmp/third_party/MANIFEST.toml"
deps; expect "duplicate component name rejected"                  1 $? "declared twice" "$out"

good_manifest
mkdir -p "$tmp/third_party/undeclared-library"
deps; expect "vendored directory nobody declared FAILS"           1 $? "present but not declared" "$out"
rmdir "$tmp/third_party/undeclared-library"

good_manifest
rm "$tmp/NOTICE"
deps; expect "missing NOTICE FAILS"                               1 $? "NOTICE is missing" "$out"
touch "$tmp/NOTICE"

rm "$tmp/third_party/MANIFEST.toml"
deps; expect "missing manifest FAILS"                             1 $? "MANIFEST.toml is missing" "$out"

rm -rf "$tmp"

echo "check-exclusions.sh"
out="$("$ROOT/tools/check-exclusions.sh" 2>&1)"
expect "the real tree has zero exclusions" 0 $? "0 marker(s)" "$out"

# A synthetic root, because the point of these cases is source that the real
# tree must never contain.
ex="$(mktemp -d)"; mkdir -p "$ex/src" "$ex/tools"
excl() { # source-body-on-stdin, then reviewed-file contents as $1
  printf '%s' "${1-}" > "$ex/tools/coverage-exclusions.txt"
  out="$("$ROOT/tools/check-exclusions.sh" "$ex" src 2>&1)"; return $?
}

cat > "$ex/src/a.cpp" <<'EOF'
int f(int x) { return x; }
EOF
excl ""; expect "empty tree passes"                          0 $? "0 marker(s)" "$out"

cat > "$ex/src/a.cpp" <<'EOF'
void f() { abort(); }  // GCOVR_EXCL_LINE  abort() does not return
EOF
excl ""
expect "unreviewed exclusion FAILS"                          1 $? "UNREVIEWED EXCLUSION" "$out"

excl 'src/a.cpp:GCOVR_EXCL_LINE:abort() does not return'
expect "reviewed exclusion passes"                           0 $? "reviewed: src/a.cpp" "$out"

excl 'src/a.cpp:GCOVR_EXCL_LINE:some entirely different reason'
expect "reworded justification needs re-review"              1 $? "UNREVIEWED EXCLUSION" "$out"

cat > "$ex/src/a.cpp" <<'EOF'
void f() { abort(); }  // GCOVR_EXCL_LINE
EOF
excl 'src/a.cpp:GCOVR_EXCL_LINE:abort() does not return'
expect "exclusion with no written reason FAILS"              1 $? "NO WRITTEN REASON" "$out"

cat > "$ex/src/a.cpp" <<'EOF'
int f(int x) { return x; }
EOF
excl 'src/a.cpp:GCOVR_EXCL_LINE:abort() does not return'
expect "stale reviewed entry FAILS"                          1 $? "STALE EXCLUSION ENTRY" "$out"

cat > "$ex/src/a.cpp" <<'EOF'
// GCOVR_EXCL_START  the whole block needs hardware CI does not have
void f() { }
EOF
excl 'src/a.cpp:GCOVR_EXCL_START:the whole block needs hardware CI does not have'
expect "region left open to end of file FAILS"               1 $? "UNBALANCED EXCLUSION REGION" "$out"

cat >> "$ex/src/a.cpp" <<'EOF'
// GCOVR_EXCL_STOP
EOF
excl 'src/a.cpp:GCOVR_EXCL_START:the whole block needs hardware CI does not have'
expect "balanced region with a reviewed reason passes"       0 $? "reviewed: src/a.cpp" "$out"

rm -rf "$ex"

echo; echo "  tooling tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
