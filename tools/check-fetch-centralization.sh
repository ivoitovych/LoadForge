#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every external fetch lives in one reviewed module.
#
# tools/check-dependencies.py reconciles third_party/MANIFEST.toml against
# cmake/Dependencies.cmake and any file a component names in 'pinned_by'. That
# reconciliation is only as complete as its list of places to look: a
# FetchContent_Declare added to CMakeLists.txt, or to some new module nobody
# declared, would be fetched by the build and seen by nothing.
#
# There is no such dependency today -- the tree is centralised correctly. This
# gate exists to keep it that way while M1 grows the CMake tree, because the
# convention is currently held up by nothing but everyone remembering it.
#
# The rule: the primitives that reach the network may appear only in the modules
# listed in ALLOWED below. Anywhere else is a failure, and the fix is to declare
# the dependency properly rather than to widen this list.
set -uo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

# The reviewed home for external dependencies. Adding to this list is a decision
# about the project's supply chain, not a formality.
ALLOWED=(
  "cmake/Dependencies.cmake"
)

# Primitives that can pull code into the build from outside the repository.
FETCH_RE='FetchContent_Declare|FetchContent_MakeAvailable|FetchContent_Populate|ExternalProject_Add|file[[:space:]]*\([[:space:]]*DOWNLOAD|execute_process[^)]*(git[[:space:]]+clone|curl|wget)'

bad=0
while IFS= read -r hit; do
  file="${hit%%:*}"
  relative="${file#"$ROOT"/}"
  allowed=0
  for permitted in "${ALLOWED[@]}"; do
    [ "$relative" = "$permitted" ] && allowed=1
  done
  if [ "$allowed" -eq 0 ]; then
    echo "  EXTERNAL FETCH OUTSIDE THE DEPENDENCY MODULE: $hit" >&2
    bad=$((bad + 1))
  fi
done < <(grep -rnE "$FETCH_RE" "$ROOT/cmake" "$ROOT/src" "$ROOT/tests" "$ROOT/CMakeLists.txt" \
           --include='*.cmake' --include='CMakeLists.txt' 2>/dev/null || true)

if [ "$bad" -ne 0 ]; then
  echo "" >&2
  echo "Declare the dependency in third_party/MANIFEST.toml and fetch it from one of:" >&2
  printf '  %s\n' "${ALLOWED[@]}" >&2
  echo "so that tools/check-dependencies.py can reconcile the pin. A fetch the manifest" >&2
  echo "cannot see is a dependency nobody reviewed." >&2
fi

echo "external-fetch centralization: $bad violation(s)"
[ "$bad" -eq 0 ]
