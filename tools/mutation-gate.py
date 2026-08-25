#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The mutation gate: ZERO UNEXPLAINED SURVIVING MUTANTS (docs/PLAN.md §7.3).

Not a percentage. A mutation-score threshold licenses a standing population of
unexamined survivors, which is the same defect the 100% coverage rule avoids.
Every survivor is either killed by a better test, or listed in
tools/mutation-allowlist.txt with a written justification.

WHY THIS IS PYTHON AND READS TWO REPORTS
----------------------------------------
The previous gate was shell, and scraped Mull's human-readable IDE output with a
regular expression modelled on a fixture somebody wrote by hand. It was wrong.
Mull's IDEReporter emits (lib: rust/mull-reporters/src/ide_reporter.rs):

    {path}:{line}:{col}: warning: {label}: {message} [{mutator}]

with the mutator in the trailing brackets and the message shaped
"Replaced X with Y". The old parser captured the first word after "Survived:",
which on real output is the word "Replaced" — so every mutant at one source
location collapsed to the same key, and one reviewed allowlist entry would have
excused all of them. The protection the key was widened to provide was absent
in exactly the case it existed for.

Every fixture test passed throughout, because the fixtures modelled the tool
incorrectly in the same way the parser did. That is the lesson worth keeping:
a test suite written against your own idea of an external tool cannot tell you
that your idea is wrong. The fixtures under tests/tools/fixtures/mull/ are now
taken from Mull's own integration tests, and tests/tools/mutation_integration.sh
runs the real binary end to end.

So this gate reads TWO renderings of the same run and requires them to agree:

    IDE    human-readable text — carries the totals (killed/survived/score)
    Sarif  SARIF 2.1.0 JSON   — carries machine-readable ruleId per result

Neither alone is sufficient. SARIF omits killed mutants entirely, so an empty
SARIF file cannot be told apart from a crashed run; the IDE text has totals but
must be parsed out of prose. Requiring both to describe the same set of mutants
is the same doctrine the project applies to its workloads: never verify
something by re-reading the one artifact it produced. Any disagreement between
the two is a failure of the gate, not a pass (docs/PLAN.md F20).
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys

# All four shapes below are taken from Mull 0.34.0's
# rust/mull-reporters/src/ide_reporter.rs, not from a guess at its output.
#
# Mutant records go through diag_raw!, which writes the line with NO prefix.
# Headers and the score go through diag_info!, which prefixes "[info] ". Getting
# that backwards is not cosmetic: an anchored-but-lazy path group would swallow
# the prefix into the filename, and every allowlist key would carry it.
#
# The message quotes source text and may itself contain brackets, so the mutator
# is the LAST bracketed group on the line and may not contain brackets.
RECORD_RE = re.compile(
    r"^(?P<path>\S.*?):(?P<line>\d+):(?P<col>\d+): warning: "
    r"(?P<label>Killed|Survived|Not Covered): (?P<message>.*) \[(?P<mutator>[^\[\]]+)\]$"
)
INFO = r"^(?:\[info\] )?"
HEADER_RE = re.compile(
    INFO + r"(?P<label>Killed|Survived|Not Covered) mutants \((?P<n>\d+)/(?P<total>\d+)\):"
)
SCORE_RE = re.compile(INFO + r"Mutation score: (?P<score>\d+)%")
ALL_KILLED_RE = re.compile(INFO + r"All mutations have been killed")
# Mull colours the "[info]" prefix when writing to a terminal. The file report is
# written uncoloured, but stripping is cheap and a coloured report must not read
# as an unparseable one.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
# Any line that announces a mutant must parse. Catching these separately is how
# the gate notices that its own regex has fallen behind Mull's output.
ANNOUNCE_RE = re.compile(r": warning: (?:Killed|Survived|Not Covered):")

LABELS = ("Killed", "Survived", "Not Covered")


class GateFailure(Exception):
    """Raised for every refusal, so that no code path can fail open by falling
    through to the end of the script."""


def relative(path: str, root: pathlib.Path) -> str:
    """Allowlist keys must survive moving between machines.

    Mull reports the path the compiler saw, which in CI is an absolute path
    under the runner's workspace. Keying the allowlist on that would make every
    reviewed entry valid on exactly one machine.
    """
    try:
        return pathlib.PurePath(path).relative_to(root).as_posix()
    except ValueError:
        return pathlib.PurePath(path).as_posix()


def parse_ide_report(text: str, root: pathlib.Path) -> dict:
    """Parse the IDE text report, insisting the parse is total."""
    records = {label: [] for label in LABELS}
    headers: dict[str, tuple[int, int]] = {}
    score = None
    all_killed = False
    unparsed = []

    for raw_line in text.splitlines():
        line = ANSI_RE.sub("", raw_line.rstrip("\r"))
        match = RECORD_RE.match(line)
        if match:
            records[match["label"]].append(
                {
                    "key": f"{relative(match['path'], root)}:{match['line']}:{match['col']}"
                    f":{match['mutator']}",
                    "path": relative(match["path"], root),
                    "line": int(match["line"]),
                    "col": int(match["col"]),
                    "mutator": match["mutator"],
                }
            )
            continue
        if ANNOUNCE_RE.search(line):
            # A mutant was announced in a shape this parser does not recognise.
            # An unparsed mutant is an unexamined mutant, not an absent one.
            unparsed.append(line)
            continue
        header = HEADER_RE.match(line)
        if header:
            if header["label"] in headers:
                raise GateFailure(
                    f"the IDE report states the {header['label']!r} header twice; "
                    "refusing to guess which one counts"
                )
            headers[header["label"]] = (int(header["n"]), int(header["total"]))
            continue
        found_score = SCORE_RE.match(line)
        if found_score:
            score = int(found_score["score"])
            continue
        if ALL_KILLED_RE.match(line):
            all_killed = True

    if unparsed:
        raise GateFailure(
            f"{len(unparsed)} mutant line(s) in the IDE report did not parse. The first is:\n"
            f"    {unparsed[0]}\n"
            "  Mull's output format has changed, or this parser was never right. Either way "
            "the gate cannot pronounce on a report it has only partly read. Fix RECORD_RE in "
            "tools/mutation-gate.py against the pinned Mull's ide_reporter.rs."
        )
    return {"records": records, "headers": headers, "score": score, "all_killed": all_killed}


def reconcile_ide(report: dict) -> None:
    """Demand internal consistency of every total the report states.

    Mull prints a header per non-empty category, a score always, and
    "All mutations have been killed" when nothing survived. Each of those is an
    independent claim about the same run; wherever two of them are present they
    must agree, or the gate is reading something it does not understand.
    """
    records, headers, score = report["records"], report["headers"], report["score"]

    if score is None and not report["all_killed"]:
        raise GateFailure(
            "no Mull summary line in the report. It carries no evidence that a mutation "
            "run completed, so there is nothing to certify. A truncated, empty or "
            "unrecognised report is a FAILURE, never a clean sweep."
        )

    for label in LABELS:
        counted = len(records[label])
        if label in headers:
            stated = headers[label][0]
            if stated != counted:
                raise GateFailure(
                    f"the report says {stated} {label!r} mutant(s) but lists {counted}. "
                    "Either it is truncated or this parser is misreading it; both are failures."
                )
        elif counted:
            raise GateFailure(
                f"{counted} {label!r} mutant(s) are listed with no {label!r} header. "
                "The report is not shaped the way this gate understands it."
            )

    totals = {total for _, total in headers.values()}
    if len(totals) > 1:
        raise GateFailure(
            f"the report's headers disagree about how many mutants were generated: "
            f"{sorted(totals)}. Refusing to certify an incoherent report."
        )
    if not totals:
        # Every category empty: only legal when everything was killed and the
        # killed list was suppressed. Mull says so explicitly in that case.
        if not report["all_killed"] or score != 100:
            raise GateFailure(
                "the report lists no mutants at all and does not state that all mutations "
                "were killed. A run that generated nothing is not a clean sweep."
            )
        return

    total = totals.pop()
    if total == 0:
        raise GateFailure(
            "the report says zero mutants were generated. Mutation testing that mutated "
            "nothing measures nothing, so this is a failure, not a pass."
        )

    counts = {label: len(records[label]) for label in LABELS}
    if "Killed" in headers:
        accounted = sum(counts.values())
        if accounted != total:
            raise GateFailure(
                f"{accounted} mutant(s) accounted for ({counts}) out of {total} generated. "
                f"{total - accounted} went unreported, so the gate has not seen the whole run."
            )
        killed = counts["Killed"]
    else:
        # Killed mutants were not printed; derive them and check against the score.
        killed = total - counts["Survived"] - counts["Not Covered"]
        if killed < 0:
            raise GateFailure(
                f"more mutants are listed as survived or not-covered than the {total} "
                "the report says were generated. Refusing to certify an incoherent report."
            )

    if score is not None:
        expected = killed * 100 // total  # integer division, as Mull computes it
        if score != expected:
            raise GateFailure(
                f"the report states a mutation score of {score}% but {killed} killed of "
                f"{total} is {expected}%. The score and the mutant lists describe different runs."
            )
    if report["all_killed"] and (counts["Survived"] or counts["Not Covered"]):
        raise GateFailure(
            "the report says all mutations were killed while listing "
            f"{counts['Survived']} survivor(s) and {counts['Not Covered']} not-covered mutant(s)."
        )


def parse_sarif(text: str, root: pathlib.Path) -> dict:
    """Parse the SARIF rendering of the same run.

    Mull emits level=warning for survived and level=note for not-covered, and
    puts the mutator in ruleId — machine-readable, so no prose is scraped here.
    Killed mutants are not emitted at all, which is why SARIF cannot stand alone.
    """
    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        raise GateFailure(f"the SARIF report does not parse as JSON: {error}") from error

    runs = document.get("runs")
    if not isinstance(runs, list) or len(runs) != 1:
        raise GateFailure(
            "the SARIF report does not contain exactly one run; this gate judges one "
            "mutation run at a time and will not guess which."
        )

    by_level = {"warning": [], "note": []}
    for result in runs[0].get("results") or []:
        level = result.get("level")
        if level not in by_level:
            raise GateFailure(
                f"the SARIF report contains a result at level {level!r}, which this gate "
                "does not recognise. An unclassified result is an unexamined one."
            )
        mutator = result.get("ruleId")
        locations = result.get("locations") or []
        if not mutator or len(locations) != 1:
            raise GateFailure(
                "a SARIF result is missing its ruleId or does not have exactly one location; "
                "it cannot be matched against the IDE report."
            )
        physical = locations[0].get("physicalLocation") or {}
        uri = (physical.get("artifactLocation") or {}).get("uri")
        region = physical.get("region") or {}
        if uri is None or "startLine" not in region or "startColumn" not in region:
            raise GateFailure("a SARIF result is missing its file, line or column.")
        by_level[level].append(
            f"{relative(uri, root)}:{region['startLine']}:{region['startColumn']}:{mutator}"
        )
    return {"Survived": by_level["warning"], "Not Covered": by_level["note"]}


def cross_check(ide: dict, sarif: dict) -> None:
    """The two renderings must describe the same mutants.

    This is the reason the gate reads both. A parser bug, a truncated file, or a
    Mull change that moves a field shows up here as a disagreement rather than
    as a quietly wrong verdict.
    """
    for label in ("Survived", "Not Covered"):
        from_ide = sorted(record["key"] for record in ide["records"][label])
        from_sarif = sorted(sarif[label])
        if from_ide != from_sarif:
            only_ide = sorted(set(from_ide) - set(from_sarif))
            only_sarif = sorted(set(from_sarif) - set(from_ide))
            raise GateFailure(
                f"the IDE and SARIF reports disagree about {label!r} mutants.\n"
                f"    only in the IDE report:   {only_ide or '-'}\n"
                f"    only in the SARIF report: {only_sarif or '-'}\n"
                "  Two renderings of one run cannot both be right, and the gate cannot tell "
                "which is wrong. Neither is trusted."
            )


def load_allowlist(path: pathlib.Path) -> list[str]:
    if not path.exists():
        return []
    entries = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            entries.append(line)
    duplicates = {entry for entry in entries if entries.count(entry) > 1}
    if duplicates:
        raise GateFailure(
            f"tools/mutation-allowlist.txt lists {sorted(duplicates)} more than once; "
            "one reviewed decision per line."
        )
    return entries


def run_mull(binary: str, runner: str, report_dir: pathlib.Path) -> tuple[str, str]:
    """Run Mull, asking for both renderings in one pass.

    --allow-surviving because survivors are this gate's business, not an exit
    code: Mull exits non-zero on any survivor, which once killed the shell
    version of this script before it reached its own allowlist logic.
    --ide-reporter-show-killed because without it the killed total is never
    printed and the report cannot be reconciled against its own score.
    --include-not-covered because a mutant on an unexecuted line contradicts the
    100% coverage gate and the project would rather hear about it.
    """
    report_dir.mkdir(parents=True, exist_ok=True)
    command = [
        runner,
        binary,
        "--reporters",
        "IDE",
        "--reporters",
        "Sarif",
        "--report-dir",
        str(report_dir),
        "--report-name",
        "mutation",
        "--ide-reporter-show-killed",
        "--include-not-covered",
        "--allow-surviving",
    ]
    print("mutation gate: " + " ".join(command))
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    if completed.returncode != 0:
        raise GateFailure(
            f"{runner} exited {completed.returncode}. With --allow-surviving that is a "
            "genuine failure to run, not a surviving mutant."
        )
    return _read_reports(report_dir)


def _read_reports(report_dir: pathlib.Path) -> tuple[str, str]:
    ide_path = report_dir / "mutation.txt"
    sarif_path = report_dir / "mutation.sarif"
    for path in (ide_path, sarif_path):
        if not path.exists():
            raise GateFailure(
                f"expected report {path} does not exist. The gate requires both the IDE and "
                "SARIF renderings; one of them is missing, so the run cannot be cross-checked."
            )
    return (
        ide_path.read_text(encoding="utf-8", errors="replace"),
        sarif_path.read_text(encoding="utf-8", errors="replace"),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", help="test binary to mutate")
    parser.add_argument("--runner", default="mull-runner-18", help="Mull runner executable")
    parser.add_argument(
        "--report-dir",
        type=pathlib.Path,
        help="read mutation.txt and mutation.sarif from here instead of running Mull",
    )
    parser.add_argument("--allowlist", type=pathlib.Path, help="reviewed-survivor file")
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent,
        help="repository root, used to make report paths relative",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    allowlist_path = args.allowlist or root / "tools" / "mutation-allowlist.txt"

    try:
        if args.report_dir is not None:
            ide_text, sarif_text = _read_reports(args.report_dir)
        elif args.binary:
            ide_text, sarif_text = run_mull(args.binary, args.runner, root / "build" / "mutation")
        else:
            parser.error("give a test binary to mutate, or --report-dir to judge existing reports")

        ide = parse_ide_report(ide_text, root)
        reconcile_ide(ide)
        cross_check(ide, parse_sarif(sarif_text, root))
        allowed = load_allowlist(allowlist_path)

        not_covered = sorted(record["key"] for record in ide["records"]["Not Covered"])
        if not_covered:
            # Not allowlistable on purpose. A mutant on a line the tests never
            # execute contradicts the 100% line-coverage gate, measured by an
            # entirely different mechanism. That disagreement is the finding.
            for key in not_covered:
                print(f"  NOT-COVERED MUTANT: {key}", file=sys.stderr)
            raise GateFailure(
                f"{len(not_covered)} mutant(s) sit on lines the tests never executed, while "
                "the coverage gate reports 100% line coverage. Two independent measurements "
                "of the same property disagree; resolve that before trusting either."
            )

        survivors = sorted({record["key"] for record in ide["records"]["Survived"]})
        unexplained, used = [], set()
        for key in survivors:
            if key in allowed:
                print(f"  allowed (reviewed): {key}")
                used.add(key)
            else:
                print(f"  UNEXPLAINED SURVIVOR: {key}", file=sys.stderr)
                unexplained.append(key)

        # A stale entry is a defect too: an allowlist that outlives its mutant
        # quietly grants permission for something that no longer exists.
        stale = [key for key in allowed if key not in used]
        for key in stale:
            print(f"  STALE ALLOWLIST ENTRY (mutant no longer survives): {key}", file=sys.stderr)

        print(
            f"mutation gate: {len(unexplained)} unexplained, {len(allowed)} allowed, "
            f"{len(stale)} stale"
        )
        if unexplained or stale:
            raise GateFailure(
                "kill each survivor with a better test, or add its key to "
                "tools/mutation-allowlist.txt with a written justification. Remove stale "
                "entries — an allowlist that outlives its mutant is a permission nobody reviewed."
            )
    except GateFailure as failure:
        print(f"mutation gate: FAILED — {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
