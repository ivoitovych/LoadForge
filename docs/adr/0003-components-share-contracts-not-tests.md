# ADR 0003 — Components share contracts, never tests or mocks

**Status:** accepted
**Date:** 2026-10-20 01:58 UTC

## Context

LoadForge is built as components in their own directories (`core`, `platform`,
`topology`, `config`, `worker`, `controller`, `reporters`, and from M2a on,
workloads, verification and telemetry). Each has a header contract and a test
suite held to 100% coverage, mutation testing and the sanitizers. Most of what
is left to build can be developed in parallel, provided each component can be
built and tested against its dependencies' contracts before those dependencies
exist.

The obvious way to do that is to share: one mock per contract, one conformance
suite per contract, used by everyone. This project has already paid for that.
Every component above the system-call seam is tested against a single shared
fake, and that fake models `write(2)` to a pipe with no reader as returning
`EPIPE`. The kernel first delivers `SIGPIPE`, which kills the writer. The
controller inherited that reading, 637 tests passed, and the real-binary stress
test then died by signal 13 in 2 runs out of 12 (finding R-02 of the review of
2026-10-19). One interpretation, written once, cannot disagree with itself, so
nothing could have flagged it.

`docs/verification.md` §2 already forbids verifying an implementation by
re-running it. A shared mock is that mistake made at the level of a contract.

## Decision

1. **The contract is the only thing components share.** A contract is a header
   with its documented semantics: preconditions, results, error cases, ordering
   and lifetime. It is written before either side is built.
2. **Each component writes its own tests and its own mocks.** A provider tests
   that it meets the contract; each consumer mocks the contract as it reads it.
   Neither reads the other's tests or mocks. Two readings of one contract are
   two chances to notice it is ambiguous.
3. **Readings are compared by execution, not by sharing.** The provider's
   contract tests are also run against each consumer's mock. A mock that fails
   them is a consumer that read the contract differently, and the contract is
   corrected until both readings agree. Authorship stays independent; only
   execution crosses the boundary.
4. **A real-against-real tier stays.** Cross-execution catches disagreements
   either suite thought to test. What neither thought of is found by running the
   real components together, and against the real kernel, as the supervision
   and real-binary tiers do today.
5. **A component knows only its direct dependencies' contracts,** never their
   internals, tests or mocks. Adding a component then costs a fixed amount of
   understanding, not one per component already present: the cost of the
   system grows linearly, not quadratically.

## Consequences

- More mock code: each consumer writes its own. That duplication is the point,
  and each mock is small because it covers only what its consumer uses.
- Contracts must be precise enough to test from the outside. A contract that two
  careful readers implement differently is a defect in the contract, found early.
- **The shared system-call fake is a known deviation.** `tests/support/fake_syscalls.hpp`
  predates this decision and serves every module above the seam. It is not
  replaced at once. New components bring their own fakes of the contracts they
  consume, and the seam's fake is checked against the real kernel by the
  existing real-syscall tests; each behaviour the kernel shows that the fake
  does not (the `SIGPIPE` case first) becomes a real-kernel test.
- The roadmap gains a contracts-first step after M1: the workload, verifier and
  telemetry-source contracts, written before any of their implementations, so
  M2a to M4 can proceed in parallel streams.
