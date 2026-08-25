# What this changes

<!-- What and why. Link the issue if there is one. -->

# Test tiers touched

<!--
Name the tier(s) this change adds to or modifies. Tiers are listed in docs/PLAN.md §7.1.
e.g. T1 unit, T3 oracle, T5 fault injection, T5b unrun check, T6 determinism, T7 supervision.
-->

# Checklist

- [ ] Commits are signed off (`git commit -s`) — see [CONTRIBUTING.md](../CONTRIBUTING.md)
- [ ] New files carry `// SPDX-License-Identifier: GPL-3.0-or-later`
- [ ] **100% line and branch coverage** — tests land in this same PR, not later
- [ ] Passes on {x86-64, ARM64} × {GCC 13, Clang 18}, plus ASan/UBSan and TSan
- [ ] Any unreachable line carries an exclusion marker **with an `LF-COV-NNN` ID**, and a
      matching record in `tools/coverage-exclusions.txt` in this same PR

## If this touches `verification/`

- [ ] A **fault-injection test** is included — data is deliberately corrupted and the
      corruption is asserted to be caught *and classified correctly*. Required
      regardless of coverage numbers.
- [ ] An **unrun-check test** (T5b) is included — the check is made *impossible* (the
      oracle cannot allocate, the block loop runs zero times) and the run is asserted to
      report `VerificationNotPerformed` rather than success. Fault injection proves the
      verifier notices corruption; this proves it notices itself not running.
- [ ] Error records emit the right one of `ExactBitCorruption`,
      `NumericalVerificationFailure`, `VerificationNotPerformed` — with inapplicable
      provenance reported as absent, never fabricated.

## If this touches `tools/` or a CI gate

- [ ] The gate has a test in `tests/tools/test_tooling.sh` that feeds it evidence it must
      **refuse to interpret**, and asserts it fails (docs/PLAN.md F20). Testing only that
      it says "pass" when it should is half a test.

## If this adds or changes a workload

- [ ] Has an **independent reference oracle**, derived from the mathematical definition
      rather than transcribed from the optimized implementation — ideally **written first**
- [ ] Declares a determinism class, and does not weaken an existing one
- [ ] Has invariant / metamorphic tests
- [ ] Vectorized paths keep a working scalar fallback

## If this touches atomics or shared state

- [ ] Every atomic operation names its memory order explicitly
- [ ] Anything weaker than `seq_cst` has a **documented synchronization argument** and a
      **dedicated test exercising the intended ordering protocol**
- [ ] If this introduces a lock-free protocol: measurement is cited showing the simple
      (mutex / `seq_cst`) version was actually a bottleneck

## If this touches `platform/` or telemetry

- [ ] Fixture tests included, covering the hostile cases (missing sensor, `EACCES`,
      wrapping counter, sensor disappearing mid-run)
- [ ] No new runtime dependency
