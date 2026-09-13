#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The runtime-dependency gate: the shipped binary links nothing but the C and
# C++ runtime.
#
# docs/PLAN.md §5 states the promise and says it is "enforced by a CI check on
# the linked binary (lands at M0)". It was not. The only linkage check in the
# project asserted that the *static* artifact is static, which says nothing
# about what the dynamic build depends on -- and the dynamic build is what a
# distribution packages.
#
# The promise held when this was written; the binary needed only libstdc++,
# libgcc_s, libc and libm. Nothing kept it true, and vendoring toml++ made that
# matter: it is the first third-party code compiled into the binary, and the
# next one might not be header-only.
#
# WHY readelf AND NOT ldd
# ----------------------
# ldd may EXECUTE the binary to resolve its dependencies. Running an artifact in
# order to inspect it is a poor idea generally and a worse one in CI, and it also
# cannot inspect a binary for another architecture. readelf reads the ELF header
# and reports DT_NEEDED without running anything, and works cross-architecture --
# which matters here, because this project ships x86-64 and ARM64.
set -uo pipefail

BINARY="${1:?usage: check-runtime-deps.sh <binary> [extra-allowed-soname]...}"
shift || true

# The C and C++ runtime, and nothing else. These are not third-party
# dependencies in the sense the promise is about: they are the language
# implementation, they are present on any system that can run a C++ program,
# and a distribution does not "install a dependency" to provide them.
#
# Adding to this list is a change to what the project promises its users, not a
# formality. Anything else linked into the shipped binary must be argued for in
# an issue first (CONTRIBUTING.md).
ALLOWED=(
  "libc.so.6"
  "libm.so.6"
  "libstdc++.so.6"
  "libgcc_s.so.1"
)

# The dynamic loader, matched by pattern because its soname is
# architecture-specific: ld-linux-x86-64.so.2, ld-linux-aarch64.so.1,
# ld-linux-armhf.so.3, ld64.so.2 on ppc64le.
#
# It is not a dependency in any meaningful sense -- it is the thing that loads
# dependencies -- but whether it appears as DT_NEEDED at all varies by
# architecture and by how the binary was linked. On x86-64 this project's binary
# reaches it through PT_INTERP and it never shows up here; on AArch64 it is a
# NEEDED entry, which is what the first ARM CI run of this gate reported.
#
# That difference is exactly why the check runs on every matrix entry rather
# than once, and it showed up on the first run. The comment in ci.yml predicting
# it was written before it happened; the allowlist was written from the x86-64
# evidence in front of me, which is a smaller set of facts than the project
# ships on.
#
# The pattern is deliberately narrow -- glibc's documented loader names -- so
# that it cannot be satisfied by an arbitrary library that merely starts "ld".
LOADER_PATTERNS=( "ld-linux-*.so.*" "ld64.so.*" )
# A caller may allow more for a binary that is not the shipped one -- the test
# binary links GoogleTest, for instance -- but never for `loadforge` itself.
ALLOWED+=("$@")

[ -f "$BINARY" ] || {
  echo "check-runtime-deps: '$BINARY' does not exist." >&2
  echo "  Nothing was inspected, so nothing is certified. A missing artifact is a" >&2
  echo "  failure, not a pass." >&2
  exit 1
}

command -v readelf >/dev/null || {
  echo "check-runtime-deps: readelf not found -- refusing to report success." >&2
  echo "  Install binutils. A gate that cannot run has not run." >&2
  exit 1
}

# Confirm this is an ELF object at all before drawing conclusions from silence.
# Without this, a text file or a shell script would report zero dependencies and
# sail through, which is the F20 shape: "I found nothing" reading as "there is
# nothing".
if ! header="$(readelf -h "$BINARY" 2>&1)"; then
  echo "check-runtime-deps: '$BINARY' is not an ELF object readelf can parse:" >&2
  echo "$header" | sed 's/^/    /' >&2
  exit 1
fi
arch="$(grep -oE 'Machine: +.*' <<< "$header" | sed 's/Machine: *//' | head -n 1)"

dynamic="$(readelf -d "$BINARY" 2>/dev/null || true)"
mapfile -t needed < <(grep -oE 'Shared library: \[[^]]+\]' <<< "$dynamic" \
                      | sed -E 's/.*\[(.*)\]/\1/' | sort -u)

# A static binary has no dynamic section and therefore no NEEDED entries. That
# satisfies the promise trivially and is reported as such rather than silently,
# so a reader can tell "nothing to depend on" from "nothing found".
if [ "${#needed[@]}" -eq 0 ]; then
  if grep -q 'There is no dynamic section' <<< "$dynamic" || [ -z "$dynamic" ]; then
    echo "runtime dependencies (${arch}): none -- statically linked"
    exit 0
  fi
  echo "check-runtime-deps: '$BINARY' has a dynamic section but no NEEDED entries." >&2
  echo "  That is not a shape this gate understands, and an unrecognised shape is" >&2
  echo "  not evidence of a clean result." >&2
  exit 1
fi

violations=0
for library in "${needed[@]}"; do
  permitted=0
  for candidate in "${ALLOWED[@]}"; do
    [ "$library" = "$candidate" ] && permitted=1
  done
  for pattern in "${LOADER_PATTERNS[@]}"; do
    # shellcheck disable=SC2053  # the right-hand side is a glob, deliberately
    [[ "$library" == $pattern ]] && permitted=1
  done
  if [ "$permitted" -eq 1 ]; then
    echo "  ok        $library"
  else
    echo "  THIRD-PARTY RUNTIME DEPENDENCY: $library" >&2
    violations=$((violations + 1))
  fi
done

echo "runtime dependencies (${arch}): ${#needed[@]} linked, ${violations} not permitted"

if [ "$violations" -ne 0 ]; then
  echo "" >&2
  echo "LoadForge promises no third-party runtime dependencies (docs/PLAN.md §5)." >&2
  echo "It is not a stylistic preference: the tool has to run from live and rescue" >&2
  echo "media, where whatever the medium carries is all there is, and on a machine" >&2
  echo "whose owner already suspects their hardware. Every library that must be" >&2
  echo "present is one more thing that can be absent, wrong, or subtly different." >&2
  echo "" >&2
  echo "Vendor it header-only under third_party/, or discuss it in an issue first." >&2
  exit 1
fi
