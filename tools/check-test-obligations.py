#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Enforce the per-module test-obligation ledger (docs/IMPLEMENTATION.md §2.5).

The 100% coverage gate proves every branch ran. It cannot prove that the paths a
coverage tool does not enumerate -- syscall error modes, capability states,
process lifecycle transitions -- were even thought about, because their failure
arms are invisible to it: the success case covers the branch and gcov reports
100% either way.

This gate does not attempt to prove those paths are covered; no tool can, since
enumerating them is judgement. It proves the judgement was recorded, is current,
and that the tests it promises exist:

  * every directory under src/ appears in tests/obligations.toml;
  * every ledger entry names a directory that still exists;
  * every declared path class is one of the seven in the taxonomy;
  * every declared tier has test files that plausibly serve it;
  * every declared fixture exists on disk.

Built to the F20 standard, because a gate is only worth what it does with
evidence it cannot read: a ledger that does not parse is a failure, and an empty
ledger beside a populated src/ is a failure. "I found no problems" and "I found
nothing I understood" do not share an exit code here.
"""

import argparse
import pathlib
import sys
import tomllib

CLASSES = {"P1", "P2", "P3", "P4", "P5", "P6", "P7"}
# Tiers a module can owe. T0 (static), T0b (gate tests) and T9 (sanitizers) are
# universal and deliberately not declarable per module -- see the ledger header.
TIERS = {"T1", "T2", "T3", "T4", "T5", "T5b", "T6", "T7", "T8", "T10", "T11"}
REQUIRED_FIELDS = {"path", "classes", "tiers"}
OPTIONAL_FIELDS = {"fixtures", "note"}

# Where a tier's tests live. A tier is satisfied when at least one file exists
# under one of its directories; the mapping is deliberately coarse, because this
# gate checks that the promised tests EXIST, not that they are good. Mutation
# testing is what judges whether they are good.
TIER_DIRECTORIES = {
    "T1": ["tests/unit"],
    "T2": ["tests/unit", "tests/fixtures"],
    "T3": ["tests/oracle"],
    "T4": ["tests/property"],
    "T5": ["tests/fault_injection"],
    "T5b": ["tests/fault_injection", "tests/unit"],
    "T6": ["tests/determinism"],
    "T7": ["tests/supervision"],
    "T8": ["tests/integration", "tests/CMakeLists.txt"],
    "T10": ["tests/soak"],
    "T11": ["tests/hardware"],
}

failed = False


def fail(message: str) -> None:
    global failed
    print(f"  FAIL  {message}", file=sys.stderr)
    failed = True


def die(message: str) -> None:
    print(f"  FAIL  {message}", file=sys.stderr)
    print("\ntest-obligation check: FAILED", file=sys.stderr)
    sys.exit(1)


def source_modules(root: pathlib.Path) -> set[str]:
    """Every directory under src/ that holds source files.

    A directory of subdirectories is a grouping, not a module; only the ones
    that actually contain code owe tests.
    """
    src = root / "src"
    if not src.is_dir():
        return set()
    modules = set()
    for directory in src.rglob("*"):
        if not directory.is_dir():
            continue
        if any(child.suffix in {".cpp", ".hpp", ".in"} for child in directory.iterdir()):
            modules.add(directory.relative_to(root).as_posix())
    return modules


def tier_is_served(root: pathlib.Path, tier: str) -> bool:
    for candidate in TIER_DIRECTORIES[tier]:
        target = root / candidate
        if target.is_file():
            return True
        if target.is_dir() and any(p.is_file() for p in target.rglob("*")):
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "root",
        nargs="?",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    parser.add_argument("--ledger", type=pathlib.Path, help="path to obligations.toml")
    args = parser.parse_args()

    root = args.root.resolve()
    ledger_path = args.ledger or root / "tests" / "obligations.toml"

    if not ledger_path.exists():
        die(
            f"{ledger_path} is missing. The ledger is not optional: without it the "
            "project has no record of which execution paths each module was judged to have."
        )

    try:
        document = tomllib.loads(ledger_path.read_text(encoding="utf-8"))
    except (tomllib.TOMLDecodeError, UnicodeDecodeError) as error:
        die(f"{ledger_path} does not parse as TOML: {error}")

    unexpected = set(document) - {"module"}
    if unexpected:
        fail(f"unexpected top-level key(s) {sorted(unexpected)}; the only one is [[module]]")

    entries = document.get("module", [])
    if not isinstance(entries, list) or any(not isinstance(e, dict) for e in entries):
        die("'module' must be an array of tables ([[module]])")

    modules = source_modules(root)
    if not entries and modules:
        die(
            f"the ledger declares nothing while src/ holds {len(modules)} module(s). "
            "An empty ledger beside real code is a failure, not a clean sheet."
        )

    declared: dict[str, dict] = {}
    for index, entry in enumerate(entries):
        path = entry.get("path")
        if not isinstance(path, str) or not path:
            fail(f"entry #{index + 1}: 'path' must be a non-empty string")
            continue
        if path in declared:
            fail(f"{path}: declared twice; one record per module")
        declared[path] = entry

        missing = REQUIRED_FIELDS - entry.keys()
        if missing:
            fail(f"{path}: missing required field(s) {sorted(missing)}")
        unknown = entry.keys() - REQUIRED_FIELDS - OPTIONAL_FIELDS
        if unknown:
            fail(
                f"{path}: unknown field(s) {sorted(unknown)}; a misspelt field name "
                "silently disables the check that reads it"
            )

        classes = entry.get("classes")
        if not isinstance(classes, list) or not classes:
            fail(f"{path}: 'classes' must be a non-empty list of path classes")
        else:
            for name in classes:
                if name not in CLASSES:
                    fail(
                        f"{path}: {name!r} is not a path class; expected one of "
                        f"{sorted(CLASSES)} (docs/IMPLEMENTATION.md §2.2)"
                    )
            if "P1" not in classes:
                # Every module that runs at all has decision paths. Omitting P1
                # means the classification was not really done.
                fail(
                    f"{path}: does not declare P1. Every module has decision paths; "
                    "an entry without P1 has not been classified, only filled in."
                )

        tiers = entry.get("tiers")
        if not isinstance(tiers, list):
            fail(f"{path}: 'tiers' must be a list (empty means a deliberate exemption)")
        else:
            for tier in tiers:
                if tier not in TIERS:
                    fail(
                        f"{path}: {tier!r} is not a declarable tier; expected one of "
                        f"{sorted(TIERS)}. T0, T0b and T9 are universal and not declared."
                    )
                elif not tier_is_served(root, tier):
                    fail(
                        f"{path}: declares {tier}, but no tests exist under "
                        f"{TIER_DIRECTORIES[tier]}. A promised tier with no tests is a "
                        "promise, not a test."
                    )

        # P2 and P7 are both unreachable without something to force the state,
        # but they are forced by different means, and the first version of this
        # gate conflated them. P7 -- a telemetry source present, absent or
        # returning EACCES -- needs a hostile tree on disk. P2 -- an errno from a
        # syscall -- is forced through the substitutable Syscalls seam, and has
        # no on-disk fixture at all. Demanding one of a module like
        # src/platform/fs was wrong, and demanding it of every future P2 module
        # would have pushed people to invent fixtures that prove nothing.
        #
        # Found on this gate's first real use, which is the argument for landing
        # gates early rather than at the milestone that needs them.
        declared_classes = set(classes if isinstance(classes, list) else [])
        fixtures = entry.get("fixtures", [])
        if not isinstance(fixtures, list):
            fail(f"{path}: 'fixtures' must be a list")
            fixtures = []
        for fixture in fixtures:
            if not (root / "tests" / "fixtures" / str(fixture)).exists():
                fail(f"{path}: fixture 'tests/fixtures/{fixture}' does not exist")
        if "P7" in declared_classes and not fixtures:
            fail(
                f"{path}: declares P7 but names no fixtures. A capability state -- a "
                "source present, absent, or refusing permission -- is reachable only "
                "from a tree on disk that is in that state."
            )
        if "P2" in declared_classes and not ({"T2", "T5"} & set(tiers or [])):
            fail(
                f"{path}: declares P2 but owes neither T2 nor T5. An errno is reachable "
                "only by forcing it through the substitutable syscall seam, and those "
                "are the tiers where the forcing happens."
            )

    for path in sorted(modules - declared.keys()):
        fail(
            f"{path} holds source but is absent from {ledger_path.name}. "
            "Every module records which execution-path classes it contains."
        )
    for path in sorted(declared.keys() - modules):
        fail(
            f"{path} is in the ledger but holds no source. Remove the entry — a record "
            "that outlives its module is a judgement nobody is reviewing."
        )

    for path in sorted(declared):
        if not failed:
            entry = declared[path]
            print(
                f"  ok    {path}  classes={','.join(entry['classes'])}  "
                f"tiers={','.join(entry['tiers']) or '-'}"
            )

    print(f"\ntest-obligation check: {len(declared)} module(s), {'FAILED' if failed else 'ok'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
