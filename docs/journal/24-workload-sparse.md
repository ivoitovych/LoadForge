# Journal fragment 24 — the sparse workload

**Written:** 2026-10-20 17:31 UTC
**Change:** `src/workloads/sparse/` (the workload, its scalar kernels and the fixed reduction
tree) and its tests at tiers T1, T3, T4, T5, T5b and T6.

## Established by experiment

### The residual the specification could not prove

Specification derivation 2 takes the exact-arithmetic bound on conjugate gradients,
`||r_K||_2 <= 3 * 5^-16 ||b||_2 = 1.97e-11 ||b||_2`, as the bound for floating point, and flags
it as an assumption the sparse stream is to measure. Measured on the scalar path with the
explicit residual the workload itself writes (`r^`): 2000 solves, seeds 0 to 249, units 0 and 1,
at each of the four chunk sizes.

| | `||r^||_2 / ||b||_2` |
|---|---|
| largest (seed 138, chunk rows 2048, unit 0 or 1) | 2.250e-16 |
| mean | 2.188e-16 |
| the specification's bound | 1.966e-11 |

The bound is held by a factor of 8.7e4 at the worst of 2000 solves, and every solve sits within
a few percent of the mean: the residual is the attainable one, `u ||A||_2 ||x||_2`, from the
first iterations on. No seed or chunk size approaches the bound, and the largest ratio at one
chunk size is within 2% of the largest at any other, so the chunk size does not move it. The
assumption holds on the scalar path; the vector paths do not exist yet (below), so the
measurement over "every ISA path" is the scalar path's alone. The command is a scratch program
that links `sparse.cpp` and `kernels.cpp` at `-O2 -ffp-contract=off` and loops over seeds and
chunk sizes, run on x86-64 (GCC 13.3, a 4-core virtual machine). The T3 tests assert the bound
on 4 seeds and a margin of three orders (`1e-14`) so that a regression that costs
precision shows well before the specification's bound is near.

### The chunk size changes the bits, and the difference is the derived one

Across the four chunk sizes the outputs of one unit are different byte strings (the
decomposition is part of the bits, which is the class), and the iterates differ by far less
than `2 tau / lambda_min` with `tau = 3 * 5^-16 ||b||_2`, the bound that follows from both being
within `||r||_2 / lambda_min` of the solution. The T6 test asserts both facts.

### A scalar path with no libm and no contraction is one function of its input

The scalar output of unit 2 of seed `0x5EED` has these CRC-32 digests, which the T6 test
records: 231517602 (chunk rows 256), 77685606 (512), 2838486056 (1024), 4027288572 (2048).
They were first recorded from a debug build with GCC 13.3 and are asserted by every
configuration the gate builds (GCC and Clang, debug, coverage, sanitizers, optimised) on x86-64;
the portability workflow's ARM64 job is the one that shows they hold across architectures. They
are a tripwire (verification.md §4), not an oracle.

### A throwing constructor puts a branch on the line of a `new`

With the `SparseWorkload` constructor not declared `noexcept`, `return
std::unique_ptr<Workload>(new SparseWorkload(...))` carried two never-taken branches in the
coverage report (the null check before the `operator delete` the compiler emits for a
constructor that throws: the constructor makes a `std::string`). Declaring the constructor
`noexcept` removed them, which is §1.13's mechanism from the other side: the `bad_alloc` of the
string now ends the process, as the allocation of the workload itself would.

## Decisions

### Scalar path only; the vector kernels are not here

*Options.* (a) Write AVX2 and NEON kernels in `src/workloads/sparse/`. (b) Write them under
`src/primitives/simd/x86/` and `arm/`. (c) Ship the scalar path behind the dispatch table and
leave the vector paths to a stream that owns those directories. (d) A "vector" kernel in portable
code with lane-shaped accumulators.

*Chosen: (c).* The sparse specification and the brief say the workload has scalar, AVX2 (FMA3)
and NEON kernels; `dispatch.hpp` says kernels that use intrinsics live under
`src/primitives/simd/x86/` or `arm/`, and `portability.yml` fails the build on `_mm256_` and the
other x86 spellings anywhere else under `src/`. (a) is refused by that workflow. (b) puts
files in directories this stream does not own, which rule 4 of the brief forbids. (d) would be a
path named after an instruction set it does not use: Descriptor::paths would claim AVX2 for code
the CPU runs as SSE2. So the workload's kernel table, `kKernelTable` in `sparse.cpp`, holds the
scalar kernel set, `Descriptor::paths` is what the table holds (`simd::kernel_paths`), and a
request for any other path is refused with `kConfiguration` naming it. A vector kernel set is
one more entry in the table and a function in `primitives/simd/`; nothing else in the workload
changes, because the kernels are the five steps of one chunk (`Kernels` in `kernels.hpp`) and
the solver owns the shape. See "Contract questions". NEON could not be built or run in this
environment in any case (x86-64 only).

### The decomposition is the chunk, and a chunk is one task

Chunk rows `C` in {256, 512, 1024, 2048} divide 16384 into 64 to 8 chunks, always a power of
two. Each chunk is one index of `Executor::run`; the task writes only its own partial-sum slot
(`partials[chunk]`) and its own rows of the vectors, so no two tasks write the same bytes and
which thread ran which chunk is not in the result. The partials are combined by `reduce_tree`
on the calling thread, after `run` returns, in the specification's order. One iteration is
three `run` calls (`q = A p` with `p . q`; the `x` and `r` updates with `r . r`; the `p`
update), with a barrier between them because the SpMV reads `p` at distances up to 1031 rows.
The unit makes 1 + 3 * 16 + 1 = 50 calls. *Rejected:* fusing the `p` update into the next
iteration's SpMV, which needs rows outside the chunk; a persistent worker per slot, which is the
executor's business (it is written once, by its own stream).

### Reading a corrupt matrix: mask the column, clamp the row

`compute()` is total over input bytes, and the matrix is input. A column is read as
`column & (kRows - 1)` (kRows is a power of two, asserted), so any 32-bit value gathers from
inside the vector and a valid column is unchanged; a row's entries are `[row_ptr[i],
row_ptr[i + 1])` clamped into `[0, kNonZeros)` and cut at 7 entries, so a row_ptr out of order,
out of range or enormous costs at most a valid row's work and a valid row_ptr is unchanged.
*Rejected:* validating the structure once and falling back to the known structure (it would let
the kernel stop reading `row_ptr` and `columns`, which are part of the traffic the workload
exists to generate, and would hide a corrupt column from the output); trapping.

### The explicit residual is formed in the finishing kernel

`r^_i = b_i - sum_j a_ij x^_j` is `b_i` minus each product in ascending column order, each
product rounded and then subtracted (no FMA), in the same pass that copies `x^` to the output
(the `finish` kernel). The specification allows any order with or without FMA; the plain
ordering keeps the scalar path identical across architectures.

### Scratch

`Plan::scratch_bytes` is "per executor slot", and the workload cannot know the slot count when
it plans. A unit's `x`, `r`, `p`, `q` and chunk partials are one state shared by every task, not
per-slot state, so the workload takes the first slot's piece (`scratch_bytes`, 525,320 bytes:
`4 * 16384 + 64` doubles and 8 bytes to align) and leaves the rest untouched; the other
`threads() - 1` pieces are scratch the runner allocates and the workload does not use. This is
the cheapest reading of the contract that keeps one piece sufficient for a unit.

### The plan's traffic and operations

`logical_bytes` counts every operand of each step read or written once: the init (5 vectors),
16 iterations of one SpMV (`112430 * (8 + 4 + 8) + 16385 * 4` bytes: value, column, the gathered
`x`, and `row_ptr`) plus 13 vectors, and the finishing SpMV plus 3 vectors; it is 67,651,932.
`operations` is 2 per stored entry per SpMV and the vector steps' 2 + 4 + 2 + 2 + 2 per row per
iteration, plus 2 per row for `r_0 . r_0`: 6,476,828. Both are written out as numbers in the
unit test.

### Descriptor

`version` 1 (Descriptor::version, the bits' version, starts at 1 with the workload and is
independent of `kVersion`, the specification's). `determinism`
`kBitExactFixedDecomposition`, `contract` `kBounded` (the specification's oracle contract),
`paths` `{scalar}`.

### What T5 and T5b mean for a workload with no verification code

The oracle for sparse is another stream's. The brief says T3 here is "a scalar SpMV and the
explicit residual", so the sparse tests carry their own reading of the specification's check
(`tests/oracle/workloads/sparse/spec_check.hpp`), written from `spec.hpp` and nothing in
`src/workloads/sparse/`, and use it for the three tiers that need a verdict. T5 injects bit flips
into the iterate and the explicit residual, NaN and infinity, and asserts which check fires and
at which row (the first row that reads a corrupted `x^_j` is `j` minus the largest distance that
fits), asserts a self-consistent unconverged iterate passes every row check and fails the norm,
and asserts that a bit flipped in the *input* before the solve is solved faithfully and passes
the residual check -- the limit verification.md §4a names, shown as a fact together with the
fact that the output differs from the clean unit's, so a regeneration finds it. T5b makes the
check impossible (every `NotChecked` condition of the specification, one case each) and
asserts the verdict has nothing compared. The tests prove the workload's output can be judged
and corruption of it classified; they are not the oracle, which the oracle stream writes.

### No P5 and no P2

The workload makes no system call and allocates nothing in `compute()`, and has no atomic,
lock or ordering protocol: the one concurrency fact it relies on is the executor's (`run`
returns after every task and their writes happen-before it), which its T6 tests exercise on
real threads under TSan. The brief's table lists P1, P3, P4; the ledger fragment says why not P5.

### Tests use their own executors, and share nothing but the header

The tests' executors (`tests/unit/workloads/sparse/executors.hpp`) are serial, reverse-order and
a pool that starts `n` threads per `run` and has them claim indices from a shared counter with
sequentially consistent operations, so which thread runs which chunk changes from run to run.
`ExecutorContract` runs against each (`executor_mock_test.cpp`). The T3 to T6 tiers include
that header and `oracle/workloads/sparse/{spec_check,unit_run}.hpp` by path from `tests/`; both
are inside this stream's directories.

## Contract questions

1. **Where do the vector kernels live?** `spec.hpp` and the brief place AVX2 and NEON kernels in
   the sparse module; `dispatch.hpp` and `portability.yml` place intrinsics under
   `src/primitives/simd/x86/` and `arm/`. The reading taken is the repository's (kernels there),
   which leaves the vector paths to a stream that owns those directories. To close it, either a
   contract change assigning `src/primitives/simd/{x86,arm}/sparse_*.cpp` to this stream, or a
   second stream for them. The kernel table in `sparse.cpp` is where they plug in.
2. **Derivation 2 is an assumption.** Measured above: it holds with a factor of 8.7e4 on the
   scalar path. A tighter constant than `3 * 5^-16` could be taken from a bound of the
   attainable residual (`~1e-15 ||b||`) if the oracle stream wanted a tolerance that catches
   more; that is the contract's to decide.
3. **`Plan::scratch_bytes` is per slot, and a unit's state is not.** Read as above (first slot's
   piece). A contract that said "the first piece, `scratch_bytes`, is the unit's and the rest are
   per-slot" would say what every parallel workload will do.
4. **`verification.md` §3 says sparse is "Parallel CG with preconditioner".** The
   specification says there is none (the diagonal is constant, so Jacobi would scale every
   vector by 1/2 and change nothing). Followed the specification; the table in `verification.md`
   is stale.
5. **`tools/install-git-hooks.sh` does not exist** in this tree, so the hook install step was
   skipped; commits are signed off and carry only that trailer by hand.
