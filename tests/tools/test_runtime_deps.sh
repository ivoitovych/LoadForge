#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tests for the runtime-dependency gate.
#
# A separate file from test_tooling.sh on purpose: this landed while the
# platform-seam change was still in review and touching that file, and two
# parallel changes editing one test runner is a merge conflict for no benefit.
# They can be merged into one runner once both have landed.
#
# The cases below use REAL binaries, compiled here, rather than fixture text.
# A gate that inspects ELF objects tested against strings the author invented is
# testing the author's idea of an ELF object (F21), and this gate exists
# precisely because a claim in the documents had never met an artifact.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE="$ROOT/tools/check-runtime-deps.sh"
CC_BIN="${CC:-cc}"
pass=0; fail=0

# Runs the gate and saves BOTH the output and the exit status, so that two
# assertions about one run cannot read $? from each other. Taking $? after a
# previous expect has already run is a trap this project's test scripts have
# fallen into three times; the fix is to make it impossible rather than to
# remember. RC and OUT are the only things an assertion reads.
run_gate() {
  OUT="$("$GATE" "$@" 2>&1)"
  RC=$?
}

expect() { # description, expected-exit, needle -- always reads RC and OUT
  if [ "$RC" -eq "$1" ] && grep -q -- "$3" <<< "$OUT"; then
    echo "  PASS  $2"; pass=$((pass + 1))
  else
    echo "  FAIL  $2 (expected exit $1, got $RC, looking for '$3')"; fail=$((fail + 1))
    echo "$OUT" | sed 's/^/          /'
  fi
}

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "check-runtime-deps.sh"

# --- a plain C++ binary: the shape the project actually ships -----------------
cat > "$work/plain.cpp" <<'EOF'
#include <string>
int main() { return static_cast<int>(std::string("ok").size()) - 2; }
EOF
if ! "${CXX:-c++}" -o "$work/plain" "$work/plain.cpp" 2>"$work/build.log"; then
  echo "  FAIL  could not build the fixture binary"; sed 's/^/          /' "$work/build.log"; exit 1
fi
run_gate "$work/plain"
expect 0 "a plain C++ binary passes"                    "0 not permitted"

# --- THE case the gate exists for --------------------------------------------
# zlib is a real third-party shared library and a realistic thing for someone to
# reach for. If this does not fail, the gate is decoration.
cat > "$work/withzlib.c" <<'EOF'
#include <zlib.h>
int main(void) { return zlibVersion()[0] == 0; }
EOF
if "$CC_BIN" -o "$work/withzlib" "$work/withzlib.c" -lz 2>/dev/null; then
  run_gate "$work/withzlib"
  expect 1 "a real third-party shared library FAILS"    "THIRD-PARTY RUNTIME DEPENDENCY: libz"
  expect 1 "and the message says what to do instead"    "Vendor it header-only"

  # The allowlist argument exists for binaries that are not the shipped one --
  # the test binary links GoogleTest. It must never be used for `loadforge`.
  run_gate "$work/withzlib" libz.so.1
  expect 0 "an explicitly allowed library passes"       "ok        libz.so.1"
else
  echo "  SKIP  zlib headers unavailable; the failing case cannot be built here" >&2
  echo "        (this is the one case that proves the gate can fail -- do not" >&2
  echo "         let it stay skipped in CI)" >&2
  fail=$((fail + 1))
fi

# --- a static binary satisfies the promise trivially, and says so -------------
if "${CXX:-c++}" -static -o "$work/static" "$work/plain.cpp" 2>/dev/null; then
  run_gate "$work/static"
  expect 0 "a static binary reports no dependencies"    "statically linked"
else
  echo "  SKIP  no static libstdc++ available here"
fi

# --- evidence the gate must refuse to interpret ------------------------------
run_gate "$work/absent"
expect 1 "a missing binary FAILS"                       "does not exist"

printf '#!/bin/sh\necho not an elf object\n' > "$work/script.sh"
chmod +x "$work/script.sh"
run_gate "$work/script.sh"
expect 1 "a non-ELF file FAILS rather than reporting no deps" "not an ELF object"

: > "$work/empty"
run_gate "$work/empty"
expect 1 "an empty file FAILS"                          "not an ELF object"

echo; echo "  runtime-dependency tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
