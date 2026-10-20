# Independent review response — request of 2026-10-20 12:51 UTC

- **Request:** [review-request-2026-10-20-1251-UTC.md](review-request-2026-10-20-1251-UTC.md).
- **Date:** 2026-10-20.
- **Scope:** focused source-level examination of PR #39 (verification record), #42 (dense-linear bounded oracle/residual), #41 (compression oracle), #38 (executor), #40 (input integrity), #43/#44 (workload kernels), #37 (ISA/FP controls), #45 (isolation), #46 (mutation tooling), and #47 (Git policy). The deeper findings concern #39, #42, and #47. **This is not a line-by-line audit or approval of all eleven PRs.**
- **Revisions inspected:** #39 177e4231fb1b; #42 bc952e893d70; #41 a4e813645194; #38 36611c880199; #40 33ed9a494214; #43 79e995017873; #44 f7337e177282; #37 afb9a29a9357; #45 814f18444dc2; #46 a8e2c18115af; #47 93ae470aef0f. Code was read from each **PR head branch**, not from main or the documentation branch carrying this response.
- **Method and limitations:** GitHub source, nearby tests, written contracts, and journal fragments were inspected remotely. No LoadForge build, ctest, sanitizer, mutation campaign, live ARM64 hardware run, or project-level fault-injection experiment was carried out by this reviewer. All reproductions below are source-derived **proposed regression tests** unless explicitly labelled otherwise. Findings are not claims of a measured CI failure.
- **Already-known issues excluded:** EXT-01 through EXT-03 in [the earlier external review](review-response-2026-10-20-0352-UTC.md), the journal's listed contract ambiguities, and the 56 known mutation survivors. The findings here are different from those exclusions.

## Verdict

**Changes requested in the verification-record contract and in the Git policy before treating the trust-chain gates as complete.** The default dense-linear sampling scheme is a consciously bounded check, but its guarantee is overstated and needs an explicit coverage/attribution contract. There was no compelling counterexample in the focused reading of the strict LZ4 decoder or the executor hand-off, but neither has been proved correct by this review.

| ID | Area | Severity | Confidence |
|---|---|---|---|
| W1-01 | #39 record contract and decoder accept a numerical *failure* whose residual is inside tolerance | **Major: verdict integrity** | High, deterministic from source |
| W1-02 | #42 one-entry-per-tile sampling does not guarantee detection of a damaged microkernel block | **Major: overstated verification guarantee** | High, arithmetic counterexample |
| W1-03 | #47 CI trusts a spoofable two-parent "GitHub" committer exemption, including commits with new content | **Major: identity/policy gate bypass** | High, deterministic from source |
| W1-04 | #47 CI and pre-push accept a sign-off that does not match the author, despite the commit-message policy | **Moderate: gate inconsistency** | High, deterministic from source |
| W1-05 | #39 record contract accepts a claimed exact bit difference whose displayed bytes are identical | **Moderate: evidence consistency** | High, deterministic from source |

These priorities describe the consequence of a *hypothetical malformed producer or corrupted evidence*, not evidence that the normal shipped workload currently emits such records. In particular, record frames also have a CRC; that makes arbitrary in-transit mutation a separate, much harder event.

## W1-01 — A decodable NumericalVerificationFailure can describe a successful check

**Where:** [PR #39](https://github.com/ivoitovych/LoadForge/pull/39), [src/verification/record.hpp lines 368–398 and 513–555](https://github.com/ivoitovych/LoadForge/blob/feature/verification-core/src/verification/record.hpp#L368-L398), [src/verification/record/codec.cpp lines 472–482 and 570–572](https://github.com/ivoitovych/LoadForge/blob/feature/verification-core/src/verification/record/codec.cpp#L472-L482).

The type is documented as a **bounded check that exceeded its tolerance**, and its ratio() comment says every decodable record has residual/tolerance greater than 1 or NaN. However, is_valid() and broken_kind_rule() validate **only** that tolerance is positive and finite (plus text length). Neither validates the residual against the tolerance.

**Counterexample:** Construct a NumericalVerificationFailure with check = "C versus AB", residual = 0.0, tolerance = 1.0, no condition number, no element. It passes is_valid(); decode(encode(record)) yields the same *failure* record. is_wrong_result() identifies the record as a wrong result because it uses only the variant kind; the controller may therefore route a passing measurement toward isolation/reporting as a numerical fault. A negative finite residual also passes. The PR's codec layout test even deliberately serializes a -0.0 residual, illustrating that this state is presently considered decodable.

**Regression:** Assert that residual <= tolerance and residual < 0 (with a finite positive tolerance) do not decode to a numerical failure. Preserve NaN and +infinity as failure residuals, and finite values strictly greater than tolerance. Exercise is_valid(), broken_rule(), encode/decode, and the consumer's is_wrong_result() semantics. A producer-specific check alone is insufficient: the decoder is the trust boundary.

**Correction:** Modify the frozen record contract deliberately, then both is_valid() and broken_kind_rule(), to enforce that a numerical *failure* has either a NaN residual or residual > tolerance. Confirm how infinities, negative NaN payloads, and signed zero are meant to behave; the source documentation says infinity is a failure. This is a **contract weakness implemented consistently**, not a divergence between codec.cpp and its current weaker is_valid() contract.

## W1-02 — A damaged register block can be wholly missed by the default dense-linear sample

**Where:** [PR #42](https://github.com/ivoitovych/LoadForge/pull/42), [src/verification/oracle/dense_linear/dense_linear.hpp lines 13–21](https://github.com/ivoitovych/LoadForge/blob/feature/bounded-verification/src/verification/oracle/dense_linear/dense_linear.hpp#L13-L21) and [dense_linear.cpp lines 120–149, 171–177](https://github.com/ivoitovych/LoadForge/blob/feature/bounded-verification/src/verification/oracle/dense_linear/dense_linear.cpp#L120-L149); [PR #44](https://github.com/ivoitovych/LoadForge/pull/44), the 4-by-8 register-blocked kernels.

The header claims that one checked entry per tile means a fault confined to any single tile — "one microkernel's registers" or rows in a cache line — **meets a check**. That implication is false. An 8-by-8 tile grid over the 256-by-256 output divides the matrix into 64 **32-by-32** tiles, but checks only **one of 1,024 elements** in each. Touching a region does not imply touching its altered element.

**Counterexample:** Make a known-correct unit (the test suite's flat matrix is sufficient), take sample_entry(index, 8, 0), and change C at a *different* element in tile 0 by +1.0 (well beyond the floating-point tolerance). No checked entry changed, so compare_sample() returns Agreement{64}. For a uniform sample position, a single defective **4-by-8 kernel output block (32 elements) within one tile** is hit only with probability 32/1024 = 3.125%; a four-lane accumulator affecting four elements in one row is hit with probability 4/1024 = 0.390625%. Those fractions describe one unit; repeated persistent faults may have better eventual detection, but a transient fault in that unit does not.

**Regression:** Construct a wrong 4-by-8 output block avoiding the sampled position and assert **Agreement is possible under default sampling**; the test should lock in the known limitation, not misclassify it as a decoder defect. Show separately that grid=256 detects the corruption, and that any reported per-unit confidence distinguishes *sampled entries passed* from *all entries correct*.

**Correction:** Revise the header/verification contract: every tile is **represented**, not every fault confined to a tile is detected. If a one-unit whole-output correctness claim is required, use full sampling (grid=256), or add a genuinely global check with a documented sensitivity guarantee. Expose the sampling rate and any probabilistic confidence in the final verdict; do not promote Agreement on 64 entries into a proof of the remaining 65,472.

This finding disputes the documented certainty, **not** the intentional sampling design or Philox determinism. It is especially relevant to the stated objective of avoiding confident wrong hardware verdicts.

## W1-03 — A fabricated "GitHub" merge commit can bypass identity, signing, and trailer checks

**Where:** [PR #47](https://github.com/ivoitovych/LoadForge/pull/47), [tools/check-commit-identity.sh lines 78–95](https://github.com/ivoitovych/LoadForge/blob/chore/repo-git-hooks/tools/check-commit-identity.sh#L78-L95), [tools/check-dco.sh lines 42–49](https://github.com/ivoitovych/LoadForge/blob/chore/repo-git-hooks/tools/check-dco.sh#L42-L49); [.github/workflows/licensing.yml](https://github.com/ivoitovych/LoadForge/blob/chore/repo-git-hooks/.github/workflows/licensing.yml).

The identity gate performs a wholesale early continue when *committer email == noreply@github.com* and *parent count == 2*. Those are writable Git commit fields, **not authenticated evidence of a GitHub-generated merge**. The early continue skips the committer allowlist, signature policy, and trailer allowlist for the entire commit. The separate DCO gate uses git rev-list --no-merges and intentionally does not inspect any two-parent commit.

**Counterexample:** In a throwaway repository, make two ordinary valid parent commits, then use git commit-tree (or plumbing with explicit environment variables) to create a **two-parent** commit with an *admitted author*, *committer email noreply@github.com*, and a tree that contains a **new file or changed content not present in either parent**. Add a forbidden trailer and omit Signed-off-by. Its object is not a GitHub-generated merge, yet the identity gate takes the exemption and the DCO gate omits it. The policy gates therefore have no authenticated basis on which to accept that new content's provenance. This is not evidence the repository has actually received such a commit; it is an executable test construction.

**Regression:** Add a throwaway repository case with a synthetic two-parent commit that adds content, spoofed GitHub committer email, and forbidden trailer. Run both CI scripts and assert refusal, then check a genuine GitHub merge scenario remains supported. Also test a two-parent merge commit with an unadmitted committer and a normal human merge with a DCO sign-off.

**Correction:** Do not recognize GitHub automation solely by forgeable email and parent count. Treat unsigned, trailer-free merge commits as a narrowly documented exception **without trusting their committer identity**, or authenticate provenance through immutable trusted CI event metadata/verified signatures. More importantly, verify that a merge's tree is a legitimate combination of its parents if the policy intends to exempt merge-only metadata: a merge commit can itself add code, and "merges add no authored content" is not a sound invariant.

## W1-04 — The pre-push gate is weaker than the commit-message gate; CI accepts a mismatching sign-off

**Where:** [PR #47](https://github.com/ivoitovych/LoadForge/pull/47), [tools/git-hooks/commit-msg lines 25–36](https://github.com/ivoitovych/LoadForge/blob/chore/repo-git-hooks/tools/git-hooks/commit-msg#L25-L36), [pre-push lines 27–35](https://github.com/ivoitovych/LoadForge/blob/chore/repo-git-hooks/tools/git-hooks/pre-push#L27-L35), [check-commit-identity.sh lines 90–100](https://github.com/ivoitovych/LoadForge/blob/chore/repo-git-hooks/tools/check-commit-identity.sh#L90-L100), [check-dco.sh lines 61–76](https://github.com/ivoitovych/LoadForge/blob/chore/repo-git-hooks/tools/check-dco.sh#L61-L76).

The commit-msg hook requires a Signed-off-by trailer **matching the author email**. The pre-push hook calls only check-commit-identity.sh, which accepts any Signed-off-by trailer but does not check its presence or signer. The CI DCO gate tests only whether at least one Signed-off-by trailer exists, not whether it names the author. Therefore two CI gates together do not enforce the stronger rule the hook claims to mirror.

**Counterexample:** With an admitted author and committer, create a commit via git commit --no-verify containing Signed-off-by: Other <other@example.invalid>. Pre-push accepts the admitted identities and only allowed trailer key; CI's DCO finds a sign-off and accepts it. The commit-msg hook would have refused the exact same commit without --no-verify. A missing sign-off likewise passes pre-push, although it fails the DCO CI job.

**Regression:** In tests/tools/test_git_hooks.sh, bypass commit-msg and submit: no sign-off; sign-off with another email; correct sign-off. Assert pre-push and CI have the same outcomes as the intended policy. Include a positive test for the supported contribution workflow where author and signer intentionally differ, **if** that is actually allowed — currently the hook says it is not.

**Correction:** Centralize the DCO/identity/trailer validation and have the hook and CI use the same predicate, accounting explicitly for trusted merge-commit exceptions. Avoid a local green gate for a commit CI must reject; more importantly, prevent a commit-msg-only requirement from silently disappearing in CI.

## W1-05 — The exact-difference decoder can certify an impossible bit classification

**Where:** [PR #39](https://github.com/ivoitovych/LoadForge/pull/39), [src/verification/record.hpp lines 301–365](https://github.com/ivoitovych/LoadForge/blob/feature/verification-core/src/verification/record.hpp#L301-L365), [src/verification/record/codec.cpp lines 417–445](https://github.com/ivoitovych/LoadForge/blob/feature/verification-core/src/verification/record/codec.cpp#L417-L445).

The contract describes first_offset as the **first byte where expected and actual differ** and popcount as the differing bits over the whole comparison. But is_valid(Difference) and broken_difference_rule() do not verify even the part that is present in the record's own window: that the byte at first_offset actually differs. A record with popcount=1, first_offset=last_offset=0, window_offset=0, and nonempty **identical** expected/actual windows passes and is classified kSingleBit.

**Regression:** Build an ExactBitCorruption record with the fields above; assert rejection by both broken_rule and decode. Also check a first_offset whose byte is outside an implausibly short stored window. Keep the complete-comparison popcount rule separate: the codec cannot in general recompute a popcount from a 64-byte excerpt if the original comparison was longer.

**Correction:** At minimum require the window to cover first_offset and expected[first_offset - window_offset] != actual[that index]; check analogous last-offset consistency only when last_offset lies in the recorded window. Document which whole-comparison claims cannot be independently reconstructed from an excerpt. This is a hardening of evidence consistency, not a way to overcome the record channel's finite CRC.

## What appeared defensible in the focused reading

- **#41 compression oracle:** The decoder checks literal bounds before copying, rejects zero/past-start offsets, copies overlapping matches forward, checks the last token and last-five-literals margin, then compares all 65,536 decoded bytes against the input. A different valid LZ4 encoding of the same original data is correctly an Agreement; the journal's counterexample of a harmless offset flip is a principled limitation. No distinct false-pass counterexample was established here.
- **#42 residual bound:** The implementation follows its derivation's two-evaluation allowance and adds a positive underflow floor. I did not independently prove every rounding inequality, and a code reading or tests over selected orders are not a general proof for all legal evaluation trees. Do not interpret this review as such proof.
- **#38 executor:** The reviewed mutex/generation hand-off, seq_cst index claims and done_ wait appear internally coherent under the documented non-concurrent run()/destruction contract. The ARM64 execution and scheduler races were not reproduced here.
- **#40 integrity runner:** Opening and closing regeneration, the post-check provenance update, and triplicated-copy voting have explicit fail-closed paths in the reviewed source. Faults in the generator itself are not independent evidence because both regeneration and input use the same generator — an inherent assumption to document, not an additional confirmed bug here.
- **#37/#43/#44/#45/#46:** Selected ISA probe, FP register, LZ4 writer, dense-linear kernel, isolation, and mutation-gate files were inspected for obvious violations; they have **not** received full independent test or architecture coverage in this response. A missing finding must not be read as an approval.

## Proposed disposition and closing evidence

1. **First:** settle W1-01 and W1-05 as explicit corrections to the frozen verification-record contract, not local workarounds in the two producers. Show red-before/green-after codec and consumer regression tests.
2. **Second:** fix W1-03 and W1-04 across hooks and CI together, with throwaway Git object tests that run the real gate scripts; one green test suite against a self-consistent model is not sufficient.
3. **Third:** correct the guarantee in W1-02 and decide what the tool will report when only a sample was verified. Add a deliberate unsampled corruption test and a grid=256 control.
4. **Before relying on these PRs in a hardware diagnosis:** run the project tests and relevant sanitizers/real-kernel checks against integrated heads, including ARM64, and record the resulting commits and logs. None of that execution evidence is supplied by this review.

This response is **deliberately bounded**: it identifies concrete contract and gate counterexamples while declining to certify the remaining unexamined code.
