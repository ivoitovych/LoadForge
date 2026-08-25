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
GATE="$ROOT/tools/mutation-gate.py"
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

gate() { # report-fixture-dir, allowlist-fixture
  local out; out="$(python3 "$GATE" --report-dir "$FIX/mull/$1" \
                    --allowlist "$FIX/$2" --root "$ROOT" 2>&1)"; local rc=$?
  GATE_OUT="$out"; GATE_RC=$rc; return $rc
}

echo "mutation-gate.py"
gate clean allow-empty.txt
expect "clean report passes"                                   0 $? "0 unexplained" "$GATE_OUT"
gate two-survivors allow-empty.txt
expect "unexplained survivors fail"                            1 $? "UNEXPLAINED SURVIVOR" "$GATE_OUT"
# Two assertions about ONE run, so both read the saved GATE_RC rather than $?,
# which the first expect would otherwise have overwritten.
gate two-survivors allow-one.txt
expect "the reviewed survivor is excused"                      1 "$GATE_RC" "allowed (reviewed)" "$GATE_OUT"

# THE regression test. Both survivors sit at src/core/duration.cpp:51:9; one
# reviewed entry names cxx_ge_to_gt. The previous parser read every mutator on
# such a line as the word "Replaced", so both collapsed to one key and this
# single entry excused them both.
expect "a second operator at the SAME location is not excused" 1 "$GATE_RC" \
       "UNEXPLAINED SURVIVOR: src/core/duration.cpp:51:9:cxx_ge_to_lt" "$GATE_OUT"

gate clean allow-one.txt
expect "stale allowlist entry fails"                           1 $? "STALE ALLOWLIST ENTRY" "$GATE_OUT"

# Evidence the gate must refuse to interpret.
gate malformed allow-empty.txt
expect "unrecognisable report FAILS (no completion marker)"    1 $? "no Mull summary line" "$GATE_OUT"
gate empty allow-empty.txt
expect "empty report FAILS rather than reading as a clean run" 1 $? "no Mull summary line" "$GATE_OUT"
gate unparsed-record allow-empty.txt
expect "mutant line the parser cannot read FAILS"              1 $? "did not parse" "$GATE_OUT"
gate header-count-mismatch allow-empty.txt
expect "header count that disagrees with the list FAILS"       1 $? "but lists" "$GATE_OUT"
gate totals-disagree allow-empty.txt
expect "headers disagreeing on the total FAILS"                1 $? "disagree about how many" "$GATE_OUT"
gate score-mismatch allow-empty.txt
expect "score that does not follow from the counts FAILS"      1 $? "describe different runs" "$GATE_OUT"
gate all-killed-with-survivor allow-empty.txt
expect "'all killed' alongside a survivor FAILS"               1 $? "all mutations were killed while listing" "$GATE_OUT"
gate zero-mutants allow-empty.txt
expect "a run that generated no mutants FAILS"                 1 $? "mutated nothing measures nothing" "$GATE_OUT"

# The two renderings must agree, which is the point of reading both.
gate sarif-disagrees allow-empty.txt
expect "IDE and SARIF naming different operators FAILS"        1 $? "disagree about 'Survived'" "$GATE_OUT"
gate sarif-malformed allow-empty.txt
expect "unparseable SARIF FAILS"                               1 $? "does not parse as JSON" "$GATE_OUT"
gate missing-sarif allow-empty.txt
expect "a missing SARIF rendering FAILS"                       1 $? "does not exist" "$GATE_OUT"

# A mutant on an unexecuted line contradicts the 100% coverage gate, measured by
# a different mechanism. Never allowlistable.
gate not-covered allow-empty.txt
expect "a not-covered mutant FAILS and is not allowlistable"   1 $? "NOT-COVERED MUTANT" "$GATE_OUT"

out="$(python3 "$GATE" --report-dir "$FIX/mull/no-such-run" --allowlist "$FIX/allow-empty.txt" 2>&1)"
expect "missing report directory FAILS" 1 $? "does not exist" "$out"

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
ex="$(mktemp -d)"; mkdir -p "$ex/src"
excl() { # reviewed-file contents
  printf '%s' "${1-}" > "$ex/reviewed.txt"
  out="$("$ROOT/tools/check-exclusions.sh" "$ex" src "$ex/reviewed.txt" 2>&1)"; return $?
}

printf 'int f(int x) { return x; }\n' > "$ex/src/a.cpp"
excl ""; expect "empty tree passes"                          0 $? "0 marker(s)" "$out"

printf 'void f() { abort(); }  // GCOVR_EXCL_LINE LF-COV-001 abort() does not return\n' \
  > "$ex/src/a.cpp"
excl ""
expect "unreviewed exclusion FAILS"                          1 $? "UNREVIEWED EXCLUSION" "$out"

excl 'LF-COV-001 src/a.cpp GCOVR_EXCL_LINE -- abort() does not return'
expect "reviewed exclusion passes"                           0 $? "reviewed: LF-COV-001" "$out"

excl 'LF-COV-002 src/a.cpp GCOVR_EXCL_LINE -- abort() does not return'
expect "an ID the source does not carry is stale"            1 $? "STALE EXCLUSION ENTRY" "$out"

excl 'LF-COV-001 src/other.cpp GCOVR_EXCL_LINE -- abort() does not return'
expect "an exclusion that moved needs re-review"             1 $? "EXCLUSION MOVED" "$out"

excl 'src/a.cpp GCOVR_EXCL_LINE -- no identifier at all'
expect "a reviewed record with no ID is rejected"            1 $? "MALFORMED REVIEWED ENTRY" "$out"

printf 'void f() { abort(); }  // GCOVR_EXCL_LINE LF-COV-001\n' > "$ex/src/a.cpp"
excl 'LF-COV-001 src/a.cpp GCOVR_EXCL_LINE -- abort() does not return'
expect "exclusion with no written reason FAILS"              1 $? "NO WRITTEN REASON" "$out"

printf 'void f() { abort(); }  // GCOVR_EXCL_LINE it just cannot happen\n' > "$ex/src/a.cpp"
excl ""
expect "exclusion with no ID FAILS"                          1 $? "NO EXCLUSION ID" "$out"

# The hole the ID scheme closes: two exclusions, one file, same marker, same
# wording. Under the previous reason-keyed scheme they generated one key and a
# single reviewed record satisfied both.
{ printf 'void f() { abort(); }  // GCOVR_EXCL_LINE LF-COV-001 abort() does not return\n'
  printf 'void g() { abort(); }  // GCOVR_EXCL_LINE LF-COV-001 abort() does not return\n'
} > "$ex/src/a.cpp"
excl 'LF-COV-001 src/a.cpp GCOVR_EXCL_LINE -- abort() does not return'
expect "one ID cannot cover two exclusions"                  1 $? "DUPLICATE EXCLUSION ID" "$out"

# --- region sequencing, not just counting ------------------------------------
printf '// GCOVR_EXCL_START LF-COV-001 needs hardware CI does not have\nvoid f() { }\n' \
  > "$ex/src/a.cpp"
excl 'LF-COV-001 src/a.cpp GCOVR_EXCL_START -- needs hardware CI does not have'
expect "region left open to end of file FAILS"               1 $? "UNCLOSED EXCLUSION REGION" "$out"

printf '// GCOVR_EXCL_STOP\nvoid f() { }\n// GCOVR_EXCL_START LF-COV-001 needs absent hardware\n' \
  > "$ex/src/a.cpp"
excl 'LF-COV-001 src/a.cpp GCOVR_EXCL_START -- needs absent hardware'
expect "STOP before START FAILS despite balanced counts"     1 $? "EXCLUSION STOP WITH NO START" "$out"

{ printf '// GCOVR_EXCL_START LF-COV-001 needs absent hardware\n'
  printf '// GCOVR_EXCL_START LF-COV-002 also needs absent hardware\n'
  printf '// GCOVR_EXCL_STOP\n// GCOVR_EXCL_STOP\n'
} > "$ex/src/a.cpp"
excl 'LF-COV-001 src/a.cpp GCOVR_EXCL_START -- needs absent hardware
LF-COV-002 src/a.cpp GCOVR_EXCL_START -- also needs absent hardware'
expect "nested regions FAIL"                                 1 $? "NESTED EXCLUSION REGION" "$out"

{ printf '// GCOVR_EXCL_START LF-COV-001 needs hardware CI does not have\n'
  printf 'void f() { }\n// GCOVR_EXCL_STOP\n'
} > "$ex/src/a.cpp"
excl 'LF-COV-001 src/a.cpp GCOVR_EXCL_START -- needs hardware CI does not have'
expect "balanced region with a reviewed reason passes"       0 $? "reviewed: LF-COV-001" "$out"

rm -rf "$ex"

echo "check-fetch-centralization.sh"
out="$("$ROOT/tools/check-fetch-centralization.sh" 2>&1)"
expect "the real tree centralises every fetch" 0 $? "0 violation(s)" "$out"

fz="$(mktemp -d)"; mkdir -p "$fz/cmake" "$fz/src"
printf 'project(x)\n' > "$fz/CMakeLists.txt"
out="$("$ROOT/tools/check-fetch-centralization.sh" "$fz" 2>&1)"
expect "a tree with no fetches passes" 0 $? "0 violation(s)" "$out"

printf 'FetchContent_Declare(sneaky GIT_REPOSITORY https://example.invalid/x.git)\n' \
  >> "$fz/CMakeLists.txt"
out="$("$ROOT/tools/check-fetch-centralization.sh" "$fz" 2>&1)"
expect "a fetch in CMakeLists.txt FAILS" 1 $? "EXTERNAL FETCH OUTSIDE" "$out"

printf 'project(x)\n' > "$fz/CMakeLists.txt"
printf 'ExternalProject_Add(sneaky URL https://example.invalid/x.tar.gz)\n' > "$fz/cmake/Other.cmake"
out="$("$ROOT/tools/check-fetch-centralization.sh" "$fz" 2>&1)"
expect "a fetch in an undeclared module FAILS" 1 $? "EXTERNAL FETCH OUTSIDE" "$out"

rm -f "$fz/cmake/Other.cmake"
printf 'FetchContent_Declare(gtest GIT_REPOSITORY https://example.invalid/x.git)\n' \
  > "$fz/cmake/Dependencies.cmake"
out="$("$ROOT/tools/check-fetch-centralization.sh" "$fz" 2>&1)"
expect "the same fetch in the reviewed module passes" 0 $? "0 violation(s)" "$out"
rm -rf "$fz"

echo; echo "  tooling tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
