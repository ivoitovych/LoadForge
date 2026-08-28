# Contributing to LoadForge

Contributions are welcome — issues, forks and pull requests alike.

**Current status: M0 (foundation) is complete.** The build system, CI and the first core
module are in place; workloads begin at M2a. The rules below apply to code now.

Review of [`docs/PLAN.md`](docs/PLAN.md) is still valuable, particularly the open
questions at the end.

---

## Three rules that are not obvious

LoadForge is a tool whose only product is trust. A false "your CPU is broken" destroys
its value as surely as a missed real fault. Three project rules exist because of that,
and a well-meaning contributor will violate all three by default. Please read these
before writing code.

### 1. Never verify an implementation by re-running it

Checking a blocked, vectorized GEMM by calling the same blocked, vectorized GEMM again
proves the code is *deterministic*, not that it is *correct*. If the optimized kernel
has a bug, both runs agree and the test passes forever.

Every workload has two implementations:

| Stress implementation | Reference oracle |
|---|---|
| Parallel, vectorized, optimized | Scalar, simple, slow is fine |
| Runs the full problem | Small subsets or sampled blocks |
| Derived from performance engineering | **Derived from the mathematical definition** |

The oracle must be written *from the specification*, not transcribed from the optimized
code — a "simplified copy" inherits the original's bugs and gives false assurance.

Golden vectors are a **regression tripwire** for detecting unintended change across
compilers and refactors. They are not a correctness oracle, because they are generated
by the code they validate.

See [`docs/verification.md`](docs/verification.md).

### 2. Do not weaken a workload's declared determinism class

Every workload declares one of:

- `BitExact` — identical bits across thread counts, repeat runs, ISA paths **and
  architectures** (integer and discrete results only)
- `BitExactFixedDecomposition` — identical bits **within one ISA path** for a given
  decomposition. SIMD width is part of the decomposition: AVX2, NEON and AVX-512 build
  different reduction trees and cannot match bit-for-bit
- `BoundedResidual` — reproducible only within a derived tolerance

A pull request may not silently move a workload to a weaker class. Thread-count
invariance is not pedantry: it is what allows a failing result to be reproduced
single-threaded, which is the most valuable diagnostic step the tool has.

Also: `-ffast-math` and `-Ofast` are banned outright. `-ffp-contract=off` is set
project-wide, and FMA is exercised **explicitly** so it is both deterministic and fully
stressed. In stress kernels use the ISA intrinsic (`_mm256_fmadd_pd`, `vfmaq_f64`), which
guarantees the hardware instruction; `std::fma` guarantees the IEEE semantics but may
compile to a slow software call, so keep it for oracles and portable code.

See [`docs/determinism.md`](docs/determinism.md).

### 3. State memory ordering explicitly, always

Every atomic operation names its memory order. No relying on the `seq_cst` default by
omission — the ordering is a design decision each time, and a reviewer needs to see it.

x86-64 is total-store-ordered; ARM64 is not. Code that is *accidentally* correct on x86
compiles fine and fails rarely and non-deterministically on ARM. In this project that
failure mode is uniquely dangerous: **a memory-ordering bug in LoadForge is
indistinguishable from the hardware fault it claims to have found.**

CI runs the full suite on both architectures and under ThreadSanitizer. Both must pass —
but understand what that does and does not establish:

- **ARM64 CI** gives ordering bugs an *opportunity* to manifest on weakly-ordered
  hardware. A rare-but-legal reordering may simply never occur during a run.
- **ThreadSanitizer** is a dynamic **data-race** detector that understands atomics and
  synchronization. It is not a model checker and does not enumerate permitted executions.

They are complementary evidence, **not proof**. So:

> Any atomic weaker than `seq_cst` needs a **documented synchronization argument** and a
> **dedicated test exercising the intended ordering protocol**.

**Across processes, add one more:** every process-shared lock is `PTHREAD_MUTEX_ROBUST`,
and every acquisition handles `EOWNERDEAD` by making the protected state consistent. A
worker can die holding a lock — that is not an edge case here, it is the expected event the
process split exists to survive. Without robustness one dead worker deadlocks every
survivor, and multi-process becomes *worse* than threads at the one thing it was chosen
for. Each such lock needs a test that kills the holder mid-critical-section.

And the cheapest defence of all: **prefer not to write lock-free protocols.** Default to
mutexes and `seq_cst`. Reach for `relaxed`/`acquire`/`release` only when measurement shows
the simple version is a real bottleneck — then pay for the argument and the test. In this
project a concurrency bug is indistinguishable from a hardware fault, so having less
clever concurrency to be wrong about is worth more than any amount of testing it.

---

## Signing off your work

LoadForge uses the [Developer Certificate of Origin](https://developercertificate.org/)
(DCO) — the same lightweight mechanism as the Linux kernel. There is no CLA and no
copyright assignment.

Add a sign-off line to each commit:

```bash
git commit -s
```

which appends:

```text
Signed-off-by: Your Name <your.email@example.com>
```

This certifies that you wrote the contribution, or otherwise have the right to submit it
under the project's licence. CI checks for it.

## Licensing

LoadForge is **GPL-3.0-or-later**. By contributing you agree your work is licensed the
same way.

Every source file carries an SPDX header as its first line:

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
```

New third-party code must be GPL-compatible, vendored under `third_party/`, pinned to an
exact version, and recorded in `NOTICE`. Runtime dependencies stay at zero — if a change
adds one, discuss it in an issue first.

---

## Pull requests

Say in the description **which test tier(s) your change adds to**. The tiers are in
[`docs/PLAN.md` §7.1](docs/PLAN.md), and
[`docs/IMPLEMENTATION.md` §2](docs/IMPLEMENTATION.md) explains what each execution path in
your change owes — including the classes a coverage tool cannot see, such as syscall error
paths and capability states. The short version of that rule:

> A path is covered when it is **executed** by a test, **asserted** on its observable
> result, and **demonstrated to fail** when the behaviour it guards is broken.

The third part is the one usually skipped, and it is what separates a test suite from a
decorative one.

Non-negotiable review standards:

- A change touching `verification/` **must** come with a fault-injection test — a test
  that deliberately corrupts data and asserts the corruption is caught and classified
  correctly. This is required regardless of what coverage numbers say. A verification
  path without a negative test counts as untested.
- A change touching `verification/` also needs an **unrun-check test** (tier T5b): make
  the check *impossible* — the oracle cannot allocate, the block loop runs zero times —
  and assert the run reports `VerificationNotPerformed` rather than success. Fault
  injection proves the verifier notices corruption; this proves it notices itself not
  running, which is the failure mode that reads to a user as "your hardware is fine".
- A new workload needs an independent oracle, invariant tests, a declared determinism
  class, and fault-injection coverage.
- Anything touching the platform layer needs fixture tests, including the hostile
  fixtures (missing sensors, `EACCES`, wrapping counters).
- **Coverage is 100%, gated.** Code and its tests land together in the same pull
  request. There is no "tests to follow" — that campaign never comes, and tests written
  later encode what the code *does* (bugs included) rather than what it should do.
- Anything genuinely unreachable needs an inline exclusion marker carrying an
  **`LF-COV-NNN` ID and a written reason**, plus a matching record in
  `tools/coverage-exclusions.txt` — in the same pull request, so the justification lands
  in the diff a reviewer is already reading:

  ```cpp
  if (rc < 0) { abort(); }  // GCOVR_EXCL_LINE LF-COV-001 abort() cannot return
  ```

  `tools/check-exclusions.sh` gates this: a missing ID, a reason-less marker, an
  unreviewed or relocated exclusion, a reused ID, a `STOP` before its `START`, a nested or
  unclosed region, or a record that outlived its code all fail the build. The count is
  currently zero. Try hard to keep it there — read the header of that file first; every
  exclusion this project has been tempted by so far turned out to be a real defect wearing
  a disguise.
- A change to `tools/` that adds or alters a gate needs a case in
  `tests/tools/test_tooling.sh` that feeds the gate evidence it should **refuse to
  interpret** — a truncated report, an empty manifest — and asserts it fails. The gates
  have twice reported success on input they did not understand (`docs/PLAN.md` F20), and
  a gate in that state produces the same output as a clean run. Testing that it says
  "pass" when it should is only half the test.
- If the gate parses the output of an **external tool**, the fixtures must be derived from
  that tool — its own test suite, or a captured run — never from your reading of its
  documentation, and there must be a test that runs the real binary. F21: the mutation
  gate's entire fixture suite passed while the parser misread every mutator identifier,
  because the fixtures encoded the same wrong assumption. A model of an external tool
  cannot falsify itself.
- External dependencies are fetched **only** from `cmake/Dependencies.cmake`. Adding a
  `FetchContent_Declare` elsewhere fails `tools/check-fetch-centralization.sh`, because
  the manifest cross-check cannot reconcile a pin it does not know to look for.
- For a new workload, **write the reference oracle and its tests first**. F11 requires
  the oracle to be derived independently from the specification; writing it before the
  optimized kernel exists is the cleanest guarantee of that.

The full test suite must pass on **{x86-64, ARM64} × {GCC 13, Clang 18}** plus the
sanitizer jobs.

## Reporting a hardware failure

If LoadForge says your machine computed something wrong, that is exactly the report the
project wants — but it is only actionable with provenance. Please use the
**hardware failure** issue template, and note the first question it asks:

> **Did `loadforge selftest` pass?**

That single answer separates a broken build or an unsupported machine from a genuine
hardware finding, and it is the first thing triage looks at.

## Reporting missing or wrong telemetry

Sensor naming is not standardized across boards and drivers, so "LoadForge reports the
wrong temperature" or "no package power on my machine" are expected reports, not
nuisances. Use the **unsupported platform** template; a capture of your `/sys` tree
(there will be a `tools/capture-sysfs.sh` for this) can become a permanent test fixture,
which is one of the most useful contributions available.

## Building

```bash
cmake --preset debug        # or: release, asan, tsan, coverage, simd-scalar
cmake --build --preset debug
ctest --preset debug
```

Reference platform is **Ubuntu 24.04 LTS** — GCC 13.3, Clang 18.1, CMake 3.28. The full
supported-components table is in [`docs/platforms.md`](docs/platforms.md).

## Conduct

Be decent to each other. Technical disagreement is welcome and expected; personal
hostility is not. Report problems to the address in `CODE_OF_CONDUCT.md`.
