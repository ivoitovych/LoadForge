# Journal fragment 25 — workload: graph (BFS, PageRank)

**Written:** 2026-10-20 17:51 UTC
**Change:** `src/workloads/graph` (the workload, its PageRank kernels), its tests in the
unit, oracle, property, fault-injection and determinism tiers, and its ledger row.

## Established by experiment

### The spec's arithmetic is reproduced bit for bit on all three kernels, on both architectures

The independent reference in the tests (a plain queue BFS and a PageRank that gathers each
vertex's in-edges into a vector and accumulates term by term, written from `spec.hpp`
alone) equals the workload's output exactly for a = 1, 2, 4, 8, on the scalar path and on
AVX2 (x86-64, GCC 13, this container). The NEON kernel was built with the AArch64 cross
compiler and run under `qemu-aarch64` (user mode): the determinism tier, including the
golden digests recorded on x86-64, and the unit tier's NEON contract subject passed. So the
claim in `spec.hpp` that the ranks are the same bits on any ISA path and either
architecture holds for the code as built, with the evidence being those runs.

### `__m256d` cannot be a `std::array` element under `-Werror`

GCC 13 reports `ignoring attributes on template argument '__m256d'`
(`-Wignored-attributes`) for `std::array<__m256d, N>`. The AVX2 kernel keeps its two
accumulator vectors in a local struct. The NEON vector types had no such problem.

### Where the BFS spends its levels, and why bottom-up needs a floor

On the generated graphs (out-degree 8, uniformly random heads) the frontiers run roughly
1, 8, 60, 400, 2800, 14000, then a thin tail: the bottom-up step is chosen for the last
large levels. On a path (every edge to tail + 1) the BFS has 32768 levels of one vertex.
Classic direction optimisation switches to bottom-up when the frontier's edges exceed a
fraction of the unvisited vertices' edges, with no floor, which on an adversarial input
(only corrupt memory produces one here) can send many near-empty levels through a
full-graph step. The rule here also requires a frontier of at least 512 vertices; frontiers
are disjoint, so there are at most 64 bottom-up steps whatever the input, and total work is
bounded by 64 × (V + E) + O(V + E) (the totality clause of `workload.hpp`).
`APathOfThirtyTwoThousandLevelsIsComputedAndIsBounded` asserts the executor is not entered
once per level.

### `Descriptor::version`, `kVersion` and the golden digests

`Descriptor::version` starts at the specification's `kVersion` (1). The CRC-32 digests in
`tests/determinism/workloads/graph/determinism_test.cpp` are produced by the tests' own
reference, not by the kernels, and are asserted on both architectures (the scalar-only job
of each); they move only when `Descriptor::version` does.

### Sanitisers

ASan+UBSan and TSan (clang 18) pass over every tier. Both need `libclang-rt-18-dev`, which
this container did not have installed; it was installed with apt.

## Decisions

1. **Factory, not registry.** `graph::create(const WorkloadConfig&)` in
   `src/workloads/graph/graph.hpp` returns the workload or a `WorkloadError`. The Registry
   belongs to the integration stream, which links `create` under `graph::kName`.
2. **Paths.** The descriptor's `paths` is exactly the set of kernels the build has: scalar
   always, AVX2 on x86-64, NEON on AArch64, unless the build is scalar-only
   (`LOADFORGE_SIMD_SCALAR`). No AVX-512 kernel: `spec.hpp` names scalar, AVX2 and NEON,
   and the arithmetic is pinned, so a wider kernel buys speed only. An ISA path without a
   kernel is refused as `kConfiguration`, never replaced (`WorkloadContract`). The
   kernels sit in one table (`simd::KernelTable`) behind `simd::kernel_at`.
3. **a = 1 and 2 on AVX2.** A 256-bit vector holds four doubles; one and two accumulators
   are narrower. The AVX2 entry point runs the scalar sweep for them (the same bits).
   NEON holds two doubles, so it vectorises a = 2, 4, 8 and runs scalar for 1. The AVX2
   kernel is a `target("avx2")` function in a file compiled without `-mfma`; the project's
   `-ffp-contract=off` keeps the NEON build from fusing the multiply and the add.
4. **Scratch is one arena.** `plan().scratch_bytes` is one slot's share, sized to hold the
   whole arena (the in-edge lists, two 256 KiB regions reused by phase, the distances; about
   1.9 MiB plus a cache line to align its start), because with one thread the total is one
   share. With n threads the runner provides n shares and the unit uses the first.
   The cost is memory the unit does not need at n > 1; the alternative, slot-private
   scratch with a shared arena built by one task, would make the unit's memory depend on
   the executor. Recorded in case a runner wants it smaller.
5. **The in-edge lists are built serially.** A counting sort of 262144 edges is a few
   hundred thousand integer operations, an order of magnitude below the sixteen sweeps. A
   parallel build would need per-task histograms and a combine; it would not change a bit
   of the output, and is left for a profile to ask for.
6. **Relaxed atomics in the BFS**, with the argument at the top of `graph.cpp`: `dist[]`
   via `atomic_ref`, one `fetch_add` counter and plain stores to distinct slots; every
   step is bracketed by `Executor::run`. The ordering test is thirty runs on threads that
   claim work in whatever order they get to it (`PullExecutor`), plus TSan. As the plan
   says of P5, that is corroborating evidence, not proof.
7. **P4 and T5/T5b for a module that is not a verifier.** The graph is what the oracle
   checks. T5 therefore injects corruption into the unit's input and output and asserts
   what a checker will see: the classification `verification/record.hpp` assigns to
   single and multiple bit flips of workload-shaped bytes, that every low bit of every
   head word changes the output, and, as a pinned fact, that a bit above bit 14 of a vertex
   word does not (the specification reduces it away; the input-integrity bracket catches
   it). T5b asserts that a `compute()` that has not run is never a pass: no byte of the
   output buffer survives it, another unit's correct answer included, nor does scratch
   residue. The oracle's own T5/T5b belong to the oracle stream.
8. **T3 is the whole output, not a subgraph.** The brief's T3 text mentions a scalar BFS on
   a subgraph; `spec.hpp` says the oracle compares everything because it costs
   milliseconds, so the tests do the same (stronger), plus the triangle inequality on
   every edge.
9. **Executors in the tests are this module's own** (serial, partitioned, pulling), run
   against `ExecutorContract` as the mocks of that contract (ADR 0003). No other
   component's executor, test or mock was read.
10. **Test cost.** Each compute is a few hundred milliseconds in a debug build and
    seconds under coverage with atomic counters, and the contract suite runs it nine or
    more times per subject; the graph binaries are the slowest in the tree. The
    oracle test checks the decompositions {4, 8} on hand-built graphs and all four only on
    generated units, to bound it.
11. **`-fprofile-update=atomic`** is added to the graph library and its test targets in
    coverage builds (this module's CMake fragments), per the earlier waves' lesson.

## Contract questions

1. **`workload.hpp`: may `compute()` use `scratch` as one arena?** The contract says slot
   `s` owns the `s`-th piece, and the executor's `slot` argument exists for per-slot
   scratch. The reading chosen: a workload that needs shared scratch (here the in-edge
   lists every task reads) may use the whole span as one arena provided it is the one
   thread that writes the shared part before `run` and nothing writes it during; the
   per-slot split is a convenience for tasks that need private scratch, not an obligation.
   The unit needs no private scratch at all. If the contract means slots must not be
   crossed, the unit must be restructured and the memory sizing changes.
2. **`workload.hpp`: `Descriptor::version` against the specification's `kVersion`.** The
   header says the descriptor's version moves whenever a configuration's output changes;
   `spec.hpp` says `kVersion` versions the specification. The reading: the descriptor's
   starts equal to `kVersion` and diverges upward the first time a bug fix changes bits
   without a specification change.
3. **`spec.hpp`: `Plan::decomposition` for a request of 0.** Resolved to
   `kDefaultAccumulators` (4); the contract's own test for `config.decomposition == 0`
   accepts any non-zero.
4. **`spec.hpp`, TOTALITY, for `Plan::logical_bytes` and `operations`.** The spec does not
   define either. `logical_bytes` counts the algorithm's traffic as described in
   `graph.cpp` (the input read by the sort and by the BFS, the in-edge lists, the
   distances, per sweep every tail and gathered rank read and every rank written, and the
   output); `operations` is sixteen sweeps of `2E + (a + 1)V` floating-point operations
   plus one examination per edge. Both are estimates for rates, not measurements.

## Intrinsics move to the primitives layer (2026-10-21)

The portability workflow refuses x86 intrinsics outside `src/primitives/simd/x86/` (and
NEON outside `src/primitives/simd/arm/`); `src/workloads/graph/kernels.cpp` used both
directly. Decision, following the dense-linear workload:

1. **Options.** (a) Exempt the file from the check: rejected, the rule is the point of the
   check and dispatch.hpp states it. (b) Move the vector sweeps into the primitives layer as
   dense-linear did: chosen. (c) Rewrite the sweeps with compiler vector extensions in the
   workload: rejected, it hides the instruction choice the bits depend on and changes the
   code under a bit-exact contract.
2. **Result.** `graph_sweep_avx2.{hpp,cpp}` joins the x86 library and `graph_sweep_neon.{hpp,cpp}`
   the ARM one; both libraries already define `LOADFORGE_SIMD_X86_KERNELS` /
   `LOADFORGE_SIMD_ARM_KERNELS` for whatever links them, and the graph library now links
   them when the targets exist, so the table holds a vector sweep exactly when one was built
   (no `__x86_64__` test in the workload any more). `kernels.cpp` keeps the scalar sweep and
   a thin wrapper per path that sends the accumulator counts narrower than a vector (1 for
   AVX2 and NEON, 2 for AVX2) to the scalar sweep, as before. `rank_of`, `add_tail` and
   `kMaxAccumulators`, which every sweep ends with, moved to `kernels.hpp` as inline so the
   scalar and the vector code share one text and the arithmetic is the same operations in
   the same order.
3. **Behaviour.** Unchanged: the same operations in the same order, the AVX2 function still
   without FMA. The only addition is `_mm256_zeroupper()` at the end of the AVX2 sweep, as
   the dense-linear kernel does, which affects speed only.
4. **Ledger.** The module fragments of `primitives/simd/x86` and `arm` and of this module
   say that the sweeps live there and are covered through the graph tests; the tiers are
   unchanged.
