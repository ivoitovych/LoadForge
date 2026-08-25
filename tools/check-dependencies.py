#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate third_party/MANIFEST.toml against the build that consumes it.

Checks that every declared component carries the facts we need to reproduce and
audit it, that its licence is GPL-compatible, that anything present under
third_party/ is declared -- and, in both directions, that the manifest and the
CMake files agree about what the build actually fetches.

Two fail-open defects shaped this file, and both are worth stating because the
shape of the code is a response to them:

  1. It used to parse TOML with a regular expression. A manifest that became
     empty, truncated to comments, or merely changed syntax produced zero
     declared components -- and zero components meant zero checks, zero
     mismatches, and a cheerful "0 declared, ok" while cmake/Dependencies.cmake
     went on fetching GoogleTest. Nothing was validated and nothing complained.
     Parsing is now tomllib, and a manifest that declares nothing while the
     build declares something is a failure.

  2. The manifest/CMake cross-check was hardcoded to GoogleTest by name. It
     therefore validated exactly one dependency and would have ignored every
     future one, silently, forever. The check is now derived from the manifest
     in both directions: every pinned SHA in the manifest must appear in the
     file it names, and every pinned SHA in those files must be declared.

The theme: a checker that cannot understand its own evidence must say so, not
report success by default.
"""

import pathlib
import re
import sys
import tomllib

ALLOWED_LICENCES = {
    "BSD-3-Clause",
    "BSD-2-Clause",
    "MIT",
    "Apache-2.0",
    "GPL-3.0-or-later",
    "LGPL-2.1-or-later",
    "LGPL-3.0-or-later",
    "ISC",
    "Zlib",
}
# Every field is required of every component; 'pinned_by' additionally so for
# 'fetched'. Unknown fields are rejected: a typo ('license' for 'licence') would
# otherwise disable a check while looking like extra documentation.
REQUIRED_FIELDS = {"name", "version", "sha", "source", "licence", "kind", "linked"}
OPTIONAL_FIELDS = {"pinned_by", "comment"}
KINDS = {"fetched", "vendored"}
SHA_RE = re.compile(r"[0-9a-f]{40}")

root = pathlib.Path.cwd()
if not (root / "third_party").exists():
    root = pathlib.Path(__file__).resolve().parent.parent
manifest = root / "third_party" / "MANIFEST.toml"
default_pin_file = pathlib.Path("cmake") / "Dependencies.cmake"
failed = False


def fail(message: str) -> None:
    global failed
    print(f"  FAIL  {message}", file=sys.stderr)
    failed = True


def die(message: str) -> None:
    print(f"  FAIL  {message}", file=sys.stderr)
    print("\ndependency check: FAILED", file=sys.stderr)
    sys.exit(1)


if not (root / "NOTICE").exists():
    fail("NOTICE is missing")

if not manifest.exists():
    die("third_party/MANIFEST.toml is missing")

try:
    document = tomllib.loads(manifest.read_text(encoding="utf-8"))
except (tomllib.TOMLDecodeError, UnicodeDecodeError) as error:
    die(f"third_party/MANIFEST.toml does not parse as TOML: {error}")

unexpected_tables = set(document) - {"component"}
if unexpected_tables:
    fail(
        f"MANIFEST.toml has unexpected top-level key(s) {sorted(unexpected_tables)}; "
        "the only one this checker understands is [[component]]"
    )

components = document.get("component", [])
if not isinstance(components, list) or any(not isinstance(c, dict) for c in components):
    die("MANIFEST.toml: 'component' must be an array of tables ([[component]])")

declared: dict[str, dict] = {}
pins: list[tuple[str, str, pathlib.Path]] = []  # (name, sha, file that must carry it)

for index, fields in enumerate(components):
    name = fields.get("name")
    if not isinstance(name, str) or not name:
        fail(f"component #{index + 1}: 'name' must be a non-empty string")
        name = f"<component #{index + 1}>"
    elif name in declared:
        # Two entries under one name: whichever the reader believes, the other
        # one is unreviewed. There is no safe way to pick.
        fail(f"{name}: declared twice; component names must be unique")
    declared[name] = fields

    missing = REQUIRED_FIELDS - fields.keys()
    if missing:
        fail(f"{name}: missing required field(s) {sorted(missing)}")
    unknown = fields.keys() - REQUIRED_FIELDS - OPTIONAL_FIELDS
    if unknown:
        fail(
            f"{name}: unknown field(s) {sorted(unknown)}; a misspelt field name "
            "silently disables the check that reads it"
        )

    for text_field in ("version", "source", "licence", "sha"):
        value = fields.get(text_field)
        if text_field in fields and (not isinstance(value, str) or not value):
            fail(f"{name}: '{text_field}' must be a non-empty string")

    kind = fields.get("kind")
    if "kind" in fields and kind not in KINDS:
        fail(f"{name}: 'kind' is {kind!r}; it must be one of {sorted(KINDS)}")

    # A boolean, not the string "false". The string is truthy in every language
    # that might later read this manifest, so "linked" would read as yes.
    if "linked" in fields and not isinstance(fields.get("linked"), bool):
        fail(f"{name}: 'linked' must be a TOML boolean (true/false), not {fields.get('linked')!r}")

    licence = fields.get("licence")
    if "licence" in fields and licence not in ALLOWED_LICENCES:
        fail(f"{name}: licence {licence!r} is not on the GPL-compatible allowlist")

    sha = fields.get("sha")
    if kind == "fetched":
        if not isinstance(sha, str) or not SHA_RE.fullmatch(sha):
            fail(f"{name}: 'sha' must be a full 40-character commit SHA, not a movable tag")
        pinned_by = fields.get("pinned_by")
        if not isinstance(pinned_by, str) or not pinned_by:
            fail(
                f"{name}: a fetched component must name the file that pins it "
                "in 'pinned_by', so the pin can be cross-checked"
            )
        elif isinstance(sha, str):
            pins.append((name, sha, pathlib.Path(pinned_by)))

# ---------------------------------------------------------------------------
# The manifest is only authoritative if the build actually uses what it declares.
# Both directions matter, and the reverse direction is the one that catches an
# empty or truncated manifest: a pin in the build that nothing declares.
# ---------------------------------------------------------------------------
pin_files = {default_pin_file} | {path for _, _, path in pins}
pin_text: dict[pathlib.Path, str] = {}
for relative in sorted(pin_files):
    absolute = root / relative
    if absolute.exists():
        pin_text[relative] = absolute.read_text(encoding="utf-8")

for name, sha, relative in pins:
    if relative not in pin_text:
        fail(f"{name}: pinned_by names {relative}, which does not exist")
    elif sha not in pin_text[relative]:
        fail(f"{name}: MANIFEST.toml pins {sha}, which does not appear in {relative}")
    else:
        print(f"  ok    {name} pin matches {relative}")

declared_shas = {sha for _, sha, _ in pins}
declared_sources = {c.get("source") for c in components if isinstance(c.get("source"), str)}
for relative, text in sorted(pin_text.items()):
    for sha in sorted(set(SHA_RE.findall(text))):
        if sha not in declared_shas:
            fail(
                f"{relative} pins commit {sha}, which no MANIFEST.toml component "
                "declares; every dependency the build fetches must be declared"
            )
    for url in sorted(set(re.findall(r"GIT_REPOSITORY\s+(\S+)", text))):
        if url not in declared_sources:
            fail(f"{relative} fetches {url}, which no MANIFEST.toml component declares")

for name, fields in declared.items():
    if not failed:
        print(
            f"  ok    {name} {fields.get('version')} [{fields.get('licence')}] "
            # str(False) is "False"; echo back the TOML spelling the file uses.
            f"{fields.get('kind')}, linked={str(fields.get('linked')).lower()}"
        )

third_party = root / "third_party"
for entry in sorted(p.name for p in third_party.iterdir() if p.is_dir()):
    if entry not in declared:
        fail(f"third_party/{entry} is present but not declared in MANIFEST.toml")

print(f"\ndependency check: {len(declared)} declared, {'FAILED' if failed else 'ok'}")
sys.exit(1 if failed else 0)
