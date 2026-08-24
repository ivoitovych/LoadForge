# The determinism contract

**Required reading before touching any workload, reduction or atomic.**

This document is the normative form of finding F1 in [`PLAN.md`](PLAN.md). If the two
ever disagree, this file wins for implementation detail and `PLAN.md` wins for rationale.

---

## 1. Why this matters more here than in most projects

Floating-point addition is not associative:

```text
(a + b) + c  ≠  a + (b + c)
```

So a parallel reduction whose accumulation order varies with thread count, work
stealing, or which worker happens to finish first produces **different bits on every
run, on perfectly healthy hardware**.

For most software that is a curiosity. For LoadForge it is fatal, twice over:

**It manufactures false positives.** LoadForge's entire output is the claim "your
hardware computed this wrong." A tool that reports corruption on a healthy machine is
worse than no tool at all — it will be dismissed, and correctly so.

**It destroys diagnosis.** When a real failure occurs, the most valuable next step is
re-running the failing unit of work single-threaded (see
[`verification.md`](verification.md), fault isolation). If single-threaded execution
legitimately produces different bits, that step is meaningless and the most informative
diagnostic the project has is gone.

The second reason is why this project holds a stricter line than "use tolerances
everywhere" — tolerances handle the false-positive problem but not the diagnostic one.

---

## 2. Determinism classes

Every workload **declares exactly one** class. The declaration is part of the workload's
public description, is recorded in `run.json`, and is enforced by the T6 test tier.

### `BitExact`

Identical bits across thread counts, across repeat runs, and across tile or block
configuration.

Achievable whenever the computation is integer or otherwise exact — integer arithmetic
is associative, so accumulation order does not matter. Required for: integer arithmetic,
sorting, compression, hashing, checksums, and the **integer** graph results (BFS
distances, connected-component labels).

For graph workloads, note the qualifier: only *order-independent* outputs qualify. A BFS
distance array is a property of the graph and is `BitExact`; a BFS *tree* (which parent
each vertex was reached from) depends on frontier ordering and is not. Checksum the
former, never the latter.

### `BitExactFixedDecomposition`

Identical bits across thread counts and repeat runs **for a given decomposition**
(tile grid, block size, panel width). Changing tile size legitimately changes results.

This is the interesting case and the default target for numerical workloads. It is
achievable far more often than people assume — see §3. Expected for: dense linear
algebra, stencil, FFT, Monte-Carlo, and the **floating-point** graph algorithms
(PageRank and other iterative FP methods).

"Decomposition" includes **SIMD width**, not just tile geometry — see §3.

### `BoundedResidual`

Reproducible only within a derived tolerance. Bits may differ between runs.

A legitimate choice, but it **costs the workload its single-threaded reproduction
path**, and that must be an explicit decision rather than an accident. Expected for:
adaptive mesh and anything with genuinely dynamic load balancing.

> **A pull request may not move a workload to a weaker class without an explicit,
> reviewed justification.** Silent weakening is the failure mode this document exists to
> prevent.

---

## 3. How to achieve bit-exactness under parallelism

The key insight: **bit-exactness does not require single-threaded execution. It requires
a fixed accumulation order.** Those are different things.

### Fixed-shape reduction trees

Make the accumulation order a function of the *problem decomposition* and nothing else:

```text
    Decomposition (fixed by configuration)      Reduction (fixed shape)

    ┌────┬────┬────┬────┐                          t0  t1  t2  t3
    │ t0 │ t1 │ t2 │ t3 │                           └─┬─┘   └─┬─┘
    └────┴────┴────┴────┘                             └───┬───┘
                                                          ▼
```

Any number of threads may execute that dependency graph, in any order, and the result is
identical — because the *shape* of the tree, not the timing, determines which partial
sums are added to which. A worker finishing early changes nothing.

What this rules out:

- **Work stealing that alters accumulation order.** Stealing is fine if the stolen unit
  keeps its position in the reduction tree; it is not fine if it changes which partial
  sums combine.
- **`omp parallel for reduction(+:x)`** and equivalents, whose order is
  implementation-defined and may vary with thread count.
- **Atomic accumulation into a shared float.** The order is whatever the hardware
  arbitration produced that run.
- **"First finished wins" merge patterns.**

### Compensated summation

Where accumulation depth is large enough that ordinary summation loses unacceptable
precision, use Kahan or Neumaier compensation. It does not replace a fixed reduction
order — it reduces the error within one.

### Counter-based RNG

Any workload generating pseudo-random data uses a **counter-based generator** (Philox,
Threefry). The value at index *i* is a pure function of `(seed, i)`:

```text
value(i) = philox(seed, i)        // independent of thread count, of decomposition,
                                  // and of which worker asks for it
```

A conventional sequential PRNG makes the data a thread produces depend on how work was
split, so changing thread count changes the dataset and the run is not reproducible
across machines with different core counts. Counter-based generation also lets fault
isolation regenerate a single failing block in isolation, which is why it is mandatory
rather than advisory.

### SIMD width is part of the decomposition

**A vectorized floating-point reduction is not bit-identical to a scalar one, and AVX2
is not bit-identical to NEON.** This is the same associativity problem as §1, one level
down, and it is easy to miss.

A lane-parallel reduction accumulates into per-lane accumulators and then combines them
horizontally:

```text
AVX2      4 doubles/vector  →  4 partial sums  →  horizontal combine
NEON      2 doubles/vector  →  2 partial sums  →  horizontal combine
AVX-512   8 doubles/vector  →  8 partial sums  →  horizontal combine
scalar    1 accumulator     →  no combine
```

Four different accumulation trees, therefore four different bit patterns — from the same
input, on flawless hardware, with identical IEEE 754 semantics and identical FMA
behaviour. Explicit FMA does not rescue this; it is orthogonal to it.

So **bit-exactness is scoped to an ISA path**, and the ISA path is recorded in the run
fingerprint (see `PLAN.md` F12) precisely so a result is interpretable.

### What is actually guaranteed

| Comparison | `BitExact` | `BitExactFixedDecomposition` | `BoundedResidual` |
|---|---|---|---|
| Across thread counts, same machine | **identical** | **identical** | within tolerance |
| Repeat runs, same machine | **identical** | **identical** | within tolerance |
| Across ISA paths (scalar vs AVX2 vs NEON) | **identical** | within tolerance | within tolerance |
| Across architectures (x86-64 vs ARM64) | **identical** | identical *only* on the same ISA path class | within tolerance |

The taxonomy is cleaner than it first looks, because the distinction falls out of the
arithmetic rather than being imposed:

- **`BitExact` workloads are integer or discrete.** Integer arithmetic is exact and
  associative, so a vectorized radix sort, a scalar radix sort, and an ARM one all
  produce the same sorted array; a compression round-trip is exact everywhere; a hash is
  a hash. These *are* identical across ISA paths and architectures, and CI asserts it.
- **`BitExactFixedDecomposition` workloads are floating-point.** They are identical
  within an ISA path and only bounded across paths.

**The scalar path is bit-identical across x86-64 and ARM64 for every class except
`BoundedResidual`** — it is pure IEEE 754 with no lane structure to differ. This is a
real, valuable and cheap guarantee: the `LOADFORGE_SIMD=scalar` CI job builds on both
architectures, so comparing its outputs turns the portability job into a
**cross-architecture determinism check** at no extra cost.

### Consequences to keep in mind

- **Golden vectors are per ISA path.** One recorded set of bits cannot serve scalar,
  AVX2, AVX-512 and NEON for a floating-point workload.
- **Comparing the scalar oracle against a vectorized implementation is a *bounded*
  check for FP workloads**, never a bit comparison. This is already how the pairings in
  [`verification.md`](verification.md) are written — residual norms, not equality — but
  the reason is this section.
- **If cross-width bit-exactness were ever genuinely needed**, it can be bought:
  normalize lane accumulators into a single accumulator at fixed block boundaries,
  independent of vector width. It costs performance in the inner loop. It is not done by
  default, and no current requirement justifies it.

---

## 4. Compiler flag policy

| Flag | Policy | Why |
|---|---|---|
| `-ffast-math` | **Banned** | Permits reassociation, drops NaN/Inf semantics, enables flush-to-zero. Breaks the verification contract itself. |
| `-Ofast` | **Banned** | Implies `-ffast-math`. |
| `-ffp-contract` | **`off`, project-wide** | Prevents *implicit*, compiler-discretionary fusion of `a*b+c`, which varies by compiler, version and optimization level. |
| `-freciprocal-math`, `-fassociative-math` | **Banned** | Same class of problem. |
| Explicit FMA intrinsics | **Encouraged** | See below. |

### Implicit contraction vs explicit FMA — not the same thing

This distinction matters, and getting it wrong once cost this plan a revision.

**Implicit contraction** is the compiler deciding on its own to fuse `a*b+c` into a
single FMA. It changes results depending on compiler and flags, and gives nothing in
return that cannot be had deliberately.

**Explicit FMA** — `std::fma`, `_mm256_fmadd_pd`, `vfmaq_f64` — is fully specified by
IEEE 754 (one rounding, not two) and therefore perfectly deterministic. It exercises the
FMA units exactly as hard.

So `-ffp-contract=off` does **not** mean "no FMA in LoadForge." It means FMA is used
*on purpose, where written*, which is both deterministic and better for a project whose
job includes stressing those units. Workloads intending to stress FMA use the intrinsics
directly.

---

## 5. How this is tested — tier T6

For each workload, according to its declared class:

| Class | Test |
|---|---|
| `BitExact` | Run at 1, 2, 3, 4, 7 threads → assert byte-identical output. Run twice at the same count → identical. Vary tile size → identical. |
| `BitExactFixedDecomposition` | Same thread-count sweep at a **fixed** tile configuration → byte-identical. Varying tile size may differ; the tolerance for that difference is asserted to stay within bounds. |
| `BoundedResidual` | Repeat runs stay within the derived tolerance. The declared tolerance itself is asserted to be derived, not hard-coded. |

Thread counts deliberately include non-powers-of-two (3, 7) because those expose
decomposition bugs that 2/4/8 hide.

Tests run on both x86-64 and ARM64.

---

## 6. Contributor checklist

Before opening a PR that touches numerical code or reductions:

- [ ] The workload declares a determinism class, and this change does not weaken it.
- [ ] Accumulation order depends only on the decomposition — not on thread count,
      completion order, or scheduling.
- [ ] No `omp reduction`, atomic float accumulation, or first-finished-wins merging.
- [ ] Any pseudo-random data uses the counter-based RNG, not a sequential PRNG.
- [ ] No banned flags introduced; FMA, if wanted, is explicit.
- [ ] T6 tests updated to cover the new or changed code path.
- [ ] Passes on both architectures.

If you need to weaken a class, say so explicitly in the PR description with the reason.
That is a legitimate request; doing it silently is not.
