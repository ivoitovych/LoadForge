#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The mutation sweep's shard plan, and what each shard runs Mull with.

WHY SHARDS
----------
The first sweep over the whole project ran as one job on GitHub's runners and
was cancelled at the six-hour job limit with no report at all (docs/JOURNAL.md
§1.26 has the measurements and §2.28 the decisions). So the sweep runs as one CI
job per shard, each mutating only its own sources and judged on its own by
tools/mutation-gate.py --scope.

THE PLAN
--------
One shard per top-level module under src/ that has a source file of its own,
derived from the tree on every run rather than listed, so a new module is swept
from its first commit and none can be forgotten. A module with headers only --
the contracts the streams implement -- has code in the test binary only where
another module instantiates it, and a shard of its own would sweep nothing and
fail as an empty run; its headers are swept in the shard named by
HEADER_ONLY_HOST until it gains a source file, when it becomes a shard of its
own. Every module is in exactly one shard, and every allowlist entry must fall
in some shard's scope -- an entry no shard owns would never be applied and never
found stale, a permission nobody would ever review again.

THE TESTS EACH MUTANT RUNS
--------------------------
Every mutant runs the in-process unit tests, and not the tests that create a
process. Those are named RealProcess* (GTEST_FILTER below), and the name is
checked against what each test really does (check-processes): the fast set must
create no process at all, and every RealProcess* test must create one.

A process the test program forks is the one thing Mull's own timeout cannot
contain. Mull kills only the program it started, then reads that program's
output to the end; a child that a mutant leaves alive -- a broken kill, a broken
reap, a fork whose two sides swap roles -- holds the pipe open and Mull waits on
it for ever. The gate also stops Mull capturing a mutant's output, which closes
that particular wait, but a mutant whose test forks can still leave a child
running into every mutant after it, and can send the parent down the child's
path to exit 0 mid-suite. Neither is a measurement of anything.

Subcommands:
    plan                    the shards, as a JSON list for a CI matrix
    scope SHARD             the shard's repository-relative paths, one per line
    targets SHARD           the test executables that hold the shard's tests, one per line
    unit-targets --scope P [--scope P ...] [--exclude E ...]
                            the same for a unit of the CI plan: paths and what it leaves out
    mull-config SHARD       the shard's mull.yml, on standard output
    gtest-filter            the filter every mutant's test run is given
    check-processes BINARY  run the test binary and check the filter against it
"""

import argparse
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Mull's default operator set, named rather than left to Mull's default, so that
# a release that changes the default changes nothing here without a review.
MUTATORS = ["cxx_default"]

# Modules with no source file of their own are swept here (see THE PLAN).
HEADER_ONLY_HOST = "core"

# The tests that create a process. GoogleTest matches the pattern against
# Suite.Test, and a leading '-' makes the whole filter "everything except".
PROCESS_PATTERN = "RealProcess*"
GTEST_FILTER = "-" + PROCESS_PATTERN

SOURCE_SUFFIXES = (".cpp", ".cc", ".c")

# Characters a Rust regex (Mull's include paths) treats as syntax.
REGEX_META = set("\\.+*?()|[]{}^$#&-~")


class ShardFailure(Exception):
    """Every refusal, so that no path can fall through to success."""


def _gate():
    """tools/mutation-gate.py, for its allowlist reader: one parser of keys."""
    sys.dont_write_bytecode = True  # no __pycache__ left in tools/
    spec = importlib.util.spec_from_file_location("mutation_gate", ROOT / "tools/mutation-gate.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def modules(root: pathlib.Path) -> list[str]:
    src = root / "src"
    if not src.is_dir():
        raise ShardFailure(f"{src} does not exist; there is nothing to shard.")
    found = sorted(entry.name for entry in src.iterdir() if entry.is_dir())
    if not found:
        raise ShardFailure(f"{src} holds no module directory; a plan of no shards sweeps nothing.")
    return found


def has_sources(directory: pathlib.Path) -> bool:
    return any(path.suffix in SOURCE_SUFFIXES for path in directory.rglob("*") if path.is_file())


def plan(root: pathlib.Path) -> dict[str, list[str]]:
    """Shard name -> repository-relative paths it sweeps. Every module in
    exactly one shard, by construction and then by check."""
    names = modules(root)
    shards = {name: [f"src/{name}"] for name in names if has_sources(root / "src" / name)}
    if HEADER_ONLY_HOST not in shards:
        raise ShardFailure(
            f"src/{HEADER_ONLY_HOST}, which hosts the header-only modules, has no source "
            "file; choose another HEADER_ONLY_HOST rather than leave those headers unswept."
        )
    for name in names:
        if name not in shards:
            shards[HEADER_ONLY_HOST].append(f"src/{name}")
    owners = [path for paths in shards.values() for path in paths]
    if sorted(owners) != sorted(f"src/{name}" for name in names):
        raise ShardFailure(f"the plan does not hold every module exactly once: {shards}")
    return shards


def check_allowlist(root: pathlib.Path, shards: dict[str, list[str]]) -> int:
    gate = _gate()
    try:
        entries = gate.load_allowlist(root / "tools" / "mutation-allowlist.txt")
    except gate.GateFailure as failure:
        raise ShardFailure(str(failure)) from failure
    scopes = [path for paths in shards.values() for path in paths]
    orphans = [key for key in entries if not gate.in_scope(gate.key_path(key), scopes)]
    if orphans:
        raise ShardFailure(
            f"allowlist entr{'y' if len(orphans) == 1 else 'ies'} {orphans} lie in no shard's "
            "scope, so no shard would ever apply them or find them stale. Remove them, or "
            "move the code they name back under src/."
        )
    return len(entries)


def shard(root: pathlib.Path, name: str) -> list[str]:
    shards = plan(root)
    if name not in shards:
        raise ShardFailure(f"no shard named {name!r}; the plan is {sorted(shards)}")
    return shards[name]


def rust_regex_escape(text: str) -> str:
    if any(ord(char) < 0x20 or ord(char) > 0x7E for char in text):
        raise ShardFailure(
            f"{text!r} holds a character this tool will not put in a regular expression; "
            "check the repository out under a plainer path."
        )
    return "".join("\\" + char if char in REGEX_META else char for char in text)


def mull_config(root: pathlib.Path, name: str) -> str:
    """The shard's mull.yml. Each include path is anchored at the absolute
    checkout path, because Mull matches it, unanchored, against the absolute
    path the compiler recorded; '^.../src/core/' cannot also match a src/core/
    that some dependency happens to ship. Read by the compiler plugin, which
    then builds in only this shard's mutants, and by mull-runner, which then
    runs only those -- two filters, so a plugin that misses it still cannot make
    the runner sweep the wrong code unnoticed: the gate refuses any mutant
    outside the shard."""
    paths = shard(root, name)
    lines = [
        "# Generated by tools/mutation-shard.py for the "
        f"{name} shard; not a file to edit or commit.",
        "mutators:",
        *(f"  - {json.dumps(mutator)}" for mutator in MUTATORS),
        "includePaths:",
    ]
    base = rust_regex_escape(str(root.resolve()))
    for path in paths:
        # JSON strings are YAML double-quoted scalars with the same escapes.
        lines.append(f"  - {json.dumps('^' + base + '/' + rust_regex_escape(path) + '/')}")
    return "\n".join(lines) + "\n"


# ---- targets -------------------------------------------------------------------

# Test directories the sweep does not run. The supervision and integration tiers
# start processes and talk to the real kernel by design (the header says why a
# mutant must not run those), the soak tier runs for as long as a soak runs, and
# the rest hold no tests of a module of their own.
NOT_MODULE_TESTS = (
    "supervision",
    "integration",
    "soak",
    "support",
    "tools",
    "fixtures",
    "contracts",
)
UNIT_TESTS_TARGET = "loadforge_unit_tests"
SUITE_RE = re.compile(r"\bloadforge_test_suite\(\s*(?P<name>\w+)")


def targets(root: pathlib.Path, name: str) -> list[str]:
    """The test executables whose tests exercise a shard's code, by CMake target."""
    return targets_of(root, shard(root, name), [], repr(name) + " shard")


def under(path: str, base: str) -> bool:
    return path == base or path.startswith(base + "/")


def targets_of(
    root: pathlib.Path, paths: list[str], excludes: list[str], label: str
) -> list[str]:
    """The test executables whose tests exercise the code under `paths`, less
    what is under `excludes`, by CMake target.

    A mutant is compiled into every executable that links the code it sits in,
    and killed by whichever executable's tests detect it, so the sweep of a
    unit runs every executable that holds tests of the unit's modules, not the
    one binary M1 had (M2a exit criterion 6). A test belongs to a module by
    where it lives: tests/<tier>/<module>/..., whose CMakeLists.txt defines a
    suite (loadforge_test_suite) or adds sources to the unit-test executable
    (target_sources), and tests/CMakeLists.txt's own list of unit/<module>/
    sources for the modules that predate fragments. The module is a path under
    src/ (a unit may be a fragment below a top-level module, as a unit that
    owns a test program of its own is), and what a unit leaves out to other
    units (`excludes`) is not its tests. An executable that is not returned has
    no test of this unit's, and would have no mutant to run.
    """
    scopes = [path.removeprefix("src/") for path in paths]
    skipped = [path.removeprefix("src/") for path in excludes]

    def mine(module: str) -> bool:
        return any(under(module, scope) for scope in scopes) and not any(
            under(module, other) for other in skipped
        )

    found: list[str] = []

    def add(target: str) -> None:
        if target not in found:
            found.append(target)

    tests = root / "tests"
    for cmake in sorted(tests.rglob("CMakeLists.txt")):
        parts = cmake.relative_to(tests).parts[:-1]
        if len(parts) < 2 or parts[0] in NOT_MODULE_TESTS or not mine("/".join(parts[1:])):
            continue
        text = cmake.read_text(encoding="utf-8")
        for match in SUITE_RE.finditer(text):
            add(match["name"])
        if f"target_sources({UNIT_TESTS_TARGET}" in text:
            add(UNIT_TESTS_TARGET)
    top = tests / "CMakeLists.txt"
    if top.exists():
        listed = top.read_text(encoding="utf-8")
        for match in re.finditer(r"^\s*unit/(?P<dir>[\w/]+)/[\w.]+", listed, re.M):
            if mine(match["dir"]):
                add(UNIT_TESTS_TARGET)
    if not found:
        raise ShardFailure(
            f"no test executable holds a test of the {label} ({', '.join(paths)}); "
            "every mutant in it would be run against nothing and survive."
        )
    return found


# ---- check-processes -----------------------------------------------------------

PROCESS_CALL_RE = re.compile(r"^\d+\s+(?P<call>fork|vfork|clone|clone3)\(")
TESTS_RAN_RE = re.compile(r"^\[==========\] (?P<n>\d+) tests? from \d+ test suites? ran\.", re.M)


def processes_created(binary: str, gtest_filter: str) -> tuple[int, str]:
    """Run the binary under strace and count the processes it creates.

    A thread is a clone carrying CLONE_THREAD; anything else that fork, vfork,
    clone or clone3 starts is a process. Returns (count, the program's output).
    """
    strace = shutil.which("strace")
    if strace is None:
        raise ShardFailure(
            "strace is not installed, so the gate cannot see whether the tests create "
            "processes. Install it; a check that cannot look is not a check that passed."
        )
    with tempfile.TemporaryDirectory() as work:
        trace = pathlib.Path(work) / "trace"
        completed = subprocess.run(
            [
                strace,
                "-f",
                "-qq",
                "-o",
                str(trace),
                "-e",
                "trace=fork,vfork,clone,clone3",
                binary,
                f"--gtest_filter={gtest_filter}",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        output = completed.stdout + completed.stderr
        if completed.returncode != 0:
            raise ShardFailure(
                f"{binary} --gtest_filter={gtest_filter} exited {completed.returncode} under "
                f"strace; the check needs the suite to pass. Its output ends:\n{output[-2000:]}"
            )
        if not trace.exists():
            raise ShardFailure("strace wrote no trace, so it saw nothing; refusing to guess.")
        count = 0
        for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
            match = PROCESS_CALL_RE.match(line)
            if match and "CLONE_THREAD" not in line:
                count += 1
    return count, output


def tests_ran(output: str) -> int:
    found = TESTS_RAN_RE.findall(output)
    if len(found) != 1:
        raise ShardFailure(
            "the test program did not report how many tests ran, so the check cannot tell "
            "a suite that created no process from one that ran nothing."
        )
    return int(found[0])


def check_processes(binary: str) -> None:
    """The name RealProcess* must mean exactly 'creates a process'.

    One way round is what makes the sweep safe: the set every mutant runs creates
    no process at all. The other keeps the name honest, so it cannot become a
    place to put a slow or awkward test that would otherwise kill mutants: every
    RealProcess* test creates one, unless it skipped itself on this machine, which
    is reported rather than judged.
    """
    created, output = processes_created(binary, GTEST_FILTER)
    ran = tests_ran(output)
    if ran == 0:
        raise ShardFailure(
            f"--gtest_filter={GTEST_FILTER} ran no test at all; mutants run against nothing "
            "would all survive, or worse, a filter this wrong would be the next mistake."
        )
    if created:
        raise ShardFailure(
            f"the {ran} test(s) every mutant runs created {created} process(es). Name each "
            f"test that creates a process {PROCESS_PATTERN} -- strace -f -e "
            "trace=fork,vfork,clone,clone3 on one test at a time finds it."
        )
    print(f"mutation shard: the {ran} test(s) every mutant runs create no process")

    listed = subprocess.run(
        [binary, "--gtest_list_tests", f"--gtest_filter={PROCESS_PATTERN}"],
        capture_output=True,
        text=True,
        check=False,
    )
    if listed.returncode != 0:
        raise ShardFailure(f"{binary} --gtest_list_tests exited {listed.returncode}")
    names, suite = [], None
    for line in listed.stdout.splitlines():
        if not line.strip() or line.startswith("Note:"):
            continue
        if not line.startswith(" "):
            suite = line.split()[0]
        else:
            names.append(f"{suite}{line.split()[0]}")
    honest, skipped, idle = 0, [], []
    for name in names:
        count, output = processes_created(binary, name)
        if "[  SKIPPED ]" in output:
            skipped.append(name)
        elif count == 0:
            idle.append(name)
        else:
            honest += 1
    for name in skipped:
        print(f"  skipped here, so not checked: {name}")
    if idle:
        raise ShardFailure(
            f"{len(idle)} test(s) named {PROCESS_PATTERN} create no process: {idle}. The name "
            "leaves a test out of the mutation sweep, so only a test that needs leaving out "
            "may carry it; rename these."
        )
    print(
        f"mutation shard: {honest} {PROCESS_PATTERN} test(s) each create a process, "
        f"{len(skipped)} skipped here"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT, help="repository root")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("plan", help="the shards, as a JSON list")
    scope_parser = sub.add_parser("scope", help="a shard's paths, one per line")
    scope_parser.add_argument("shard")
    config_parser = sub.add_parser("mull-config", help="a shard's mull.yml")
    config_parser.add_argument("shard")
    targets_parser = sub.add_parser("targets", help="a shard's test executables, one target a line")
    targets_parser.add_argument("shard")
    unit_targets_parser = sub.add_parser(
        "unit-targets", help="a unit's test executables, one target a line"
    )
    unit_targets_parser.add_argument("--scope", action="append", required=True)
    unit_targets_parser.add_argument("--exclude", action="append", default=[])
    sub.add_parser("gtest-filter", help="the filter every mutant's tests run with")
    check_parser = sub.add_parser("check-processes", help="check the filter against a binary")
    check_parser.add_argument("binary")
    args = parser.parse_args()
    root = args.root.resolve()

    try:
        if args.command == "plan":
            shards = plan(root)
            entries = check_allowlist(root, shards)
            for name, paths in shards.items():
                print(f"  {name}: {' '.join(paths)}", file=sys.stderr)
            print(
                f"mutation shard: {len(shards)} shard(s); {entries} allowlist entr"
                f"{'y' if entries == 1 else 'ies'}, each in a shard",
                file=sys.stderr,
            )
            print(json.dumps(list(shards)))
        elif args.command == "scope":
            print("\n".join(shard(root, args.shard)))
        elif args.command == "mull-config":
            sys.stdout.write(mull_config(root, args.shard))
        elif args.command == "targets":
            print("\n".join(targets(root, args.shard)))
        elif args.command == "unit-targets":
            print("\n".join(targets_of(root, args.scope, args.exclude, "unit")))
        elif args.command == "gtest-filter":
            print(GTEST_FILTER)
        else:
            check_processes(args.binary)
    except ShardFailure as failure:
        print(f"mutation shard: FAILED — {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
