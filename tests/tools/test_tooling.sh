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

# --- the vendored path, which had never been exercised -----------------------
# Every component so far has been kind = "fetched" and test-only. The first
# vendored, linked component arrives with config/ at M1, and per F20 a gate
# untested on a path is a gate that will fail open on it -- so the path is
# tested here before anything travels down it.
# Both components, because a manifest declaring only the vendored one leaves
# GoogleTest undeclared while Dependencies.cmake still fetches it -- which the
# reverse cross-check correctly rejects, for reasons unrelated to vendoring.
vendored_manifest() {
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

[[component]]
name        = "examplelib"
version     = "v1.0.0"
sha         = "$GTEST_SHA"
source      = "https://example.invalid/examplelib.git"
licence     = "MIT"
kind        = "vendored"
linked      = true
files_sha256 = "$1"
EOF
}

mkdir -p "$tmp/third_party/examplelib"
printf '#pragma once\n' > "$tmp/third_party/examplelib/examplelib.hpp"
vendored_manifest "0000000000000000000000000000000000000000000000000000000000000000"
deps; expect "vendored tree that does not match its digest FAILS" 1 $? "has changed since it was reviewed" "$out"

# The failure message carries the digest, so recording it needs no second tool.
digest="$(grep -oE 'tree now hashes to [0-9a-f]{64}' <<< "$out" | grep -oE '[0-9a-f]{64}')"
vendored_manifest "$digest"
deps; expect "vendored tree matching its reviewed digest passes"  0 $? "matches its reviewed digest" "$out"

printf '#pragma once\nint sneaky();\n' > "$tmp/third_party/examplelib/examplelib.hpp"
deps; expect "an edit to vendored code FAILS afterwards"          1 $? "has changed since it was reviewed" "$out"

printf '#pragma once\n' > "$tmp/third_party/examplelib/examplelib.hpp"
vendored_manifest "$digest"
sed -i '/^files_sha256/d' "$tmp/third_party/MANIFEST.toml"
deps; expect "vendored component with no digest FAILS"            1 $? "must record 'files_sha256'" "$out"

vendored_manifest "$digest"
rm -rf "$tmp/third_party/examplelib"
deps; expect "vendored component whose directory is gone FAILS"   1 $? "ships no code" "$out"

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

echo "check-test-obligations.py"
out="$("$ROOT/tools/check-test-obligations.py" 2>&1)"
expect "the real ledger matches the real tree" 0 $? "module(s), ok" "$out"

ob="$(mktemp -d)"; mkdir -p "$ob/src/core" "$ob/tests/unit" "$ob/tests/fixtures"
printf 'int f();\n' > "$ob/src/core/core.hpp"
printf 'int main(){}\n' > "$ob/tests/unit/core_test.cpp"
led() { printf '%s\n' "${1-}" > "$ob/tests/obligations.toml"
        out="$("$ROOT/tools/check-test-obligations.py" "$ob" 2>&1)"; return $?; }

led '[[module]]
path    = "src/core"
classes = ["P1", "P3"]
tiers   = ["T1"]'
expect "a ledger matching the tree passes"                   0 $? "ok    src/core" "$out"

led '# nothing declared'
expect "an empty ledger beside real source FAILS"            1 $? "not a clean sheet" "$out"

led '[[module]
path = broken'
expect "an unparseable ledger FAILS"                         1 $? "does not parse as TOML" "$out"

mkdir -p "$ob/src/other"; printf 'int g();\n' > "$ob/src/other/other.hpp"
led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]'
expect "a module absent from the ledger FAILS"               1 $? "absent from obligations.toml" "$out"
rm -rf "$ob/src/other"

led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]

[[module]]
path    = "src/deleted"
classes = ["P1"]
tiers   = ["T1"]'
expect "a ledger entry outliving its module FAILS"           1 $? "holds no source" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1", "P9"]
tiers   = ["T1"]'
expect "an invented path class FAILS"                        1 $? "is not a path class" "$out"

led '[[module]]
path    = "src/core"
classes = ["P3"]
tiers   = ["T1"]'
expect "an entry without P1 FAILS as unclassified"           1 $? "has not been classified" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T9"]'
expect "declaring a universal tier FAILS"                    1 $? "not a declarable tier" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T7"]'
expect "a promised tier with no tests FAILS"                 1 $? "a promise, not a test" "$out"

# P2 and P7 are the classes that cannot be tested without something to force the
# state, so the ledger insists the fixtures are named when they are declared.
led '[[module]]
path    = "src/core"
classes = ["P1", "P2"]
tiers   = ["T1"]'
expect "error paths declared with no fixtures FAILS"         1 $? "needs something to force" "$out"

led '[[module]]
path     = "src/core"
classes  = ["P1", "P7"]
tiers    = ["T1"]
fixtures = ["sysfs/no-such-tree"]'
expect "a fixture that does not exist FAILS"                 1 $? "does not exist" "$out"

mkdir -p "$ob/tests/fixtures/sysfs/intel"
led '[[module]]
path     = "src/core"
classes  = ["P1", "P7"]
tiers    = ["T1"]
fixtures = ["sysfs/intel"]'
expect "declared classes with real fixtures passes"          0 $? "ok    src/core" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]
tears   = ["typo"]'
expect "a misspelt field FAILS rather than being ignored"    1 $? "unknown field" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]

[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]'
expect "a module declared twice FAILS"                       1 $? "declared twice" "$out"

rm -rf "$ob"

echo "check-dco.sh"
# Real git repositories, because the bug this gate had was entirely about what
# actions/checkout leaves in the working tree on a pull_request event -- which no
# amount of reasoning about the YAML would have revealed.
dco="$(mktemp -d)"
git init -q "$dco"
git -C "$dco" config user.name "Test Contributor"
git -C "$dco" config user.email "contributor@example.invalid"
git -C "$dco" config commit.gpgsign false
dco_commit() { # message, [--no-signoff]
  echo "$RANDOM" > "$dco/file.txt"
  git -C "$dco" add -A
  if [ "${2-}" = "--no-signoff" ]; then
    git -C "$dco" commit -q -m "$1"
  else
    git -C "$dco" commit -q -s -m "$1"
  fi
}

dco_commit "base commit"
base="$(git -C "$dco" rev-parse HEAD)"
git -C "$dco" checkout -q -b feature
dco_commit "signed feature work"
head="$(git -C "$dco" rev-parse HEAD)"

out="$("$ROOT/tools/check-dco.sh" "$base" "$head" "$dco" 2>&1)"
expect "a signed-off series passes"                          0 $? "1 commit(s) checked, 0 unsigned" "$out"

dco_commit "unsigned feature work" --no-signoff
head="$(git -C "$dco" rev-parse HEAD)"
out="$("$ROOT/tools/check-dco.sh" "$base" "$head" "$dco" 2>&1)"
expect "an unsigned commit FAILS"                            1 $? "MISSING Signed-off-by" "$out"

# THE regression test. On a pull_request event GitHub checks out refs/pull/N/merge
# -- a synthetic merge of head into base, authored by GitHub, with no trailers.
# Walking base..HEAD from there flagged GitHub's own commit, so the gate failed
# every pull request no matter how carefully its commits were signed.
git -C "$dco" checkout -q -b pr-merge "$base"
git -C "$dco" merge -q --no-ff --no-verify -m "Merge feature into main" feature 2>/dev/null
merge_head="$(git -C "$dco" rev-parse HEAD)"
git -C "$dco" log -1 --format='%(trailers:key=Signed-off-by)' "$merge_head" | grep -q . \
  && echo "  (fixture problem: the merge commit is signed, which defeats this test)"

out="$("$ROOT/tools/check-dco.sh" "$base" "$merge_head" "$dco" 2>&1)"; dco_rc=$?
# It fails, but for the right reason: the real unsigned commit, not the merge.
expect "the unsigned commit behind a merge is flagged"       1 "$dco_rc" "MISSING Signed-off-by: .*unsigned feature work" "$out"
if grep -q "MISSING Signed-off-by: .*Merge feature into main" <<< "$out"; then
  echo "  FAIL  GitHub's synthetic merge commit must not be flagged"; fail=$((fail + 1))
else
  echo "  PASS  GitHub's synthetic merge commit is not flagged"; pass=$((pass + 1))
fi

# Same shape, with the unsigned commit removed: the merge must not fail it.
git -C "$dco" checkout -q -B feature "$base"
dco_commit "signed work only"
git -C "$dco" checkout -q -B pr-merge "$base"
git -C "$dco" merge -q --no-ff --no-verify -m "Merge feature into main" feature 2>/dev/null
merge_head="$(git -C "$dco" rev-parse HEAD)"
out="$("$ROOT/tools/check-dco.sh" "$base" "$merge_head" "$dco" 2>&1)"
expect "a signed series behind a merge commit passes"        0 $? "0 unsigned" "$out"

out="$("$ROOT/tools/check-dco.sh" "$base" "$base" "$dco" 2>&1)"
expect "an empty range FAILS rather than passing vacuously"  1 $? "no commits in" "$out"

out="$("$ROOT/tools/check-dco.sh" "does-not-exist" "$head" "$dco" 2>&1)"
expect "an unresolvable ref FAILS"                           1 $? "is not a commit" "$out"

# A body that merely mentions the words is not a trailer.
git -C "$dco" checkout -q -B feature "$base"
echo change > "$dco/file.txt"; git -C "$dco" add -A
git -C "$dco" commit -q -m "mentions Signed-off-by: Someone <a@b.c> in prose

but has no trailer block because this line follows it."
out="$("$ROOT/tools/check-dco.sh" "$base" "$(git -C "$dco" rev-parse HEAD)" "$dco" 2>&1)"
expect "a sign-off mentioned in prose does not count"        1 $? "MISSING Signed-off-by" "$out"

rm -rf "$dco"

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
