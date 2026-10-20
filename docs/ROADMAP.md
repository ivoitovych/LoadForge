# Roadmap — the ordered queue from here to 1.0

This is the working queue. Read it at the start of every session, take the first
item that is not done, and update this file in the same PR that lands an item
(ADR 0002). Scope and exit criteria for each milestone are in
`docs/IMPLEMENTATION.md` §4; this file only orders the work and records where it
stands.

**Rules of the queue.** Each item lands as one PR, verified locally against the
full gate set before the push, merged when CI is green. A decision met on the way
is researched, taken, and recorded (ADR, journal, or PR description), and the
work continues.

## Standing items

- **Find an outside reviewer.** No PR has had one. Every completion report
  repeats this until one is found.
- **Branch protection on `main`.** Owner-only: a repository setting.

## Now — finish M1

| # | Item | Source | State |
|---|------|--------|-------|
| 1 | Land the controller: `controller/`, `reporters/console`, `loadforge stress`, `tests/supervision/` | IMPLEMENTATION §10 | done, #23 |
| 2 | Comprehensive review, reaction plan, ADR 0002, this roadmap | `reviews/` | done, #21 |
| 3 | Seqlock writer: release fence after the odd store; correct the P5 argument | review R-01 | done, #24 |
| 4 | Controller ignores SIGPIPE through the seam; test and catalogue row | review R-02 | done, this PR |
| 5 | Kill the surviving drain-bound mutant with an exact assertion | review R-06 | |
| 6 | Hygiene batch: linkage, torn-slot wording, drain bound by wall time, slot layout asserts, fixture references | review R-03..R-05, R-24, R-29 | |
| 7 | Documentation reconciliation in one commit | review R-08..R-13, R-19, R-21, R-25, R-27, R-33 | |
| 8 | Fake-worker executable with the misbehaviour catalogue; gate: no test-hook symbol in the shipped binary; gate: lock obligations in the ledger | IMPLEMENTATION §10 | |
| 9 | Memory-budget reader (`MemAvailable`, cgroup v1 and v2) and OOM classified from the kernel's record | IMPLEMENTATION §10, journal §1.18 | |
| 10 | M1 exit: every criterion in IMPLEMENTATION §4 met and marked | IMPLEMENTATION §4 | |

## Next — prerequisite for M2a

| # | Item | Source | State |
|---|------|--------|-------|
| 11 | Pin a Mull release by verified SHA-256 and run the mutation gate end to end in CI; schedule it weekly on `main` | IMPLEMENTATION §4 M0, review R-31 | |
| 12 | clang-tidy over tests and tools, or the scope documented; trust-chain linter versions pinned | review R-07, R-32 | |

## Then — the milestones

| # | Milestone | Builds |
|---|-----------|--------|
| 13 | M2a | `workloads/compression`, `verification/{oracle,checksum,isolation,golden}`, input-integrity re-verification, minimal `telemetry/capability` |
| 14 | M2b | `workloads/dense_linear`, `verification/{residual,invariant}`, `platform/fpenv`, first vectorised kernel behind one dispatch seam |
| 15 | M3 | telemetry sources, sampler, timeline, statistics, `controller/results`, CSV and JSON reporters, remediation, verification duty cycle, EDAC |
| 16 | M4 | workloads 3 to 11, `primitives/*` |
| 17 | M5 | `workloads/mixed`, thermal and power cycling, `controller/journal` |
| 18 | M6 | `explore/` |
| 19 | M7 | hardening, docs, packaging, manual hardware validation, 1.0 |

Each milestone is broken into increments in this file when it becomes the current
one, the way M1 is above.
