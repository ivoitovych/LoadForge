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
  * every declared fixture exists on disk;
  * every process-shared or robust lock in src/ is declared, with the
    robustness test that kills its holder and proves a survivor recovers via
    EOWNERDEAD (docs/IMPLEMENTATION.md §4, M1's second new gate).

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
OPTIONAL_FIELDS = {"fixtures", "fixture_builders", "note", "process_shared_locks"}

# A file named as a fixture_builder must SAY it is one. Without a marker the
# check degenerates into "some file exists", which is the vacuous shape this
# gate has already shipped three times (see TIER_FILE_MARKERS). The marker is
# something an author writes deliberately and a reviewer can grep for; it cannot
# appear by accident in a file that builds no hostile tree.
FIXTURE_BUILDER_MARKER = "LOADFORGE P7 FIXTURE BUILDER"

# Where a tier's tests live. A tier is satisfied when at least one non-empty
# directory exists among its locations; the mapping is deliberately coarse,
# because this gate checks that the promised tests EXIST, not that they are
# good. Mutation testing is what judges whether they are good.
TIER_DIRECTORIES = {
    "T1": ["tests/unit"],
    "T2": ["tests/unit", "tests/fixtures"],
    "T3": ["tests/oracle"],
    "T4": ["tests/property"],
    "T5": ["tests/fault_injection"],
    "T5b": ["tests/fault_injection", "tests/unit"],
    "T6": ["tests/determinism"],
    "T7": ["tests/supervision"],
    "T8": ["tests/integration"],
    "T10": ["tests/soak"],
    "T11": ["tests/hardware"],
}

# Some tiers are not a directory of test files. T8 drives the built binary, and
# its cases are add_test() calls in tests/CMakeLists.txt.
#
# The first version listed that file as a T8 location, which made the check
# vacuous: the file always exists, so `tests/CMakeLists.txt` is a file → served
# → T8 could never fail. A gate condition that cannot fail is not a gate, and
# this project has now shipped that shape three times (the mutation gate on an
# unreadable report, the SPDX gate narrower than its name, and this). Requiring
# a marker INSIDE the file makes the check able to fail again.
TIER_FILE_MARKERS = {
    "T8": [("tests/CMakeLists.txt", "add_test(")],
}

# THE EOWNERDEAD OBLIGATION, PER LOCK
# -----------------------------------
# docs/PLAN.md §4.6: a worker can die holding a process-shared lock, and unless
# the lock is robust and every acquirer handles EOWNERDEAD, one dead worker
# deadlocks every survivor. IMPLEMENTATION §4 turns that into an obligation per
# lock -- kill the holder mid-critical-section, assert a survivor recovers --
# and this gate makes the obligation impossible to forget: a source file that
# uses one of these primitives must be named by a declared lock, and each
# declared lock must name its robustness test.
#
# Matched as written in the raw text, comments included. A comment that names
# one of these is flagged too, and that is the safe side to err on: an
# undeclared lock passing because it sat next to a comment-stripping bug would
# be the failure this gate exists to prevent. A lock hand-rolled from atomics
# in shared memory has no name to match; the design's answer to that is to have
# none (the heartbeat region is a seqlock, docs/JOURNAL.md §2.15), and review's.
LOCK_PRIMITIVES = (
    "PTHREAD_PROCESS_SHARED",
    "PTHREAD_MUTEX_ROBUST",
    "pthread_mutexattr_setpshared",
    "pthread_mutexattr_setrobust",
    "pthread_mutex_consistent",
    "pthread_rwlockattr_setpshared",
    "pthread_condattr_setpshared",
    "pthread_barrierattr_setpshared",
    "pthread_spin_init",
    "sem_init",
    "sem_open",
    "SYS_futex",
    "FUTEX_WAIT",
    "interprocess_mutex",
)
LOCK_FIELDS = {"name", "file", "robustness_test"}
# The test that discharges the obligation kills a real holder: a supervision
# test (T7), where processes are really killed (IMPLEMENTATION §2.3, P6). And it
# must say which lock it is for, so one test cannot be named for every lock.
ROBUSTNESS_TEST_DIRECTORY = "tests/supervision"
ROBUSTNESS_TEST_MARKER = "LOADFORGE EOWNERDEAD TEST:"
SOURCE_SUFFIXES = {".cpp", ".hpp", ".in"}

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


def lock_uses(root: pathlib.Path) -> dict[str, list[str]]:
    """Every source file under src/ that names a lock primitive, and which ones.

    A file that cannot be read is a failure, not a file without locks.
    """
    uses: dict[str, list[str]] = {}
    src = root / "src"
    if not src.is_dir():
        return uses
    for path in sorted(src.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(root).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            fail(f"{relative}: cannot be read to look for process-shared locks: {error}")
            continue
        found = [primitive for primitive in LOCK_PRIMITIVES if primitive in text]
        if found:
            uses[relative] = found
    return uses


def marked_locks(test: pathlib.Path) -> set[str]:
    """The lock names a robustness test declares itself for, one per marker line.
    Whole names, so a test for "region lock two" is not one for "region lock"."""
    names = set()
    for line in test.read_text(encoding="utf-8", errors="replace").splitlines():
        if ROBUSTNESS_TEST_MARKER in line:
            names.add(line.split(ROBUSTNESS_TEST_MARKER, 1)[1].strip())
    return names


def check_locks(root: pathlib.Path, declared: dict[str, dict]) -> str:
    """The EOWNERDEAD obligation: every lock found is declared, every declared
    lock is found and has its robustness test. Returns the summary line."""
    uses = lock_uses(root)
    named: dict[str, list[str]] = {}
    count = 0
    for path, entry in sorted(declared.items()):
        locks = entry.get("process_shared_locks", [])
        if not isinstance(locks, list) or any(not isinstance(lock, dict) for lock in locks):
            fail(f"{path}: 'process_shared_locks' must be a list of tables")
            continue
        for lock in locks:
            count += 1
            if set(lock) != LOCK_FIELDS or not all(
                isinstance(lock[field], str) and lock[field] for field in LOCK_FIELDS
            ):
                fail(
                    f"{path}: a process-shared lock needs exactly {sorted(LOCK_FIELDS)}, "
                    f"each a non-empty string; got {sorted(lock)}"
                )
                continue
            name, file, test = lock["name"], lock["file"], lock["robustness_test"]
            named.setdefault(file, []).append(name)
            if pathlib.PurePosixPath(file).parent.as_posix() != path:
                fail(f"{path}: lock '{name}' is in {file}, which is not this module's")
            elif file not in uses:
                fail(
                    f"{path}: lock '{name}' is declared in {file}, which uses no "
                    "process-shared lock primitive. Remove the entry: a declaration "
                    "that outlives its lock is a judgement nobody is reviewing."
                )
            if "P5" not in entry.get("classes", []):
                fail(
                    f"{path}: holds process-shared lock '{name}' but does not declare P5. "
                    "A lock two processes share is a concurrency path."
                )
            target = root / test
            if not test.startswith(ROBUSTNESS_TEST_DIRECTORY + "/") or not target.is_file():
                fail(
                    f"{path}: lock '{name}' names robustness test '{test}', which is not a "
                    f"file under {ROBUSTNESS_TEST_DIRECTORY}/. The test kills a real holder "
                    "mid-critical-section, which is the supervision tier's work."
                )
            elif name not in marked_locks(target):
                fail(
                    f"{path}: robustness test '{test}' does not carry "
                    f"'{ROBUSTNESS_TEST_MARKER} {name}'. A test that does not say which "
                    "lock it kills the holder of is not evidence that any lock recovers."
                )
    for file, primitives in sorted(uses.items()):
        if file not in named:
            fail(
                f"{file} uses {', '.join(primitives)} and declares no process-shared lock "
                "in the ledger. Every process-shared lock owes a robustness test: kill the "
                "holder mid-critical-section and prove a survivor recovers via EOWNERDEAD "
                "(docs/PLAN.md §4.6, docs/IMPLEMENTATION.md §4)."
            )
    if not uses and not count:
        return (
            "process-shared locks: none in src/ and none declared, so no EOWNERDEAD "
            "obligation is owed (docs/PLAN.md §4.6 prefers designs with none)"
        )
    return f"process-shared locks: {len(uses)} source file(s) use one, {count} lock(s) declared"


def tier_is_served(root: pathlib.Path, tier: str) -> bool:
    for candidate in TIER_DIRECTORIES.get(tier, []):
        target = root / candidate
        if target.is_dir() and any(p.is_file() for p in target.rglob("*")):
            return True
    for relative, marker in TIER_FILE_MARKERS.get(tier, []):
        target = root / relative
        if target.is_file() and marker in target.read_text(encoding="utf-8", errors="replace"):
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
                    where = TIER_DIRECTORIES.get(tier, []) + [
                        f"{f} containing {m!r}" for f, m in TIER_FILE_MARKERS.get(tier, [])
                    ]
                    fail(
                        f"{path}: declares {tier}, but no tests exist under "
                        f"{where}. A promised tier with no tests is a "
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

        # A CHECKED-IN TREE IS ONE WAY TO PUT A SOURCE IN A STATE, NOT THE ONLY ONE
        # ------------------------------------------------------------------------
        # The original rule demanded `fixtures`, i.e. a tree committed under
        # tests/fixtures/. That is right for the states a tree can hold -- a
        # malformed value, an unexpected layout, a dangling symlink -- and wrong
        # for the rest, because some capability states CANNOT be committed:
        #
        #   * "vanishes mid-run" is an event, not a state. No static tree can
        #     hold it; only a test that reads a file and then removes it.
        #   * a mode-000 file does not survive a clone. git records the execute
        #     bit and nothing else, so a committed EACCES fixture would arrive
        #     readable and the test would pass by reading it.
        #
        # So the rule now accepts a named BUILDER: a test file that constructs
        # the hostile state at runtime. That is not a loophole -- the builder
        # must exist and must carry the marker, so naming an arbitrary file does
        # not satisfy it -- and refusing it would have forced exactly the
        # prove-nothing fixture this gate's P2 comment already warns about.
        builders = entry.get("fixture_builders", [])
        if not isinstance(builders, list):
            fail(f"{path}: 'fixture_builders' must be a list")
            builders = []
        for builder in builders:
            target = root / str(builder)
            if not target.is_file():
                fail(f"{path}: fixture builder '{builder}' does not exist")
            elif FIXTURE_BUILDER_MARKER not in target.read_text(encoding="utf-8", errors="replace"):
                fail(
                    f"{path}: fixture builder '{builder}' does not carry "
                    f"'{FIXTURE_BUILDER_MARKER}'. A file that does not say it builds "
                    "hostile trees is not evidence that anything builds one."
                )
        if "P7" in declared_classes and not fixtures and not builders:
            fail(
                f"{path}: declares P7 but names neither fixtures nor fixture_builders. "
                "A capability state -- a source present, absent, refusing permission, "
                "or vanishing mid-run -- is reachable only from a tree on disk that is "
                "in that state, committed or constructed."
            )
        if "P2" in declared_classes and not ({"T2", "T5"} & set(tiers or [])):
            fail(
                f"{path}: declares P2 but owes neither T2 nor T5. An errno is reachable "
                "only by forcing it through the substitutable syscall seam, and those "
                "are the tiers where the forcing happens."
            )
        # P6 had no rule at all until a module first declared it, which meant the
        # declaration was an unenforced promise -- the shape this whole gate
        # exists to prevent. A lifecycle path is a process starting, dying
        # abnormally, being restarted or torn down, and none of those is reachable
        # by calling a function and looking at what it returned: it needs the
        # state forced through the seam (T2), a supervision harness (T7), or a
        # real process to actually create and reap (T8).
        if "P6" in declared_classes and not ({"T2", "T7", "T8"} & set(tiers or [])):
            fail(
                f"{path}: declares P6 but owes none of T2, T7 or T8. A lifecycle path -- "
                "a process starting, dying abnormally, restarting, being torn down -- "
                "is reachable only by forcing the state through the seam, by a "
                "supervision harness, or against a real process."
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

    locks = check_locks(root, declared)

    for path in sorted(declared):
        if not failed:
            entry = declared[path]
            print(
                f"  ok    {path}  classes={','.join(entry['classes'])}  "
                f"tiers={','.join(entry['tiers']) or '-'}"
            )

    print(f"\n{locks}")
    print(f"test-obligation check: {len(declared)} module(s), {'FAILED' if failed else 'ok'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
