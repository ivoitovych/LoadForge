#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The test-hook gate: the shipped binary carries nothing from the test tree.
#
# docs/IMPLEMENTATION.md §4 (M1, "Testing a multi-process supervisor"): the
# controller is driven against a worker that misbehaves on demand, and that
# worker is a separate test-only executable -- tests/support/fake_worker --
# rather than a hidden switch on loadforge, because a tool whose only product
# is trust must not ship a binary that can be told to corrupt its own results.
# "Never installed" is a build-system fact today. This makes it a checked fact
# about the artifact: whatever the build did, the binary that ships is read
# back, and one that carries a test hook is refused.
#
# WHAT COUNTS AS A TEST HOOK
# --------------------------
# Read from the symbol table with nm, demangled, so a C++ namespace is matched
# as written:
#
#   loadforge::fake_worker::   the fake worker's code
#   loadforge::testing::       the test fakes' namespace (FakeSyscalls)
#   testing::                  GoogleTest itself: a test framework in a
#                              release binary is a test hook by definition
#
# and one string, LOADFORGE_FAKE_WORKER, the fake worker's misbehaviour
# selector: a binary that reads that variable can be told to misbehave whatever
# its symbols are called.
#
# F20: EVIDENCE IT CANNOT READ IS A FAILURE
# ----------------------------------------
# A gate that inspects symbols passes trivially on a binary that has none. So a
# missing, empty or unreadable file fails; a file nm cannot read fails; a
# stripped binary fails (run this before stripping, never after); and a symbol
# table with no loadforge:: symbol in it fails, because that is not the binary
# this gate was asked about, or not one it can see into. "No test hooks found"
# is printed only over a symbol table that was demonstrably read.
set -uo pipefail

BINARY="${1:-}"

fail() {
  echo "  FAIL  $*" >&2
  echo "test-hook check: FAILED" >&2
  exit 1
}

[ -n "$BINARY" ] || fail "usage: check-no-test-hooks.sh <binary>"
[ -f "$BINARY" ] || fail "$BINARY: not a file"
[ -r "$BINARY" ] || fail "$BINARY: not readable"
[ -s "$BINARY" ] || fail "$BINARY: empty -- there is nothing to inspect, which is not a pass"

if ! symbols="$(nm -C "$BINARY" 2>&1)"; then
  fail "$BINARY: nm could not read it: $symbols"
fi
if [ -z "$symbols" ] || grep -q 'no symbols' <<< "$symbols"; then
  fail "$BINARY: no symbol table (stripped?). Run this gate before stripping, never after."
fi
total="$(grep -c . <<< "$symbols")"
if ! grep -q 'loadforge::' <<< "$symbols"; then
  fail "$BINARY: $total symbol(s) and none in namespace loadforge:: -- not a LoadForge binary, or not one whose symbols could be read"
fi

# A namespace match must start at a name boundary, so that 'testing::' is not
# also found inside 'loadforge::testing::' (which has its own pattern) or some
# 'attesting::' that has nothing to do with GoogleTest.
hooks="$(grep -E '(^|[^A-Za-z0-9_:])(loadforge::fake_worker::|loadforge::testing::|testing::)' \
           <<< "$symbols")"
selector=0
if grep -a -q -F 'LOADFORGE_FAKE_WORKER' "$BINARY"; then
  selector=1
fi

if [ -n "$hooks" ] || [ "$selector" -ne 0 ]; then
  count=0
  [ -n "$hooks" ] && count="$(grep -c . <<< "$hooks")"
  echo "$BINARY carries test hooks:" >&2
  if [ -n "$hooks" ]; then
    head -n 20 <<< "$hooks" | sed 's/^/  TEST HOOK: /' >&2
    [ "$count" -gt 20 ] && echo "  ... and $((count - 20)) more" >&2
  fi
  [ "$selector" -ne 0 ] &&
    echo "  TEST HOOK: the string LOADFORGE_FAKE_WORKER, the fake worker's selector" >&2
  fail "$BINARY: $count test-hook symbol(s)$([ "$selector" -ne 0 ] && echo ' and the selector string')"
fi

echo "test-hook check: $BINARY: $total symbol(s) read, 0 test hook(s), ok"
