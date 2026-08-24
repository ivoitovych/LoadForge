#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate third_party/MANIFEST.toml.

Checks that every declared component carries the facts we need to reproduce and
audit it, that its licence is GPL-compatible, and that anything actually present
under third_party/ is declared. Runs in CI and locally.
"""
import pathlib
import re
import sys

ALLOWED_LICENCES = {
    "BSD-3-Clause", "BSD-2-Clause", "MIT", "Apache-2.0",
    "GPL-3.0-or-later", "LGPL-2.1-or-later", "LGPL-3.0-or-later", "ISC", "Zlib",
}
REQUIRED_FIELDS = {"name", "version", "sha", "source", "licence", "kind", "linked"}

root = pathlib.Path.cwd()
if not (root / "third_party").exists():
    root = pathlib.Path(__file__).resolve().parent.parent
manifest = root / "third_party" / "MANIFEST.toml"
failed = False


def fail(message: str) -> None:
    global failed
    print(f"  FAIL  {message}", file=sys.stderr)
    failed = True


if not (root / "NOTICE").exists():
    fail("NOTICE is missing")

if not manifest.exists():
    fail("third_party/MANIFEST.toml is missing")
    sys.exit(1)

declared = set()
pinned_shas = {}
for block in manifest.read_text(encoding="utf-8").split("[[component]]")[1:]:
    fields = dict(re.findall(r'^\s*(\w+)\s*=\s*"([^"]*)"', block, re.M))
    name = fields.get("name", "<unnamed>")
    declared.add(name)
    if fields.get("kind") == "fetched":
        pinned_shas[name] = fields.get("sha", "")

    missing = REQUIRED_FIELDS - fields.keys()
    if missing:
        fail(f"{name}: missing required field(s) {sorted(missing)}")

    licence = fields.get("licence", "")
    if licence not in ALLOWED_LICENCES:
        fail(f"{name}: licence {licence!r} is not on the GPL-compatible allowlist")

    if fields.get("kind") == "fetched" and not re.fullmatch(r"[0-9a-f]{40}", fields.get("sha", "")):
        fail(f"{name}: 'sha' must be a full 40-character commit SHA, not a movable tag")

    if not failed:
        print(f"  ok    {name} {fields.get('version')} [{licence}] "
              f"{fields.get('kind')}, linked={fields.get('linked')}")

# The manifest is only authoritative if the build actually uses what it declares.
# Without this, MANIFEST.toml could record a reviewed SHA while
# cmake/Dependencies.cmake downloaded a different one, and every check passed.
deps_cmake = root / "cmake" / "Dependencies.cmake"
if deps_cmake.exists():
    cmake_text = deps_cmake.read_text(encoding="utf-8")
    for name, sha in pinned_shas.items():
        if name == "googletest":
            found = re.search(r'LOADFORGE_GTEST_SHA\s+"([0-9a-f]{40})"', cmake_text)
            if not found:
                fail("cmake/Dependencies.cmake: no LOADFORGE_GTEST_SHA pin found")
            elif found.group(1) != sha:
                fail(f"googletest: MANIFEST.toml says {sha}, "
                     f"cmake/Dependencies.cmake pins {found.group(1)}")
            else:
                print(f"  ok    googletest pin matches cmake/Dependencies.cmake")

third_party = root / "third_party"
for entry in sorted(p.name for p in third_party.iterdir() if p.is_dir()):
    if entry not in declared:
        fail(f"third_party/{entry} is present but not declared in MANIFEST.toml")

print(f"\ndependency check: {len(declared)} declared, {'FAILED' if failed else 'ok'}")
sys.exit(1 if failed else 0)
