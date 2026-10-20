# Reaction plan for the comprehensive project review of 2026-10-19 04:49 UTC

- **Purpose:** turn the findings in `reviews/comprehensive-project-review-2026-10-19-0449.md` into ordered, bounded work items, each with its gate.
- **Ground rules:** no item is committed or pushed without the owner's authorisation; every code change lands with its test and passes the coverage, obligation and mutation gates; documentation fixes are grouped into one commit so that the status sentences change together.

## Phase 1 — correctness (do first, one PR each, on top of the controller increment)

1. **R-01 seqlock writer fence.** In `publish()`: odd sequence store, then a release fence, then the field stores, then the even release store. Rewrite the P5 argument in the heartbeat test to state what each fence orders. Add a comment in `heartbeat.hpp` replacing "checked against a run under TSan" with "checked by review; TSan cannot see writer-side ordering". Gate: existing suites; the obligation row for `src/worker/ipc` already claims P5. Estimated size: under 30 lines of code and comment.
2. **R-02 controller SIGPIPE.** Call `ignore_signal(SIGPIPE)` through the seam at the top of `Controller::run` (so the fake-driven orchestration tests see it) and treat its failure as a `ControllerError`. Add one fake-driven test asserting the call, and one catalogue row "worker closes its control pipe before the deadline stop". Verify with the T8 supervision suite that a worker exiting at the deadline no longer ends the controller. Gate: coverage and the mutation sweep on the controller.
3. **R-06 surviving mutant.** Replace the `read_count > 1000` assertion with an exact or tightly bounded expectation so the loop-bound mutant dies. Re-run the controller mutation sweep and confirm zero survivors before the gate is pointed at the increment.

## Phase 2 — hygiene (one PR, no behaviour change)

4. **R-03** move `whole_document_fault` into the anonymous namespace.
5. **R-04** reword the torn-slot detail to "slot never observed consistent in 64 attempts".
6. **R-05** bound `drain_after_death` by wall time (suggest 10 s) or lower the drain count; record the choice in the journal.
7. **R-24** add `static_assert`s pinning the heartbeat slot layout next to the magic, if absent.
8. **R-29** reference the three config fixtures from the ledger row note.

## Phase 3 — documentation reconciliation (one commit, edited together)

9. **R-08** PLAN §11 final paragraph, IMPLEMENTATION §9 table, IMPLEMENTATION §4 M5 criterion 4: mark Q10 closed per PLAN rev 14.
10. **R-09** NOTICE and MANIFEST header: state that toml++ is vendored.
11. **R-10** CONTRIBUTING status paragraph: describe the current tree.
12. **R-11** platforms.md: mutation is manual (`workflow_dispatch`); cgroup v1 is read, per JOURNAL §1.18.
13. **R-12** unsupported-platform issue template: say the capture script is planned, or drop the line until it exists.
14. **R-13** the five JOURNAL §6 contradictions, with the owner's answer on PLAN fallback-vs-override wording.
15. **R-19** fs.hpp thread-safety justification: "will be multithreaded at M3".
16. **R-21** one table of the reserved worker exit-code band (119..125) in IMPLEMENTATION.md, cited from both headers.
17. **R-25, R-27** operator wording: the stall case for "controller lost", and the `--worker` processes an operator will see.
18. **R-33** make README "Project status" point at IMPLEMENTATION §10 rather than restating it.

## Phase 4 — owner decisions (no work until answered)

19. **R-07, R-32** widen clang-tidy to tests and tools, or document the scope; pin the trust-chain linter versions.
20. **R-31** schedule the mutation workflow weekly on `main`, or keep it manual and say so in platforms.md.
21. Already pending before this review: commit and PR of the controller increment and the doc reconciliation; the Mull pin; branch protection; deletion of merged branches.

## Order of operations

1. Owner authorises commits.
2. Phase 1 items 1 and 2 as separate PRs, each gated.
3. Item 3, then Phase 2, then Phase 3 as one commit each.
4. Phase 4 as decided.

## What is explicitly not planned

- No architecture change: every defect is local.
- No coverage exclusion: R-01 and R-02 are both reachable from the fake.
- No allowlisting of the R-06 mutant.
