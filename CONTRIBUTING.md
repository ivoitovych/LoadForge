# Contributing to LoadForge

Contributions are welcome — issues, forks and pull requests alike.

**Current status: the project is at the design stage. There is no code yet.** Right now
the most valuable contribution is review of [`docs/PLAN.md`](docs/PLAN.md), especially
the open questions at the end. Once M0 lands, the rules below apply to code.

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
[`docs/PLAN.md` §7.1](docs/PLAN.md).

Non-negotiable review standards:

- A change touching `verification/` **must** come with a fault-injection test — a test
  that deliberately corrupts data and asserts the corruption is caught and classified
  correctly. This is required regardless of what coverage numbers say. A verification
  path without a negative test counts as untested.
- A new workload needs an independent oracle, invariant tests, a declared determinism
  class, and fault-injection coverage.
- Anything touching the platform layer needs fixture tests, including the hostile
  fixtures (missing sensors, `EACCES`, wrapping counters).
- **Coverage is 100%, gated.** Code and its tests land together in the same pull
  request. There is no "tests to follow" — that campaign never comes, and tests written
  later encode what the code *does* (bugs included) rather than what it should do.
- Anything genuinely unreachable needs an inline exclusion marker **with a written
  reason**. The total exclusion count is reported in CI and may only go down.
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

Once M0 lands:

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
