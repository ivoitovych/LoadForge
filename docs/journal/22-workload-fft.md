# Journal fragment 22 — workload: FFT

**Written:** 2026-10-20 17:30 UTC
**Change:** `src/workloads/fft/`, its tests in every tier it owes, its ledger fragment.

## Established by experiment

### A table built from one octant equals one computed entry by entry

The twiddle table is committed as the first octant, m in [0, 512], 513 rows of bits;
the other seven octants are exact images (swap of parts, sign change), applied at compile
time. To show the images lose nothing, `tests/fixtures/fft/gen_twiddles.py full` computes
every W[m], m in [0, 4096), from its own angle with 120 digits and no symmetry, and
`unit/twiddle_test.cpp` compares all 4096 pairs of bits against the table the header
builds. They agree. Every Re and Im that is exactly zero is +0.0, and the table holds
exactly four zero parts (cos at n/4 and 3n/4, sin at 0 and n/2).

The first version of the fixture generator lost the sign of every negative value:
`mpf.man_exp` returns the magnitude's mantissa, not a signed one. The test caught it on
the first run (every Im W[m], m in (0, n/2), disagreed in the sign bit). Fixed by taking
the sign from the value.

### The bound is a worst case; the error sits far below it

Over whole units at all 4096 bins against a long double direct DFT, the largest
component error is under a twentieth of the derived T_bin for both radices
(`oracle/oracle_test.cpp` asserts `worst < T/20`). Parseval's residual over 40 seeds
stays under a hundredth of T_parseval. The specification's bounds are loose by a factor
the tests could use for early warning if a kernel ever lost accuracy without breaking
the bound.

### The Parseval boundary cannot be placed within 2^-20 of T' by any output

The specification asks the oracle stream to test the bound "breached at (1 + epsilon) T'
and held at (1 - epsilon) T'" for epsilon = 2^-20. For the Parseval check the residual is
a difference of two doubles near 64 a (about 3300 for these inputs), so it moves in steps
of one ulp, about 4.5e-13, while T' is about 6e-9: a step is about 7e-5 of T', coarser
than 2^-20. Scaling the whole output by 1 + d and bisecting d to the last bit finds the
flip; the residuals either side are within a few ulp of each other and of T'
(`fault_injection/fault_test.cpp`). The epsilon claim is achievable for the bin check
(the residual there is a difference of numbers near 1, ulp 1e-16 against T' about 1e-10)
and is tested there. The oracle stream should construct its Parseval boundary tests by
choosing the *residual* (a synthetic output with a chosen norm), not by perturbing a real
transform, or relax epsilon for that check. Recorded under Contract questions.

### The scalar digests

The committed digests (FNV-1a 64 of generated input and computed output) are the same at
-O0 (debug preset) and -O2 (the release preset, RelWithDebInfo), with GCC 13 and with Clang 18. See the numbers in
`determinism/determinism_test.cpp`. ARM64 is not available in this container; the claim
that the scalar path matches there rests on the table being the same bits and every
operation being a single IEEE operation under `-ffp-contract=off` (no fused multiply-add,
no libm, no x87 since the target is x86-64 SSE2). The ARM64 CI job runs the same digests
and will say if that reading is wrong.

### The gates, run on this change (2026-10-20, x86-64, GCC 13.3 and Clang 18)

* Debug build and `ctest`: 1134 of 1134 pass. Release (GCC and Clang) and the scalar-only
  preset: pass. The static build links.
* Coverage: lines 100.0% (5622 of 5622), functions 100.0% (1227 of 1227), branches 100.0%
  (4494 of 4494), all 38 source files in the report, exit 0. The first run was 99.9%: a
  temporary `Descriptor` (a `std::string` inside it) alive across `make_unique`'s
  allocation left two never-executed cleanup branches (journal 1.13's mechanism), and
  `kernel_at` instantiated for this module's table had its out-of-enumeration return
  uncovered (the contract tests cover only their own instantiation). The descriptor is
  now built inside the workload's constructor, and a test asks the table for a path
  outside the enumeration.
* ASan+UBSan and TSan with Clang 18, over this module's five test binaries (125 tests):
  clean. The container lacked `libclang-rt-18-dev`; installed it.
* Each of the five test binaries, repeated 30 times in the debug build: 30 of 30 passes.
* clang-tidy-18 over the module's sources and all its test files from a Clang-configured
  tree: clean. clang-format-18, Ruff, the ledger gate, `tests/tools/test_tooling.sh` (158
  of 158): pass. ShellCheck is not installed here and no shell script was added.

## Decisions

### Stockham autosort, decimation in time

Options: (a) in-place radix-r with a digit-reversal permutation; (b) Stockham autosort
decimation in frequency; (c) Stockham decimation in time. The specification's stage
multiplies the butterfly's *inputs* by twiddles and then forms the r-point DFT, which is
decimation in time, so (c) matches the derivation of the tolerance literally (stage_error
is derived for `B D^ z`, the twiddles before the butterflies). Stockham keeps both input
and output in natural order with no permutation pass, at the price of a second buffer.
The buffer is the unit's scratch, and the stages ping-pong between it and the output so
that the last stage lands in the output without a copy. The index mapping, derived in
`kernel.hpp`: butterfly (v, c) reads complex indices `v r M + c + a M`, multiplies input
a by `W[a v M s]`, writes output b at `(v + b L) M + c`.

### One executor run per stage; the task count does not depend on the thread count

A stage's butterflies are independent (each reads r elements and writes r elements no
other butterfly touches), so splitting them over threads cannot change a bit. The split is
into tasks of 256 butterflies regardless of `threads()`, so the *decomposition* of the work
is a function of the radix alone, as the class requires, and a test counts the tasks at
one thread and at five and finds them equal. The executor's return is the barrier between
stages. `Executor::run` is called with a named task function, not `for_each_index`, per the
earlier waves' note about `bugprone-easily-swappable-parameters` in the frozen header.

### Scratch: one buffer, in the first slot's piece

`compute()` receives `scratch_bytes * threads()`. The stages need one 64 KiB ping-pong
buffer for the whole unit, shared by the tasks (they write disjoint elements), not one per
slot. The plan declares `scratch_bytes = 65536`, the buffer is the first piece, and the
other pieces are untouched (tested). The cost is that the runner reserves 64 KiB per
thread of which only the first is used. A `scratch_bytes` of 0 is not possible: the
algorithm needs a second buffer. If the runner's memory accounting makes that waste
matter, the alternative is a per-unit scratch in the contract, which is a contract change.

### Bytes, not doubles, at the boundary

Inputs and outputs are `std::span<std::byte>` with only cache-line alignment promised, and
the contract says compute is total over corrupt input. Loads and stores assemble each
double from its eight little-endian bytes (`kernel.cpp`), which compilers fuse into one
plain load or store at -O2; there is no aliasing cast, no alignment assumption and no
dependence on the machine's byte order. At -O0 it is slower, which the debug build
tolerates (a unit's whole test is milliseconds).

### Twiddles are read for every non-first input, even at v = 0

The specification says the first input of a butterfly is multiplied "by nothing"; the
others are multiplied by table entries, W[0] = (1, +0) included. Multiplying by (1, +0)
is not an identity for non-finite or negative-zero inputs (`x * 0` is NaN for infinite x),
but it is within the derivation and it keeps one code path. The first input is not
multiplied.

### Operations are counted as 5 n log2 n

`Plan::operations` is the conventional FFT flop count, 245760, the same for both radices so
that rates compare across decompositions. `logical_bytes` is each stage reading and
writing the whole 64 KiB vector: stages x 2 x 65536.

### No AVX2 or NEON kernel in this stream; the seam is in place

The specification header describes scalar, AVX2 (FMA3) and NEON kernels. The portability
workflow refuses `_mm*` intrinsics anywhere in `src/` outside `src/primitives/simd/x86/`
(and by the dispatch contract's own rule the ARM ones belong under `.../arm/`), and this
stream owns `src/workloads/fft/` and nothing under `src/primitives/`. Writing intrinsics
inside the workload's directory would fail CI; writing them under `src/primitives/simd/`
would touch files this brief does not give me. Vector extensions or compiler-vectorised
loops would not be the FMA-using kernels the specification means, and `-ffp-contract=off`
forbids letting the compiler contract. So:

* The stage kernel is `StageKernel`, held in a `simd::KernelTable<StageKernel>` per radix
  (`stage_kernels(radix)`), with `scalar` filled and the other three null.
* `create()` looks the configured path up with `kernel_at()` and refuses a null with
  `kConfiguration` naming the path; `Descriptor::paths` is `kernel_paths()` of the table.
  A vector kernel is added by filling its slot and writing its tests; nothing else changes.
* The vector kernels owe the tolerance of the specification (they may fuse), and their
  own T6 (same bits across thread counts for one path). That work is open and is listed in
  the report.

### P5 is declared

The brief's table lists P1, P3, P4. The stage barrier is the one ordering the design
depends on (a stage reads everything the previous one wrote), and the repeat and
thread-count tests exercise it under real threads, so the ledger also declares P5, with
the argument in the note: the barrier is the executor's contract; the kernels write
disjoint elements.

### The checker in the tests is the stream's own

The oracle stream writes `src/verification/oracle/fft/`. The tests here need a check to
inject faults against, so `tests/oracle/workloads/fft/fft_support.hpp` implements the
specification's 17 checks from `spec.hpp` alone (never from the kernels, never from the
oracle stream's code, which this stream may not read). It is test code, not the oracle.

## Contract questions

1. **Where do workload vector kernels live?** `spec.hpp` says the stress implementation has
   AVX2 and NEON kernels "behind the dispatch seam (src/workloads/fft/)"; `dispatch.hpp`
   and the portability workflow say intrinsics live only under `src/primitives/simd/x86/`
   or `.../arm/`. Reading chosen: the kernels' intrinsic code is a primitives-stream file
   and the workload owns only the table slots and tests. Not reconciled by this stream.
2. **Parseval epsilon.** See "Established by experiment": for the Parseval check the
   residual is quantised by an ulp of `s^`, about 7e-5 of T', so a boundary test at
   epsilon = 2^-20 needs a synthetic residual, not a perturbed output.
3. **Scratch per slot versus per unit.** `Plan::scratch_bytes` is "per executor slot", but a
   unit-level ping-pong buffer is per call. Reading chosen: the first slot's piece is the
   buffer; the rest are unused.
4. **Re-entrancy of `compute()` with the runner's executor.** The contract says a task must
   not call `run()` on the executor that is running it. If the verified runner spreads
   *units* over the executor and calls `compute()` from inside those tasks, `compute()`'s own
   `run()` calls are reentrant and a precondition violation. Reading chosen: the runner
   calls `compute()` from a thread that is not inside a `run()` of the executor it hands
   over, or hands a distinct executor. If the runner means to parallelise over units,
   the fft workload needs a way to run its stages serially (a `threads() == 1` executor
   per unit works today, and is what the contract suite's concurrent-units test uses).
