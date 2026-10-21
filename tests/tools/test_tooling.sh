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
  if [ "$2" -eq "$3" ] && { [ $# -lt 5 ] || grep -q -- "$4" <<< "$5"; }; then
    echo "  PASS  $1"; pass=$((pass + 1))
  else
    echo "  FAIL  $1 (expected exit $2, got $3)"; fail=$((fail + 1))
    [ $# -ge 5 ] && echo "$5" | sed 's/^/          /'
  fi
}

gate() { # report-fixture-dir, allowlist-fixture, [more gate arguments]
  local out; out="$(python3 "$GATE" --report-dir "$FIX/mull/$1" \
                    --allowlist "$FIX/$2" --root "$ROOT" "${@:3}" 2>&1)"; local rc=$?
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

# Captured from a real Mull 0.34.2 run on GitHub's runners, byte for byte (F21).
# Its SARIF writes each location as the absolute path minus its leading slash,
# relative to a base of file://./, and the hand-written fixtures above never
# showed that shape: the first real run of the gate disagreed with itself.
real_gate() { # fixture-dir, allowlist-file
  GATE_OUT="$(python3 "$GATE" --report-dir "$FIX/mull/$1" --allowlist "$2" \
              --root /tmp/tmp.TsJpoi7A3p 2>&1)"; GATE_RC=$?; return $GATE_RC
}
real_gate real-0.34.2 "$FIX/allow-empty.txt"
expect "real 0.34.2 reports agree once SARIF paths are resolved" 1 $? "UNEXPLAINED SURVIVOR: subject.cpp:2:13:cxx_ge_to_gt" "$GATE_OUT"
real_allow="$(mktemp)"
printf '# Reviewed for the test.\nsubject.cpp:2:13:cxx_ge_to_gt\n' > "$real_allow"
real_gate real-0.34.2 "$real_allow"
expect "the resolved key is the one an allowlist names"          0 $? "1 allowed" "$GATE_OUT"
rm -f "$real_allow"
real_gate sarif-undefined-base "$FIX/allow-empty.txt"
expect "a location relative to an undefined base FAILS"          1 $? "never defines" "$GATE_OUT"
real_gate sarif-remote-base "$FIX/allow-empty.txt"
expect "a base that is not a local file FAILS"                   1 $? "not a local file URI" "$GATE_OUT"

# A mutant on an unexecuted line contradicts the 100% coverage gate, measured by
# a different mechanism. Never allowlistable.
gate not-covered allow-empty.txt
expect "a not-covered mutant FAILS and is not allowlistable"   1 $? "NOT-COVERED MUTANT" "$GATE_OUT"

out="$(python3 "$GATE" --report-dir "$FIX/mull/no-such-run" --allowlist "$FIX/allow-empty.txt" 2>&1)"
expect "missing report directory FAILS" 1 $? "does not exist" "$out"

gate clean allow-malformed.txt
expect "an allowlist line that is not a key FAILS"             1 $? "is not an allowlist key" "$GATE_OUT"

# One shard at a time (--scope). The sweep runs one CI job per module, each
# mutating only its own sources; the gate must judge each of them alone without
# giving up anything it refuses for the whole.
# Several test executables, one run each (M2a exit criterion 6): the same
# mutants are compiled into every executable that links their code, and a
# mutant is killed when ANY executable's tests kill it.
echo "mutation-gate.py (several executables)"
merged() { # allowlist-fixture, report-fixture-dirs...
  local allow="$1"; shift; local reports=()
  for dir in "$@"; do reports+=(--report-dir "$FIX/mull/$dir"); done
  GATE_OUT="$(python3 "$GATE" "${reports[@]}" --allowlist "$FIX/$allow" --root "$ROOT" 2>&1)"
  GATE_RC=$?; return $GATE_RC
}
merged allow-empty.txt merge-a
expect "one executable alone leaves two survivors"             1 $? "2 unexplained" "$GATE_OUT"
merged allow-empty.txt merge-a merge-b
expect "a second executable's kill removes a survivor"         1 $? "1 unexplained" "$GATE_OUT"
expect "and the one left is the one nobody killed"             1 "$GATE_RC" \
       "UNEXPLAINED SURVIVOR: src/core/duration.cpp:51:9:cxx_ge_to_lt" "$GATE_OUT"
merged allow-empty.txt merge-a merge-b merge-c
expect "three executables between them kill every mutant"      0 $? "0 unexplained" "$GATE_OUT"
merged allow-empty.txt merge-c merge-b merge-a
expect "and the order they ran in does not matter"             0 $? "0 unexplained" "$GATE_OUT"
merged allow-empty.txt not-covered not-covered
expect "a mutant no executable ran stays not-covered"          1 $? "NOT-COVERED MUTANT" "$GATE_OUT"
merged allow-empty.txt merge-a malformed
expect "an executable whose report cannot be read fails all"   1 $? "no Mull summary line" "$GATE_OUT"

echo "mutation-gate.py --scope"
gate clean allow-empty.txt --scope src/core
expect "a shard whose mutants all lie in scope passes"         0 $? "2 mutant(s), all in scope" "$GATE_OUT"
gate two-modules allow-one.txt --scope src/core
expect "a mutant outside the shard FAILS, even a killed one"   1 $? \
       "MUTANT OUTSIDE THE SHARD: src/platform/fs.cpp:88:13:cxx_lt_to_ge" "$GATE_OUT"
gate two-modules allow-one.txt --scope src/core --scope src/platform/
expect "a shard of several paths holds all of them"            0 $? "3 mutant(s), all in scope" "$GATE_OUT"
gate clean allow-empty.txt --scope src/co
expect "a scope is whole path components, not a string prefix" 1 $? "outside this shard" "$GATE_OUT"
gate killed-unlisted allow-one.txt
expect "(a report that hides its kills is judged without a scope)" 0 $? "1 allowed" "$GATE_OUT"
gate killed-unlisted allow-one.txt --scope src/core
expect "...but FAILS under one: hidden kills could lie anywhere" 1 $? "does not list its killed" "$GATE_OUT"
gate all-killed-unlisted allow-empty.txt --scope src/core
expect "'all killed' with nothing listed FAILS under a scope"  1 $? "does not list its killed" "$GATE_OUT"
for bad in /src/core ../src src/./core "" 'src\core'; do
  gate clean allow-empty.txt --scope "$bad"
  expect "scope '$bad' is refused, not guessed at"             1 $? "not a path below" "$GATE_OUT"
done

# Allowlist entries for other shards are neither applied nor stale here; the
# same entry is stale for the whole run, which is the case it must not hide.
gate clean allow-elsewhere.txt --scope src/core
expect "another shard's entry is neither applied nor stale"    0 $? "1 allowlist entry for other shards" "$GATE_OUT"
gate clean allow-elsewhere.txt
expect "(the same entry is stale when the whole run is judged)" 1 $? "STALE ALLOWLIST ENTRY" "$GATE_OUT"
gate clean allow-one.txt --scope src/core
expect "an in-scope stale entry still FAILS in a shard"        1 $? "STALE ALLOWLIST ENTRY" "$GATE_OUT"
gate two-survivors allow-one.txt --scope src/core
expect "an in-scope reviewed survivor is excused in a shard"   1 "$GATE_RC" "allowed (reviewed)" "$GATE_OUT"
expect "and the other operator at that location is not"        1 "$GATE_RC" \
       "UNEXPLAINED SURVIVOR: src/core/duration.cpp:51:9:cxx_ge_to_lt" "$GATE_OUT"
gate not-covered allow-empty.txt --scope src/core
expect "a not-covered mutant still FAILS in a shard"           1 $? "NOT-COVERED MUTANT" "$GATE_OUT"

# Every refusal of the whole-run gate holds within a shard: the scope is checked
# only after the report has been read, reconciled and cross-checked.
for case in "malformed:no Mull summary line" "empty:no Mull summary line" \
            "unparsed-record:did not parse" "header-count-mismatch:but lists" \
            "totals-disagree:disagree about how many" "score-mismatch:describe different runs" \
            "all-killed-with-survivor:all mutations were killed while listing" \
            "zero-mutants:mutated nothing measures nothing" \
            "sarif-disagrees:disagree about 'Survived'" "sarif-malformed:does not parse as JSON" \
            "missing-sarif:does not exist"; do
  gate "${case%%:*}" allow-empty.txt --scope src/core
  expect "in a shard, ${case%%:*} still FAILS"                  1 $? "${case#*:}" "$GATE_OUT"
done

# How the gate runs Mull, against a stand-in runner that records its arguments
# and returns a fixture: the timeouts, no capture of mutant output, and the test
# program's arguments after "--", where Mull takes nothing as its own option.
# The real runner's reading of the same command is tested end to end in
# tests/tools/mutation_integration.sh.
run_dir="$(mktemp -d)"
cat > "$run_dir/fake-runner" <<'EOF'
#!/bin/bash
printf '%s\n' "$@" > "$(dirname "$0")/args"
dir=""
while [ $# -gt 0 ]; do [ "$1" = "--report-dir" ] && dir="$2"; shift; done
cp "$FAKE_REPORTS/mutation.txt" "$FAKE_REPORTS/mutation.sarif" "$dir/"
exit "${FAKE_EXIT:-0}"
EOF
chmod +x "$run_dir/fake-runner"
out="$(FAKE_REPORTS="$FIX/mull/clean" python3 "$GATE" ./subject --runner "$run_dir/fake-runner" \
       --allowlist "$FIX/allow-empty.txt" --root "$run_dir" --scope src/core \
       --test-arg=--gtest_filter=-RealProcess* 2>&1)"
expect "the gate runs Mull and judges the shard it reports"    0 $? "2 mutant(s), all in scope" "$out"
args="$(tr '\n' ' ' < "$run_dir/args")"
expect "Mull's warm-up and baseline are bounded"               0 0 "--timeout 120000 " "$args"
expect "a hung mutant is killed at the 20 s floor"             0 0 "--minimum-timeout 20000 " "$args"
expect "a mutant's output is not captured"                     0 0 "--no-mutant-output " "$args"
expect "the test filter follows the program, after '--'"       0 0 \
       " ./subject -- --gtest_filter=-RealProcess\*  *$" "$args"
out="$(FAKE_EXIT=1 FAKE_REPORTS="$FIX/mull/clean" python3 "$GATE" ./subject \
       --runner "$run_dir/fake-runner" --allowlist "$FIX/allow-empty.txt" --root "$run_dir" 2>&1)"
expect "a runner that fails FAILS the gate, report or not"     1 $? "genuine failure to run" "$out"
rm -rf "$run_dir"

echo "mutation-shard.py"
SHARD="$ROOT/tools/mutation-shard.py"
out="$(python3 "$SHARD" plan 2>&1)"
expect "the real tree's plan is made"                          0 $? '"platform"' "$out"
expect "and every allowlist entry is in a shard"               0 0 "each in a shard" "$out"
out="$(python3 "$SHARD" gtest-filter 2>&1)"
expect "every mutant runs the tests that create no process"    0 $? "^-RealProcess\*$" "$out"

# A throwaway tree: two modules with sources, one with headers only.
tree="$(mktemp -d)"
mkdir -p "$tree/src/core" "$tree/src/alpha/inner" "$tree/src/contract" "$tree/tools"
touch "$tree/src/core/a.cpp" "$tree/src/alpha/inner/b.cpp" "$tree/src/contract/c.hpp"
: > "$tree/tools/mutation-allowlist.txt"
out="$(python3 "$SHARD" --root "$tree" plan 2>/dev/null)"
expect "a module with a source file is a shard of its own"     0 $? '^\["alpha", "core"\]$' "$out"
out="$(python3 "$SHARD" --root "$tree" scope core 2>&1)"
expect "a headers-only module is swept in the core shard"      0 $? "src/contract" "$out"
out="$(python3 "$SHARD" --root "$tree" scope nothing 2>&1)"
expect "an unknown shard FAILS"                                1 $? "no shard named" "$out"
printf '# Reviewed.\nsrc/contract/c.hpp:3:7:cxx_add_to_sub\n' > "$tree/tools/mutation-allowlist.txt"
out="$(python3 "$SHARD" --root "$tree" plan 2>&1)"
expect "an entry under a headers-only module has its shard"    0 $? "1 allowlist entry, each in a shard" "$out"
printf '# Reviewed.\ntests/unit/x_test.cpp:3:7:cxx_add_to_sub\n' > "$tree/tools/mutation-allowlist.txt"
out="$(python3 "$SHARD" --root "$tree" plan 2>&1)"
expect "an entry no shard owns FAILS the plan"                 1 $? "lie in no shard" "$out"
printf 'not a key\n' > "$tree/tools/mutation-allowlist.txt"
out="$(python3 "$SHARD" --root "$tree" plan 2>&1)"
expect "a malformed entry FAILS the plan"                      1 $? "is not an allowlist key" "$out"
: > "$tree/tools/mutation-allowlist.txt"
out="$(python3 "$SHARD" --root "$tree" mull-config alpha 2>&1)"
expect "a shard's mull.yml names Mull's default operators"     0 $? '"cxx_default"' "$out"
# The include path, read back by a YAML parser's rules for a JSON string and
# matched the way Mull matches it: unanchored, against the absolute path.
regex="$(sed -n 's/^  - "\(\^.*\)"$/\1/p' <<< "$out" | sed 's/\\\\/\\/g')"
matches() { python3 -c 'import re,sys; sys.exit(0 if re.search(sys.argv[1], sys.argv[2]) else 1)' "$regex" "$1"; }
matches "$tree/src/alpha/inner/b.cpp"
expect "the include path matches the shard's own sources"      0 $?
matches "$tree/src/alphabet/b.cpp"
expect "and not a module whose name extends it"                1 $?
matches "/elsewhere$tree/src/alpha/b.cpp"
expect "nor the same path under another root"                  1 $?
mkdir -p "$tree/odd root+(x)[1]/src/core" "$tree/odd root+(x)[1]/tools"
touch "$tree/odd root+(x)[1]/src/core/a.cpp"
out="$(python3 "$SHARD" --root "$tree/odd root+(x)[1]" mull-config core 2>&1)"
regex="$(sed -n 's/^  - "\(\^.*\)"$/\1/p' <<< "$out" | sed 's/\\\\/\\/g')"
matches "$tree/odd root+(x)[1]/src/core/a.cpp"
expect "a checkout path full of regex syntax is escaped"       0 $?
matches "$tree/odd roott(x)1/src/core/a.cpp"
expect "and matches nothing it merely resembles"               1 $?
rm -rf "$tree/src/core"
out="$(python3 "$SHARD" --root "$tree" plan 2>&1)"
expect "a plan whose header host has no source FAILS"          1 $? "choose another HEADER_ONLY_HOST" "$out"
rm -rf "$tree/src"/*
out="$(python3 "$SHARD" --root "$tree" plan 2>&1)"
expect "an src/ with no module FAILS"                          1 $? "holds no module" "$out"
rm -rf "$tree"

# The test executables of a shard: where its tests live decides.
out="$(python3 "$SHARD" targets verification 2>&1)"
expect "a module with suites of its own names them"             0 $? "loadforge_unit_verification_checksum_tests" "$out"
out="$(python3 "$SHARD" targets resources 2>&1)"
expect "a module whose tests join the unit binary names it"     0 $? "^loadforge_unit_tests$" "$out"
out="$(python3 "$SHARD" targets worker 2>&1)"
expect "and the unit binary is named once with the suites too" 0 $? "loadforge_unit_worker_executor_tests" "$out"
leftover="$(grep -c 'supervision\|integration\|soak' <<< "$out")"
expect "the process-starting and the soak binaries are left out" 0 "$leftover" ""
tree="$(mktemp -d)"
mkdir -p "$tree/src/core" "$tree/src/alpha" "$tree/tests/unit/alpha" "$tree/tests/unit/core" "$tree/tests/supervision" "$tree/tools"
touch "$tree/src/core/a.cpp" "$tree/src/alpha/b.cpp" "$tree/tools/mutation-allowlist.txt"
printf 'loadforge_test_suite(loadforge_unit_alpha_tests SOURCES a.cpp)\nloadforge_test_suite(\n  loadforge_unit_alpha_more_tests\n  SOURCES b.cpp)\n' > "$tree/tests/unit/alpha/CMakeLists.txt"
printf 'target_sources(loadforge_unit_tests PRIVATE x.cpp)\n' > "$tree/tests/unit/core/CMakeLists.txt"
printf 'loadforge_test_suite(loadforge_supervision_x_tests SOURCES a.cpp)\n' > "$tree/tests/supervision/CMakeLists.txt"
out="$(python3 "$SHARD" --root "$tree" targets alpha 2>&1)"
expect "every suite a module's fragment defines is named"       0 $? "loadforge_unit_alpha_more_tests" "$out"
out="$(python3 "$SHARD" --root "$tree" targets core 2>&1)"
expect "target_sources joins the unit-test binary"              0 $? "^loadforge_unit_tests$" "$out"
printf 'add_executable(loadforge_unit_tests\n  unit/alpha/y_test.cpp\n)\n' > "$tree/tests/CMakeLists.txt"
rm "$tree/tests/unit/alpha/CMakeLists.txt"
out="$(python3 "$SHARD" --root "$tree" targets alpha 2>&1)"
expect "so does a listed source under tests/unit/<module>/"     0 $? "^loadforge_unit_tests$" "$out"
rm "$tree/tests/CMakeLists.txt"
out="$(python3 "$SHARD" --root "$tree" targets alpha 2>&1)"
expect "a shard no executable tests FAILS rather than sweeping nothing" 1 $? "run against nothing" "$out"
rm -rf "$tree"

# The test executables of a unit of the CI plan: a path, less what other units
# sweep. A fragment below a module is named by its own path, and the module
# around it leaves its tests to it.
tree="$(mktemp -d)"
mkdir -p "$tree/src/alpha/inner" "$tree/tests/unit/alpha/inner" "$tree/tests/property/alpha/inner"
touch "$tree/src/alpha/a.cpp" "$tree/src/alpha/inner/b.cpp"
printf 'loadforge_test_suite(loadforge_unit_alpha_inner_tests SOURCES b.cpp)\n' > "$tree/tests/unit/alpha/inner/CMakeLists.txt"
printf 'loadforge_test_suite(loadforge_property_alpha_inner_tests SOURCES b.cpp)\n' > "$tree/tests/property/alpha/inner/CMakeLists.txt"
printf 'add_executable(loadforge_unit_tests\n  unit/alpha/a_test.cpp\n)\n' > "$tree/tests/CMakeLists.txt"
out="$(python3 "$SHARD" --root "$tree" unit-targets --scope src/alpha/inner 2>&1)"
expect "a fragment's unit names every suite of its own tests"   0 $? "loadforge_property_alpha_inner_tests" "$out"
leftover="$(grep -c '^loadforge_unit_tests$' <<< "$out")"
expect "and not the shared binary its module's tests are in"    0 "$leftover" ""
out="$(python3 "$SHARD" --root "$tree" unit-targets --scope src/alpha --exclude src/alpha/inner 2>&1)"
expect "the module around it names the shared binary"           0 $? "^loadforge_unit_tests$" "$out"
leftover="$(grep -c 'inner' <<< "$out")"
expect "and leaves the fragment's suites to the fragment"       0 "$leftover" ""
out="$(python3 "$SHARD" --root "$tree" unit-targets --scope src/alpha/inner --exclude src/alpha/inner 2>&1)"
expect "a unit that leaves out all it has FAILS"                1 $? "run against nothing" "$out"
rm -rf "$tree"

# check-processes, against stand-in test programs that answer like GoogleTest:
# the set every mutant runs must create no process, and a RealProcess* test must
# create one. Bash builtins create none; running /bin/true creates one.
fake_gtest() { # file, fast-set body, marked-test body, [list output]
  cat > "$1" <<EOF
#!/bin/bash
case "\$1" in
  --gtest_list_tests) printf '${4:-RealProcessA.\n  Forks\n}';;
  --gtest_filter=-RealProcess\*) $2;;
  *) $3;;
esac
EOF
  chmod +x "$1"
}
ran='echo "[==========] 3 tests from 1 test suite ran. (1 ms total)"'
procs="$(mktemp -d)"
if command -v strace >/dev/null; then
  fake_gtest "$procs/honest" "$ran" "/bin/true; $ran"
  out="$(python3 "$SHARD" check-processes "$procs/honest" 2>&1)"
  expect "an honest split passes"                              0 $? "1 RealProcess\* test(s) each create a process" "$out"
  fake_gtest "$procs/forks" "/bin/true; $ran" "/bin/true; $ran"
  out="$(python3 "$SHARD" check-processes "$procs/forks" 2>&1)"
  expect "a process in the set every mutant runs FAILS"        1 $? "created 1 process(es)" "$out"
  fake_gtest "$procs/idle" "$ran" "$ran"
  out="$(python3 "$SHARD" check-processes "$procs/idle" 2>&1)"
  expect "a RealProcess* test that creates no process FAILS"   1 $? "create no process" "$out"
  fake_gtest "$procs/skips" "$ran" "echo '[  SKIPPED ] RealProcessA.Forks'; $ran"
  out="$(python3 "$SHARD" check-processes "$procs/skips" 2>&1)"
  expect "a RealProcess* test skipped here is reported, not judged" 0 $? "skipped here, so not checked" "$out"
  fake_gtest "$procs/none" 'echo "[==========] 0 tests from 0 test suites ran. (0 ms total)"' "$ran"
  out="$(python3 "$SHARD" check-processes "$procs/none" 2>&1)"
  expect "a filter that runs no test FAILS"                    1 $? "ran no test" "$out"
  fake_gtest "$procs/mute" "echo hello" "$ran"
  out="$(python3 "$SHARD" check-processes "$procs/mute" 2>&1)"
  expect "a run that does not say how many tests ran FAILS"    1 $? "did not report how many" "$out"
  fake_gtest "$procs/failing" "$ran; exit 1" "$ran"
  out="$(python3 "$SHARD" check-processes "$procs/failing" 2>&1)"
  expect "a failing suite FAILS the check"                     1 $? "exited 1 under strace" "$out"
  # exec, so that bash forks nothing itself: the only clone is the thread's.
  fake_gtest "$procs/threads" "exec python3 -c 'import threading; t = threading.Thread(target=print); t.start(); t.join(); print(\"[==========] 3 tests from 1 test suite ran.\")'" "/bin/true; $ran"
  out="$(python3 "$SHARD" check-processes "$procs/threads" 2>&1)"
  expect "a thread is not a process"                           0 $? "create no process" "$out"
  out="$(PATH="$procs/no-such-dir" "$(command -v python3)" "$SHARD" check-processes "$procs/honest" 2>&1)"
  expect "no strace FAILS rather than passing unseen"          1 $? "strace is not installed" "$out"
else
  echo "  FAIL  strace is not installed: check-processes cannot be tested"; fail=$((fail + 1))
fi
rm -rf "$procs"

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

# T8 is not a directory of test files -- its cases are add_test() calls. Listing
# tests/CMakeLists.txt as a location made the check vacuous, because the file
# always exists, so declaring T8 could never fail. A gate condition that cannot
# fail is not a gate. The marker must be INSIDE the file.
mkdir -p "$ob/tests"
printf 'project(x)\n' > "$ob/tests/CMakeLists.txt"
led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T8"]'
expect "T8 with a CMakeLists carrying no add_test FAILS"     1 $? "a promise, not a test" "$out"

printf 'add_test(NAME smoke COMMAND thing)\n' >> "$ob/tests/CMakeLists.txt"
led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T8"]'
expect "T8 with a real add_test passes"                      0 $? "ok    src/core" "$out"

# P2 and P7 are the classes that cannot be tested without something to force the
# state, so the ledger insists the fixtures are named when they are declared.
# P2 and P7 need different things, and conflating them was this gate's first
# real bug: an errno is forced through the substitutable syscall seam, not from
# a tree on disk, so demanding a fixture of src/platform/fs was wrong.
led '[[module]]
path    = "src/core"
classes = ["P1", "P2"]
tiers   = ["T1"]'
expect "error paths with no injecting tier FAILS"            1 $? "neither T2 nor T5" "$out"

# P6 had no rule at all until a module first declared it, so the declaration was
# an unenforced promise -- which is the shape this gate exists to catch. Found
# while adding the worker launcher: the ledger accepted P6 and asked for nothing.
led '[[module]]
path    = "src/core"
classes = ["P1", "P6"]
tiers   = ["T1"]'
expect "lifecycle paths with no way to force them FAILS"     1 $? "none of T2, T7 or T8" "$out"

mkdir -p "$ob/tests/unit"
led '[[module]]
path    = "src/core"
classes = ["P1", "P6"]
tiers   = ["T1", "T2"]'
expect "lifecycle paths forced through the seam pass"        0 $? "ok    src/core" "$out"

mkdir -p "$ob/tests/unit"
led '[[module]]
path    = "src/core"
classes = ["P1", "P2"]
tiers   = ["T1", "T2"]'
expect "error paths need no on-disk fixture, only a tier"    0 $? "ok    src/core" "$out"

led '[[module]]
path     = "src/core"
classes  = ["P1", "P7"]
tiers    = ["T1"]'
expect "capability paths with no fixtures FAILS"             1 $? "reachable only" "$out"

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

# Not every capability state can be committed to git. "Vanishes mid-run" is an
# event rather than a state, and a mode-000 file arrives readable after a clone
# because git records only the execute bit -- so a test that CONSTRUCTS the
# hostile tree is the only way to reach either, and the ledger has to be able to
# say so. The escape hatch is bounded: the named file must exist AND declare
# itself, or this is just "some file exists" wearing a new field name.
led '[[module]]
path             = "src/core"
classes          = ["P1", "P7"]
tiers            = ["T1"]
fixture_builders = ["tests/unit/no_such_test.cpp"]'
expect "a fixture builder that does not exist FAILS"         1 $? "does not exist" "$out"

printf 'int builder(){return 0;}\n' > "$ob/tests/unit/builder_test.cpp"
led '[[module]]
path             = "src/core"
classes          = ["P1", "P7"]
tiers            = ["T1"]
fixture_builders = ["tests/unit/builder_test.cpp"]'
expect "a builder that does not declare itself FAILS"        1 $? "does not carry" "$out"

printf '// LOADFORGE P7 FIXTURE BUILDER\n' >> "$ob/tests/unit/builder_test.cpp"
led '[[module]]
path             = "src/core"
classes          = ["P1", "P7"]
tiers            = ["T1"]
fixture_builders = ["tests/unit/builder_test.cpp"]'
expect "a builder that declares itself passes"               0 $? "ok    src/core" "$out"

led '[[module]]
path             = "src/core"
classes          = ["P1", "P7"]
tiers            = ["T1"]
fixture_builders = "tests/unit/builder_test.cpp"'
expect "fixture_builders that is not a list FAILS"           1 $? "must be a list" "$out"
rm -f "$ob/tests/unit/builder_test.cpp"

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

# The EOWNERDEAD obligation, per process-shared lock (IMPLEMENTATION §4, M1's
# second new gate). The real tree has none, and the gate must say so rather
# than say nothing; a fixture tree with an undeclared lock must be refused.
out="$("$ROOT/tools/check-test-obligations.py" 2>&1)"
expect "the real tree has no lock and says so"               0 $? "none in src/ and none declared" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]'
expect "a tree without locks owes nothing, explicitly"       0 $? "no EOWNERDEAD obligation is owed" "$out"

cat > "$ob/src/core/shared.cpp" <<'CPP'
#include <pthread.h>
void make(pthread_mutexattr_t* attributes) {
  pthread_mutexattr_setpshared(attributes, PTHREAD_PROCESS_SHARED);
  pthread_mutexattr_setrobust(attributes, PTHREAD_MUTEX_ROBUST);
}
CPP
led '[[module]]
path    = "src/core"
classes = ["P1", "P5"]
tiers   = ["T1"]'
expect "an undeclared process-shared lock FAILS"             1 $? "declares no process-shared lock" "$out"

printf 'void make() { /* sem_init */ }\n' > "$ob/src/core/shared.cpp"
led '[[module]]
path    = "src/core"
classes = ["P1", "P5"]
tiers   = ["T1"]'
expect "a primitive named only in a comment still FAILS"     1 $? "uses sem_init" "$out"

cat > "$ob/src/core/shared.cpp" <<'CPP'
#include <pthread.h>
void make(pthread_mutexattr_t* attributes) {
  pthread_mutexattr_setrobust(attributes, PTHREAD_MUTEX_ROBUST);
}
CPP
mkdir -p "$ob/tests/supervision"
lock_ledger() { # robustness-test, [classes]
  led "[[module]]
path    = \"src/core\"
classes = ${2:-[\"P1\", \"P5\"]}
tiers   = [\"T1\", \"T7\"]
process_shared_locks = [
  { name = \"region lock\", file = \"src/core/shared.cpp\", robustness_test = \"$1\" },
]"; }
lock_ledger "tests/supervision/no_such_test.cpp"
expect "a lock whose robustness test is missing FAILS"       1 $? "not a file under tests/supervision" "$out"

printf 'int dummy;\n' > "$ob/tests/unit/lock_test.cpp"
lock_ledger "tests/unit/lock_test.cpp"
expect "a robustness test outside the T7 tier FAILS"         1 $? "not a file under tests/supervision" "$out"

printf 'int robust;\n' > "$ob/tests/supervision/lock_test.cpp"
lock_ledger "tests/supervision/lock_test.cpp"
expect "a robustness test that names no lock FAILS"          1 $? "does not carry" "$out"

printf '// LOADFORGE EOWNERDEAD TEST: region lock two\n' >> "$ob/tests/supervision/lock_test.cpp"
lock_ledger "tests/supervision/lock_test.cpp"
expect "a robustness test for a different lock FAILS"        1 $? "does not carry" "$out"

printf '// LOADFORGE EOWNERDEAD TEST: region lock\n' >> "$ob/tests/supervision/lock_test.cpp"
lock_ledger "tests/supervision/lock_test.cpp" '["P1", "P6"]'
expect "a lock in a module without P5 FAILS"                 1 $? "does not declare P5" "$out"

lock_ledger "tests/supervision/lock_test.cpp"
expect "a declared lock with its robustness test passes"     0 $? "1 lock(s) declared" "$out"

printf 'void make() {}\n' > "$ob/src/core/shared.cpp"
lock_ledger "tests/supervision/lock_test.cpp"
expect "a lock declared where there is none FAILS"           1 $? "uses no process-shared lock" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1", "P5"]
tiers   = ["T1"]
process_shared_locks = [{ name = "region lock", file = "src/core/shared.cpp" }]'
expect "a lock entry missing a field FAILS"                  1 $? "needs exactly" "$out"

led '[[module]]
path    = "src/core"
classes = ["P1", "P5"]
tiers   = ["T1"]
process_shared_locks = "src/core/shared.cpp"'
expect "process_shared_locks that is not a list FAILS"       1 $? "must be a list of tables" "$out"

rm -f "$ob/src/core/shared.cpp"
mkdir -p "$ob/src/core/trap.cpp"
led '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]'
expect "a source file that cannot be read FAILS"             1 $? "cannot be read" "$out"
rm -rf "$ob/src/core/trap.cpp" "$ob/tests/supervision"

# One fragment per module (ADR 0004): streams built in parallel never edit the
# same ledger file. A fragment is named after its module and declares nothing
# else; anything in the directory that is not a fragment is refused, because a
# row the gate does not read is a row nobody checks.
core_row='[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]'
frag() { # relative-name, content
  mkdir -p "$(dirname "$ob/tests/obligations.d/$1")"
  printf '%s\n' "$2" > "$ob/tests/obligations.d/$1"; }
mkdir -p "$ob/src/workloads/compression"
printf 'int c();\n' > "$ob/src/workloads/compression/codec.hpp"
led "$core_row"
expect "a module in neither the ledger nor a fragment FAILS" 1 $? "obligations.d/workloads/compression.toml" "$out"

frag workloads/compression.toml '[[module]]
path    = "src/workloads/compression"
classes = ["P1", "P3"]
tiers   = ["T1"]'
led "$core_row"
expect "a fragment named after its module passes"           0 $? "ok    src/workloads/compression" "$out"
expect "the summary counts the fragment"                    0 $? "1 fragment(s)" "$out"

frag workloads/compression.toml '[[module]]
path    = "src/core"
classes = ["P1"]
tiers   = ["T1"]'
led "$core_row"
expect "a fragment declaring another module FAILS"           1 $? "only the module it is named after" "$out"

frag workloads/compression.toml '[[module]]
path    = "src/workloads/compression"
classes = ["P1"]
tiers   = ["T1"]

[[module]]
path    = "src/workloads/compression"
classes = ["P1"]
tiers   = ["T1"]'
led "$core_row"
expect "a fragment declaring two modules FAILS"              1 $? "declares 2 module(s)" "$out"

frag workloads/compression.toml '[[module]
path = broken'
led "$core_row"
expect "a fragment that does not parse FAILS"                1 $? "compression.toml does not parse as TOML" "$out"

frag workloads/compression.toml '[[module]]
path    = "src/workloads/compression"
classes = ["P1"]
tiers   = ["T1"]'
led "$core_row

[[module]]
path    = \"src/workloads/compression\"
classes = [\"P1\"]
tiers   = [\"T1\"]"
expect "a module in the ledger and in a fragment FAILS"      1 $? "declared twice" "$out"

frag workloads/compression.tml 'path = "src/workloads/compression"'
led "$core_row"
expect "a file in the fragment directory that is not one FAILS" 1 $? "is not a .toml fragment" "$out"
rm -f "$ob/tests/obligations.d/workloads/compression.tml"

frag workloads/compression.toml '[[module]]
path    = "src/workloads/compression"
classes = ["P1"]
tiers   = ["T1"]
cosumes = ["ExecutorContract"]'
led "$core_row"
expect "a misspelt field in a fragment FAILS"                1 $? "unknown field" "$out"

# Cross-execution (ADR 0003): a module that provides or consumes a contract runs
# the contract's own suite from tests/contracts/ -- against its implementation,
# or against its own mock. The gate finds the suite declared and the module's
# tests instantiating it, and refuses a declaration with either missing.
contract_row() { # field, name
  frag workloads/compression.toml "[[module]]
path    = \"src/workloads/compression\"
classes = [\"P1\"]
tiers   = [\"T1\"]
$1 = [\"$2\"]"; led "$core_row"; }
contract_row consumes ExecutorContract
expect "consuming a contract with no suite FAILS"            1 $? "is not a contract suite" "$out"

mkdir -p "$ob/tests/contracts/worker"
printf 'TYPED_TEST_SUITE_P(ExecutorContract);\n' > "$ob/tests/contracts/worker/executor_contract.hpp"
contract_row consumes ExecutorContract
expect "a consumer that never runs the suite FAILS"          1 $? "against its own mock" "$out"

mkdir -p "$ob/tests/contracts/workloads/compression"
printf 'INSTANTIATE_TYPED_TEST_SUITE_P(Mock, ExecutorContract, Types);\n' \
  > "$ob/tests/contracts/workloads/compression/mock_test.cpp"
contract_row consumes ExecutorContract
expect "an instantiation inside tests/contracts does not count" 1 $? "against its own mock" "$out"
rm -rf "$ob/tests/contracts/workloads"

mkdir -p "$ob/tests/unit/workloads/compression/deeper"
printf 'INSTANTIATE_TYPED_TEST_SUITE_P(\n    Mock, ExecutorContract, ::testing::Types<M>);\n' \
  > "$ob/tests/unit/workloads/compression/deeper/executor_mock_test.cpp"
contract_row consumes ExecutorContract
expect "a consumer running the suite against its mock passes" 0 $? "1 provides/consumes" "$out"

contract_row provides ExecutorContract
expect "a provider running the suite passes"                 0 $? "ok    src/workloads/compression" "$out"

# The instantiation must belong to THIS module: one in a more specific module's
# tests serves that module, not its parent.
mkdir -p "$ob/src/workloads/compression/deeper"
printf 'int d();\n' > "$ob/src/workloads/compression/deeper/d.hpp"
frag workloads/compression/deeper.toml '[[module]]
path    = "src/workloads/compression/deeper"
classes = ["P1"]
tiers   = ["T1"]'
contract_row provides ExecutorContract
expect "a child module's instantiation does not serve its parent" 1 $? "against its implementation" "$out"
rm -rf "$ob/src/workloads/compression/deeper" "$ob/tests/obligations.d/workloads/compression"

frag workloads/compression.toml '[[module]]
path     = "src/workloads/compression"
classes  = ["P1"]
tiers    = ["T1"]
provides = "ExecutorContract"'
led "$core_row"
expect "provides that is not a list FAILS"                   1 $? "must be a list of contract suite names" "$out"

# T1 is served by the contracts' own unit tests too.
rm -rf "$ob/tests/unit"
mkdir -p "$ob/tests/unit"
frag workloads/compression.toml '[[module]]
path    = "src/workloads/compression"
classes = ["P1"]
tiers   = ["T1"]'
led "$core_row"
expect "T1 is served by tests/contracts alone"               0 $? "ok    src/core" "$out"

rm -rf "$ob"

echo "cmake/Components.cmake: build fragments"
# The glob that replaced editing the two shared CMake files (ADR 0004). It must
# add every fragment, skip the excluded trees and files, and fail loudly -- not
# add twice, not skip -- when a fragment adds another itself.
if command -v cmake >/dev/null 2>&1; then
  cf="$(mktemp -d)"
  mkdir -p "$cf/tree/a/b" "$cf/tree/c" "$cf/tree/fixtures/x" "$cf/tree/kept"
  cat > "$cf/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.28)
project(fragments NONE)
include("$ROOT/cmake/Components.cmake")
loadforge_add_fragments("\${CMAKE_CURRENT_SOURCE_DIR}/tree" EXCLUDE kept/CMakeLists.txt EXCLUDE_TREES fixtures)
CMAKE
  printf 'message(STATUS "fragment-in: tree root, which is never a fragment")\n' > "$cf/tree/CMakeLists.txt"
  printf 'message(STATUS "fragment-in: a/b")\n' > "$cf/tree/a/b/CMakeLists.txt"
  printf 'message(STATUS "fragment-in: c")\n' > "$cf/tree/c/CMakeLists.txt"
  printf 'message(STATUS "fragment-in: fixtures")\n' > "$cf/tree/fixtures/x/CMakeLists.txt"
  printf 'message(STATUS "fragment-in: kept")\n' > "$cf/tree/kept/CMakeLists.txt"
  out="$(cmake -S "$cf" -B "$cf/build" 2>&1)"; rc=$?
  expect "every fragment is added"                           0 $rc "2 build fragment(s) under tree/" "$out"
  expect "nested fragments are found"                        0 $rc "fragment-in: a/b" "$out"
  if grep -qE "fragment-in: (fixtures|kept|tree root)" <<< "$out"; then
    expect "excluded trees and files are not added"          1 0
  else
    expect "excluded trees and files are not added"          0 0
  fi
  printf 'add_subdirectory(b)\n' > "$cf/tree/a/CMakeLists.txt"
  rm -rf "$cf/build"
  out="$(cmake -S "$cf" -B "$cf/build" 2>&1)"
  expect "a fragment that adds another FAILS at configure"   1 $? "already" "$out"
  rm -rf "$cf"
else
  echo "  FAIL  cmake is not installed, so the fragment glob is untested"; fail=$((fail + 1))
fi

# Merge commits built with plumbing, so that every field is the test's to set.
# mk_merge <repo> <parent1> <parent2> <author-email> <committer-email> <message> [extra]
#   tree = the clean automatic merge of the parents, plus one new file when a
#   seventh argument is given (content neither parent has), or parent1's own
#   tree when it is "ours" (a merge that discards what the other side did).
mk_merge() {
  local repo="$1" p1="$2" p2="$3" tree idx
  tree="$(git -C "$repo" merge-tree --write-tree "$p1" "$p2" | head -1)"
  case "${7-}" in
    "") ;;
    ours) tree="$(git -C "$repo" rev-parse "$p1^{tree}")" ;;
    *)  idx="$(mktemp -u)"
        GIT_INDEX_FILE="$idx" git -C "$repo" read-tree "$tree"
        GIT_INDEX_FILE="$idx" git -C "$repo" update-index --add --cacheinfo \
          "100644,$(echo smuggled | git -C "$repo" hash-object -w --stdin),$7"
        tree="$(GIT_INDEX_FILE="$idx" git -C "$repo" write-tree)"; rm -f "$idx" ;;
  esac
  GIT_AUTHOR_NAME=A GIT_AUTHOR_EMAIL="$4" GIT_COMMITTER_NAME=C GIT_COMMITTER_EMAIL="$5" \
    git -C "$repo" commit-tree "$tree" -p "$p1" -p "$p2" -m "$6"
}
# Two diverged commits in a repository, printed as "p1 p2".
mk_fork() { # repo, author-email
  local repo="$1" root a b
  root="$(git -C "$repo" rev-parse HEAD)"
  git -C "$repo" checkout -q -B fork-a "$root"
  echo a > "$repo/fa.txt"; git -C "$repo" add -A
  GIT_AUTHOR_EMAIL="$2" GIT_COMMITTER_EMAIL="$2" git -C "$repo" commit -q --no-verify -m "a" -m "Signed-off-by: A <$2>"
  a="$(git -C "$repo" rev-parse HEAD)"
  git -C "$repo" checkout -q -B fork-b "$root"
  echo b > "$repo/fb.txt"; git -C "$repo" add -A
  GIT_AUTHOR_EMAIL="$2" GIT_COMMITTER_EMAIL="$2" git -C "$repo" commit -q --no-verify -m "b" -m "Signed-off-by: A <$2>"
  b="$(git -C "$repo" rev-parse HEAD)"
  echo "$a $b"
}
GH=noreply@github.com
MSG_FORGED=$'Merge pull request #9\n\nCo-authored-by: Someone <s@example.invalid>'
OWNER_SOB=$'\n\nSigned-off-by: Owner <owner@example.invalid>'

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
GIT_COMMITTER_NAME=GitHub GIT_COMMITTER_EMAIL=noreply@github.com \
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
GIT_COMMITTER_NAME=GitHub GIT_COMMITTER_EMAIL=noreply@github.com \
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

# A merge is exempt only when it is GitHub's clean merge: two parents, GitHub
# as committer, and a tree equal to the automatic merge of the parents.
git -C "$dco" checkout -q -B fork-base "$base"
read -r fa fb < <(mk_fork "$dco" contributor@example.invalid)
forged="$(mk_merge "$dco" "$fa" "$fb" contributor@example.invalid $GH "$MSG_FORGED" smuggled.txt)"
out="$("$ROOT/tools/check-dco.sh" "$base" "$forged" "$dco" 2>&1)"
expect "a forged GitHub merge that adds content FAILS"       1 $? "MISSING Signed-off-by: .*Merge pull request" "$out"
genuine="$(mk_merge "$dco" "$fa" "$fb" contributor@example.invalid $GH "Merge pull request #9")"
out="$("$ROOT/tools/check-dco.sh" "$base" "$genuine" "$dco" 2>&1)"
expect "a genuine clean GitHub merge passes"                 0 $? "0 unsigned" "$out"
ours="$(mk_merge "$dco" "$fa" "$fb" contributor@example.invalid $GH "Merge pull request #9" ours)"
out="$("$ROOT/tools/check-dco.sh" "$base" "$ours" "$dco" 2>&1)"
expect "a GitHub merge that is not the clean merge FAILS"    1 $? "MISSING Signed-off-by" "$out"
human="$(mk_merge "$dco" "$fa" "$fb" contributor@example.invalid contributor@example.invalid $'Merge main\n\nSigned-off-by: C <contributor@example.invalid>')"
out="$("$ROOT/tools/check-dco.sh" "$base" "$human" "$dco" 2>&1)"
expect "a human merge with a sign-off passes"                0 $? "0 unsigned" "$out"
human_unsigned="$(mk_merge "$dco" "$fa" "$fb" contributor@example.invalid contributor@example.invalid "Merge main")"
out="$("$ROOT/tools/check-dco.sh" "$base" "$human_unsigned" "$dco" 2>&1)"
expect "a human merge without a sign-off FAILS"              1 $? "MISSING Signed-off-by: .*Merge main" "$out"
git -C "$dco" checkout -q -B other-sob "$base"
echo o > "$dco/file.txt"; git -C "$dco" add -A
git -C "$dco" commit -q -m "other" -m "Signed-off-by: Other <other@example.invalid>"
out="$("$ROOT/tools/check-dco.sh" "$base" HEAD "$dco" 2>&1)"
expect "a sign-off with another email FAILS"                 1 $? "MISSING Signed-off-by" "$out"

rm -rf "$dco"

echo "check-commit-identity.sh"
# Real repositories again: the gate reads commit objects, and the cases that
# matter -- a GitHub-shaped merge, a signature -- are shapes of those objects.
idr="$(mktemp -d)"; ids="$(mktemp)"
git init -q "$idr"
git -C "$idr" config user.name "Identity Test"
git -C "$idr" config commit.gpgsign false
printf '# admitted\nowner@example.invalid\n\n' > "$ids"
id_commit() { # name, email, message, [committer-email]; signs off as the author unless the message has a sign-off
  local msg="$3"
  grep -q '^Signed-off-by:' <<< "$msg" || msg+=$'\n\n'"Signed-off-by: $1 <$2>"
  mktemp -p "$idr" f.XXXXXX >/dev/null; git -C "$idr" add -A
  GIT_AUTHOR_NAME="$1" GIT_AUTHOR_EMAIL="$2" \
  GIT_COMMITTER_NAME="$1" GIT_COMMITTER_EMAIL="${4:-$2}" \
    git -C "$idr" commit -q --no-verify -m "$msg"
}
idg() { "$ROOT/tools/check-commit-identity.sh" "${1:-HEAD}" "$idr" "${2:-$ids}" 2>&1; }

id_commit Owner owner@example.invalid $'first\n\nSigned-off-by: Owner <owner@example.invalid>'
root="$(git -C "$idr" rev-parse HEAD)"
out="$(idg)"; expect "an admitted author and committer pass"          0 $? "0 problem(s)" "$out"

id_commit OWNER OWNER@Example.Invalid "case differs"
out="$(idg)"; expect "email comparison ignores case"                  0 $? "0 problem(s)" "$out"

git -C "$idr" checkout -q -b stranger "$root"
id_commit Someone someone@example.invalid "unadmitted author"
out="$(idg)"; expect "an author not admitted FAILS"                   1 $? "author <someone@example.invalid>" "$out"

git -C "$idr" checkout -q -B c2 "$root"
id_commit Owner owner@example.invalid "unadmitted committer" other@example.invalid
out="$(idg)"; expect "a committer not admitted FAILS"                 1 $? "committer <other@example.invalid>" "$out"

git -C "$idr" checkout -q -B c3 "$root"
id_commit Owner owner@example.invalid $'second author\n\nCo-authored-by: Someone <someone@example.invalid>\nSigned-off-by: Owner <owner@example.invalid>'
out="$(idg)"; expect "a co-author trailer FAILS without being named"  1 $? "trailer 'Co-authored-by'" "$out"

git -C "$idr" checkout -q -B c4 "$root"
id_commit Owner owner@example.invalid $'odd trailer\n\nAnything-Else: here\nSigned-off-by: Owner <owner@example.invalid>'
out="$(idg)"; expect "any trailer but Signed-off-by FAILS"            1 $? "trailer 'Anything-Else'" "$out"

git -C "$idr" checkout -q -B c4b "$root"
id_commit Owner owner@example.invalid $'buried co-author\n\nCo-authored-by: Someone <someone@example.invalid>\n\nprose after it\n\nSigned-off-by: Owner <owner@example.invalid>'
out="$(idg)"; expect "a co-author line outside the trailers FAILS"   1 $? "a 'Co-authored-by:' line" "$out"

# A signature, made by writing the header into a commit object directly: no key
# is needed to show that the gate refuses the shape.
signed="$(git -C "$idr" cat-file commit "$root" | sed '/^committer /a gpgsig -----BEGIN SSH SIGNATURE-----\n -----END SSH SIGNATURE-----' |
          git -C "$idr" hash-object -t commit -w --stdin)"
out="$(idg "$signed")"; expect "a signed commit FAILS"                1 $? "signed; commits here are unsigned" "$out"

# GitHub's merge: GitHub commits it and signs it, the author is whoever merged,
# and the PR title in the body can parse as a trailer. Only the author is judged.
git -C "$idr" checkout -q -B side "$root"
id_commit Owner owner@example.invalid "side work"
git -C "$idr" checkout -q -B gh-main "$root"
id_commit Owner owner@example.invalid "main work"
GIT_AUTHOR_NAME="OWNER" GIT_AUTHOR_EMAIL="owner@example.invalid" \
GIT_COMMITTER_NAME="GitHub" GIT_COMMITTER_EMAIL="noreply@github.com" \
  git -C "$idr" merge -q --no-ff --no-verify -m $'Merge pull request #1 from side\n\nPlatform: a title that parses as a trailer' side
merge="$(git -C "$idr" rev-parse HEAD)"
merge_signed="$(git -C "$idr" cat-file commit "$merge" | sed '/^committer /a gpgsig -----BEGIN PGP SIGNATURE-----\n -----END PGP SIGNATURE-----' |
                git -C "$idr" hash-object -t commit -w --stdin)"
out="$(idg "$merge_signed")"; expect "GitHub's signed merge commit passes"      0 $? "0 problem(s)" "$out"

git -C "$idr" checkout -q -B gh-bad "$root"
id_commit Owner owner@example.invalid "pretends to be a merge" noreply@github.com
out="$(idg)"; expect "a GitHub committer on a non-merge FAILS"        1 $? "committer <noreply@github.com>" "$out"

git -C "$idr" checkout -q -B gh-stranger "$root"
GIT_AUTHOR_NAME="Someone" GIT_AUTHOR_EMAIL="someone@example.invalid" \
GIT_COMMITTER_NAME="GitHub" GIT_COMMITTER_EMAIL="noreply@github.com" \
  git -C "$idr" merge -q --no-ff --no-verify -m "Merge pull request #2" side
out="$(idg)"; expect "a GitHub merge by someone not admitted FAILS"   1 $? "author <someone@example.invalid>" "$out"

# The merge exemption rests on the tree, not on forgeable fields.
git -C "$idr" checkout -q -B fork-root "$root"
read -r fa fb < <(mk_fork "$idr" owner@example.invalid)
forged="$(mk_merge "$idr" "$fa" "$fb" owner@example.invalid $GH "$MSG_FORGED" smuggled.txt)"
out="$(idg "$forged")"; expect "a forged GitHub merge that adds content FAILS"     1 $? "trailer 'Co-authored-by'" "$out"
expect "  ... and for want of a sign-off"                            1 1 "no Signed-off-by for <owner@example.invalid>" "$out"
genuine="$(mk_merge "$idr" "$fa" "$fb" owner@example.invalid $GH "Merge pull request #9")"
out="$(idg "$genuine")"; expect "a genuine clean GitHub merge with no trailers passes" 0 $? "0 problem(s)" "$out"
ours="$(mk_merge "$idr" "$fa" "$fb" owner@example.invalid $GH "Merge pull request #9" ours)"
out="$(idg "$ours")"; expect "a GitHub merge that is not the clean merge FAILS"   1 $? "no Signed-off-by" "$out"
unadmitted="$(mk_merge "$idr" "$fa" "$fb" owner@example.invalid other@example.invalid "Merge x$OWNER_SOB" extra.txt)"
out="$(idg "$unadmitted")"; expect "a merge with an unadmitted committer and content FAILS" 1 $? "committer <other@example.invalid>" "$out"
unadmitted_clean="$(mk_merge "$idr" "$fa" "$fb" owner@example.invalid other@example.invalid "Merge x$OWNER_SOB")"
out="$(idg "$unadmitted_clean")"; expect "a clean merge by an unadmitted non-GitHub committer FAILS" 1 $? "committer <other@example.invalid>" "$out"
human="$(mk_merge "$idr" "$fa" "$fb" owner@example.invalid owner@example.invalid "Merge main$OWNER_SOB")"
out="$(idg "$human")"; expect "a human merge with a sign-off passes"               0 $? "0 problem(s)" "$out"
human_unsigned="$(mk_merge "$idr" "$fa" "$fb" owner@example.invalid owner@example.invalid "Merge main")"
out="$(idg "$human_unsigned")"; expect "a human merge without a sign-off FAILS"    1 $? "no Signed-off-by" "$out"
forged_author="$(mk_merge "$idr" "$fa" "$fb" someone@example.invalid $GH "Merge pull request #9")"
out="$(idg "$forged_author")"; expect "a clean GitHub merge still needs an admitted author" 1 $? "author <someone@example.invalid>" "$out"

git -C "$idr" checkout -q -B sob "$root"
id_commit Owner owner@example.invalid "signed by another" ; git -C "$idr" reset -q --soft HEAD~1
GIT_AUTHOR_EMAIL=owner@example.invalid GIT_COMMITTER_EMAIL=owner@example.invalid \
  git -C "$idr" commit -q --no-verify -m "other sob" -m "Signed-off-by: Other <other@example.invalid>"
out="$(idg)"; expect "a sign-off with another email FAILS"                         1 $? "no Signed-off-by for <owner@example.invalid>" "$out"
git -C "$idr" reset -q --hard "$root"; echo q >> "$idr/q"; git -C "$idr" add -A
GIT_AUTHOR_EMAIL=owner@example.invalid GIT_COMMITTER_EMAIL=owner@example.invalid \
  git -C "$idr" commit -q --no-verify -m "none"
out="$(idg)"; expect "a missing sign-off FAILS"                                    1 $? "no Signed-off-by" "$out"

bl="$(mktemp)"; echo "$(git -C "$idr" rev-parse HEAD) a note" > "$bl"
out="$("$ROOT/tools/check-commit-identity.sh" HEAD "$idr" "$ids" "" "$bl" 2>&1)"
expect "a listed pre-DCO commit is excused its sign-off only" 0 $? "0 problem(s)" "$out"
git -C "$idr" checkout -q -B bl-bad "$root"
GIT_AUTHOR_EMAIL=someone@example.invalid GIT_COMMITTER_EMAIL=owner@example.invalid \
  git -C "$idr" commit -q --no-verify --allow-empty -m "unsigned and unadmitted"
echo "$(git -C "$idr" rev-parse HEAD)" > "$bl"
out="$("$ROOT/tools/check-commit-identity.sh" HEAD "$idr" "$ids" "" "$bl" 2>&1)"
expect "...but its author is still judged"                    1 $? "author <someone@example.invalid>" "$out"
rm -f "$bl"

out="$(idg does-not-exist)"; expect "an unresolvable ref FAILS"      1 $? "is not a commit" "$out"
out="$(idg HEAD /nonexistent/identities)"; expect "a missing identities file FAILS" 1 $? "cannot read" "$out"
printf '# nobody\n\n' > "$ids"
out="$(idg "$root")"; expect "an identities file admitting nobody FAILS" 1 $? "admits nobody" "$out"

rm -rf "$idr" "$ids"

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

echo "check-no-test-hooks.sh"
# Real binaries, compiled here, because the gate reads real symbol tables and a
# hand-written imitation of nm's output would agree with whatever the gate
# believes (F21). Each refused binary differs from the passing one in exactly
# the thing the case is about. No compiler is a FAILURE, not a skip: the cases
# that prove the gate can refuse are the only ones that prove it is a gate.
HOOKS="$ROOT/tools/check-no-test-hooks.sh"
th="$(mktemp -d)"
hooks() { out="$("$HOOKS" "$@" 2>&1)"; return $?; }
build() { # name, extra source; every binary gets a loadforge:: symbol unless told not to
  { printf 'namespace loadforge::worker { int run(int argc) { return argc - 1; } }\n'
    printf '%s\n' "$2"
    printf 'int main(int argc, char**) { return loadforge::worker::run(argc); }\n'
  } > "$th/$1.cpp"
  "${CXX:-c++}" -std=c++20 -O0 -o "$th/$1" "$th/$1.cpp" 2> "$th/$1.log"
}
if build clean '' \
   && build fake 'namespace loadforge::fake_worker { int misbehave() { return 7; } }
                  int poke() { return loadforge::fake_worker::misbehave(); }' \
   && build fakes 'namespace loadforge::testing { struct FakeSyscalls { int answer(); };
                   int FakeSyscalls::answer() { return 3; } }
                   int poke() { return loadforge::testing::FakeSyscalls{}.answer(); }' \
   && build gtest 'namespace testing { class Test { public: virtual ~Test(); }; Test::~Test() = default; }' \
   && build selector '#include <cstdlib>
                      const char* poke() { return std::getenv("LOADFORGE_FAKE_WORKER"); }' \
   && build lookalike 'namespace loadforge::attesting { int fine() { return 1; } }
                       int poke() { return loadforge::attesting::fine(); }'; then
  hooks "$th/clean"
  expect "a binary with no test hook passes"                   0 $? "0 test hook(s), ok" "$out"
  hooks "$th/fake"
  expect "a fake-worker symbol FAILS, named"                   1 $? "TEST HOOK: .*loadforge::fake_worker::misbehave" "$out"
  hooks "$th/fakes"
  expect "a test-fake symbol FAILS, named"                     1 $? "TEST HOOK: .*loadforge::testing::FakeSyscalls" "$out"
  hooks "$th/gtest"
  expect "a GoogleTest symbol FAILS, named"                    1 $? "TEST HOOK: .*testing::Test" "$out"
  hooks "$th/selector"
  expect "the fake worker's selector string FAILS"             1 $? "the selector string" "$out"
  hooks "$th/lookalike"
  expect "a name that merely ends in 'testing' passes"         0 $? "0 test hook(s), ok" "$out"

  cp "$th/fake" "$th/stripped" && strip "$th/stripped"
  hooks "$th/stripped"
  expect "a stripped binary FAILS: nothing read is not a pass" 1 $? "no symbol table" "$out"
  printf 'int main() { return 0; }\n' > "$th/foreign.cpp"
  "${CXX:-c++}" -O0 -o "$th/foreign" "$th/foreign.cpp" 2> "$th/foreign.log"
  hooks "$th/foreign"
  expect "a binary with no loadforge:: symbol FAILS"           1 $? "not a LoadForge binary" "$out"
else
  echo "  FAIL  could not compile the test-hook fixtures with ${CXX:-c++}:"
  cat "$th"/*.log 2>/dev/null | sed 's/^/          /'
  fail=$((fail + 1))
fi
: > "$th/empty"
hooks "$th/empty"
expect "an empty file FAILS"                                   1 $? "empty" "$out"
printf 'not an object file\n' > "$th/text"
hooks "$th/text"
expect "a file nm cannot read FAILS"                           1 $? "nm could not read it" "$out"
hooks "$th/no-such-binary"
expect "a missing binary FAILS"                                1 $? "not a file" "$out"
hooks
expect "no argument FAILS"                                     1 $? "usage" "$out"
rm -rf "$th"

echo; echo "  tooling tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
