# Reaction plan for the independent review of the wave-1 pull requests

- **Written:** 2026-10-20 15:47 UTC.
- **Review:** [review-response-2026-10-20-1251-UTC.md](review-response-2026-10-20-1251-UTC.md), the second independent review. It answers [the request of 2026-10-20 12:51 UTC](review-request-2026-10-20-1251-UTC.md).
- **Assessment:** all five findings are accepted. Each was checked against the source named in the review, and each is a real defect. None of them repeats an earlier finding.

## Assessment

**W1-01, major, accepted.** The record contract checks a numerical failure's tolerance, but not its residual against that tolerance. So "failed, residual 0 against tolerance 1" decodes, and is routed as a wrong result. The decoder is the trust boundary, so the fix belongs in the frozen contract (`src/verification/record.hpp`), its suite and its reference, as well as the codec. This is a contract change, made inside PR #39 and recorded there.

**W1-02, major, accepted.** The arithmetic is right. One sampled entry per 32 x 32 tile catches a damaged 4 x 8 kernel block in a single unit with probability 1/32. The header's claim, that a fault confined to a tile "meets a check", is false.
- *Decision:* sampling stays the default, because a full check costs as much as the product it checks.
- The guarantee is restated honestly, as a per-unit detection probability with detection across units for persistent faults.
- The limitation becomes a test, and grid = 256 is the tested full mode.
- The verdict's `blocks_compared` lets a consumer say "64 sampled entries agreed", never "the product is right". The reporters (stream 12) and the integration stream (11) must render it that way. This plan records that as a requirement for both.

**W1-03, major, accepted.** The CI exemption for "GitHub's merge commits" rests on a committer email and a parent count, both of which can be forged. A crafted two-parent commit can therefore carry new content past the identity, trailer and sign-off checks.
- *Decision:* a two-parent commit is exempt only when its tree is exactly the clean automatic merge of its parents (`git merge-tree --write-tree`). Such a commit adds no content, so who committed it does not matter.
- Its author must still be admitted.
- Every other merge is judged in full.
- The same predicate applies in the DCO check, which today skips every merge.

**W1-04, moderate, accepted.** The commit-msg hook requires a sign-off matching the author, while pre-push and both CI scripts accept any sign-off, or (pre-push) none. One predicate now serves all four places.

**W1-05, moderate, accepted.** An exact-difference record must be consistent with its own excerpt: the window covers `first_offset`, and the bytes differ there. Whole-comparison claims that an excerpt cannot reconstruct are documented as such.

## Work

| Finding | Where it is fixed | State |
|---|---|---|
| W1-01, W1-05 | PR #39 (verification core), contract change to `record.hpp` with suite and reference; regression tests red before and green after | in progress |
| W1-02 | PR #42 (bounded verification): header and journal text, limitation test, grid-256 control, `blocks_compared` pinned | in progress |
| W1-03, W1-04 | PR #47 (repository hooks): shared predicate for hooks and CI, throwaway-repository tests against the real scripts | in progress |

Each fix lands in the pull request the review read, before that pull request merges. The review's last point is that tests, sanitizers and real-kernel checks must be run against the integrated heads, ARM64 included. CI does that on every pull request and on main after each merge; the logs are on each PR's checks page.

## Verification evidence (added 2026-10-20 23:50 UTC)

Added under the rule of 2026-10-20 18:50 UTC (journal §4.13): a finding is reproduced or
disproved before it is fixed, and the plan records the verdict with its evidence. This plan
predates the rule. The assessment above was made by reading the cited source. The evidence
below is what now pins each verdict in the tree, so that the verdict can be checked without
trusting this text.

| Finding | Verdict | Evidence |
|---|---|---|
| W1-01 | CONFIRMED | `RecordCodecNumericalFailure.AResidualInsideTheToleranceIsACheckThatPassedAndDoesNotDecode` and `.ANanOrAResidualPastTheToleranceDecodesAndIsAWrongResult` (`tests/unit/verification/record/codec_test.cpp`); the record contract's case at `tests/contracts/verification/record_contract.hpp:230`; the fuzz oracle at `tests/fault_injection/verification/record/decode_fuzz_test.cpp:282`. Merged in #39. |
| W1-02 | CONFIRMED | `DenseLinearOracleVerdict.DocumentedLimitationADamagedBlockAvoidingTheSampleIsAgreement` reproduces the miss the review computed: a 4 x 8 block wrong by 1.0 that avoids the sampled entry is agreement. `.TheSameDamagedBlockIsCaughtAtFullGrid` is the control (`tests/unit/verification/oracle/dense_linear/oracle_test.cpp`). Merged in #42. |
| W1-03 | CONFIRMED | `tests/tools/test_git_hooks.sh`: "a forged GitHub merge adding content is refused", built with plumbing so that the committer and the parent count are the forger's; and `tests/tools/test_tooling.sh` (`mk_merge`) for the CI script. Merged in #47. |
| W1-04 | CONFIRMED | `tests/tools/test_git_hooks.sh`: "a sign-off for someone else is refused" (commit-msg), "a sign-off with another email is refused" and "a commit with no sign-off is refused" (pre-push), all through the one predicate in `tools/lib/commit-policy.sh`. Merged in #47. |
| W1-05 | CONFIRMED | `RecordCodecDifferenceWindow.AClaimedFirstDifferenceThatTheWindowShowsIdenticalDoesNotDecode`, which is the review's own counterexample (popcount 1, offsets 0, identical windows), and `.ADifferenceElsewhereInTheWindowDoesNotStandInForTheFirstOffset` (`tests/unit/verification/record/codec_test.cpp`); the contract case at `record_contract.hpp:239`. Merged in #39. |
