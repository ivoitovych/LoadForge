# Journal fragment 06 — workload: dense linear algebra, and the first vector kernels

**Written:** 2026-10-20 11:20 UTC
**Change:** `src/workloads/dense_linear` (the workload and its scalar kernel),
`src/primitives/simd/x86` (the AVX2/FMA3 kernel), `src/primitives/simd/arm` (the NEON
kernel), their tests at T1, T3, T4, T5 and T6, and the coverage gate on both
architectures (stream 06, IMPLEMENTATION M2b).

## Established by experiment

### An unoptimised build leaves the ymm registers dirty, and the SSE code after it pays

The kernel's whole-product test took 14 s in the debug build, against 0.05 s for the same
loop in a separate program. `gdb` samples put the time in the test's own scalar model — plain
SSE arithmetic and `__fma_fma3` — after the AVX2 kernel had run. GCC inserts `vzeroupper`
on the way out of a function that used 256-bit registers only when it optimises: the debug
object of `gemm_avx2.cpp` contained no `vzeroupper` at all (`objdump -d | grep -c
vzeroupper` gave 0; the `-O2` object gave 2). With the upper halves left dirty, every
following non-VEX instruction carries a false dependency or a transition penalty. An explicit
`_mm256_zeroupper()` at the end of the kernel took the test from 14.0 s to 0.6 s. Every test,
coverage and sanitizer build is unoptimised, so the cost would have landed on every tier and
every later module that runs after a kernel.

### A vector type as a template argument loses its attributes, and GCC says so

`std::array<__m256d, 2>` fails the build under `-Werror`: GCC warns `ignoring attributes on
template argument '__m256d'` (`-Wignored-attributes`), since `__m256d` is a typedef carrying
`vector_size` and `aligned` attributes that a template argument does not keep. A struct of two
named `__m256d` members, in a `std::array` of the struct, does not warn. NEON's `float64x2_t`
is a distinct built-in type on AArch64 GCC and `std::array<float64x2_t, 4>` compiles cleanly
with the same flags (checked with `aarch64-linux-gnu-g++-13`).

### Clang refuses `__attribute__((target(...))) [[nodiscard]]`; the other order is fine

With the GNU attribute first, Clang 18 reads the following `[[nodiscard]]` as applying to the
return type and errors (`'nodiscard' attribute cannot be applied to types`); GCC accepts both
orders. `[[nodiscard]] __attribute__((target("avx2,fma")))` compiles under both.

### The fused paths give the same bits on both architectures; so does the scalar one

The workload's output digests (CRC-32 of unit 0 at seed 1, every panel depth) were computed
on an x86-64 Xeon with the scalar and AVX2 kernels, and with the same tests cross-built by
`aarch64-linux-gnu-g++-13` and run under `qemu-aarch64` 8.2 with the scalar and NEON
kernels. The scalar lines agreed, and the NEON lines equalled the AVX2 lines, at every depth
(`tests/fixtures/golden/workloads/dense_linear/output-crc32.txt`). This follows from the
kernels' order: every entry is a chain of `fma(a, b, p)` in increasing k from +0.0, added to C
once per panel, and an FMA is one correctly rounded operation on both ISAs; nothing depends on
the vector width, because lanes hold different entries, never partial sums of one entry.
docs/determinism.md promises bit-identity only within a path; this workload gives more, and
its T6 tier asserts what it gives. The emulator is IEEE-exact for these operations, but it is
not hardware: CI's ARM64 runner checks the same digests on a real core on every run.

### The NEON kernel reaches 100% on AArch64 before CI sees it

The coverage build cross-compiled with `aarch64-linux-gnu-g++-13`, this stream's seven test
executables run under qemu, and gcovr reading with `aarch64-linux-gnu-gcov-13`, filtered to
`src/workloads/dense_linear/` and `src/primitives/simd/arm/`: 134 of 134 lines, 21 of 21
functions, 74 of 74 branches. The rest of `src/` was not measured that way, since its
real-machine tiers would read the x86-64 host's `/sys` through the emulator. The ARM64
coverage job is the first measurement of it there.

### No FMA in the scalar kernel, on either compiler or architecture

`objdump -d` of `scalar_kernel.cpp` compiled at `-O2 -ffp-contract=off` shows no `vfmadd`
on x86-64 and no `fmadd`/`fmla` on AArch64; the AVX2 kernel at `-O2` shows `vfmadd` (two
with GCC, eight with Clang, which unrolls the row loop). The panel-kernel suite pins the
same fact by value: a two-term product whose fused and unfused results differ (`+0.0`
against `2^-60`) gives `+0.0` from the scalar kernel and `2^-60` from both vector kernels.

### The derived bound is tight enough to mean something

Over every entry of one unit, the largest `|c - c'| / tolerance` between the workload and
the tests' naive loop was 0.0071 at the default depth, on the scalar and the AVX2 path alike
(printed by `tests/oracle/workloads/dense_linear/oracle_test.cpp`). The bound is not
vacuous — the oracle tier asserts the worst ratio stays above 10⁻⁶ — and a flipped exponent
or upper mantissa bit of C breaches it while the lowest mantissa bit does not
(`tests/fault_injection/workloads/dense_linear/fault_test.cpp`).

### gcov's counters race when the code they count runs on several threads

The first coverage run of the whole tree passed every test and then failed in gcovr:
`gemm_avx2.cpp:70 Got negative hit value in: branch 1 taken -144255 (fallthrough)`. The
determinism tier runs the kernels and the workload's tile function on up to seven threads at
once, and `--coverage` counts with plain read-modify-write increments, so concurrent
increments are lost; gcov derives a fall-through count by subtraction, and with a loop's
counter short by more than its exit's, the difference came out negative (GCC bug 68080 is
the same mechanism). `-fprofile-update=atomic` on the three libraries of this stream, in
coverage builds only, made the report parse and read 100% (5557 lines, 1199 functions,
4493 branches). `cmake/Coverage.cmake` is not this stream's, so the flag is added in its own
fragments; any later module whose code runs concurrently under test will meet the same
failure, and the shared function is the better home for it.

### clang-tidy over the kernels: one check fires on some intrinsics, and a frozen template trips another

- `portability-simd-intrinsics` flagged `_mm256_add_pd` (but not `_mm256_fmadd_pd`,
  `_mm256_loadu_pd` or `_mm256_set1_pd`): the check names the intrinsics that have a
  `std::experimental::simd` spelling. Its purpose is to keep intrinsics out of portable code,
  which the kernel directories are by definition not; each has a `.clang-tidy` that inherits
  the root configuration and removes that one check.
- `worker::for_each_index` instantiated from `src/` fails `bugprone-easily-swappable-parameters`
  at `src/worker/executor.hpp:79`, the lambda it wraps around the body taking two adjacent
  `std::size_t`. The diagnostic is in a frozen header, where no `NOLINT` can be written, and
  `HeaderFilterRegex` reports it to every `src/` unit that instantiates the template. The
  workload calls `Executor::run` with a named function instead, with its own `NOLINTNEXTLINE`
  (see Contract questions).

### The portability gate reads comments

The scalar-only job's "no x86 intrinsics outside src/primitives/simd/x86/" step is a `grep`
over the raw text, so `panel.hpp`'s comment naming the AVX2 intrinsic by its spelling
failed it, run locally before any push. The comment now names the instruction in words.
Erring towards refusal is right for that gate; a header outside the kernel directories
describes kernels without spelling their intrinsics.

### Times, for planning the tiers

At `-O0` one unit takes about 180 ms on the scalar path and 40 ms on the AVX2 path (Xeon @
2.1 GHz). Under qemu-aarch64 the same tests run about thirty times slower; the determinism
binary took 3 min 25 s there against 8 s natively.

## Decisions

### The kernel's unit of work is one panel of one row tile, and the panel's sum starts at zero

A kernel call updates `kTileRows` = 4 rows of C, all 256 columns, with one panel:
`c_ij <- c_ij + p_ij`, `p_ij` summed from +0.0 in increasing k (`panel.hpp`). This is the
shape of the micro-kernel in BLIS-style GEMM — the panel product computed in registers, then
used to update C — and it is what spec.hpp's "accumulates panel by panel" describes: each
panel's sum is formed, then added. *Rejected:* loading C into the accumulators and continuing
the chain across panels. It costs the same, but it would make the panel depth change nothing
at all, so the decomposition the specification and the determinism class name would be
decorative; and it would make the tile's first panel a special case (load C or start at
zero). The kernel granularity is a whole row tile rather than one 4×8 register block so that
the function-pointer call happens 64 × n/kc times per unit, not 2048 × n/kc times.

### The scalar kernel does not fuse

Scalar is `p <- p + (a * b)`, two roundings. It names no intrinsic, and determinism.md §4
reserves FMA for where it is named. `std::fma` would be correct but, without `-mfma`, a
library call per multiply-add on x86-64 — the one thing a stress kernel must not be — and
the scalar path's value is that it is plain portable arithmetic, bit-identical across
architectures. The specification's bound covers both.

### AVX2 by a target attribute, not by compiling the unit with `-mavx2 -mfma`

Inline functions from headers (`std::span::operator[]`, `std::memcpy` wrappers, `std::array`)
are instantiated in every unit that uses them, and the linker keeps one copy. A copy compiled
under `-mavx2` can be the one kept, and then a scalar-path caller executes AVX2 on a machine
without it — the classic ODR hazard of per-file ISA flags. With
`__attribute__((target("avx2,fma")))` on the kernel and its three helpers, only their own code
is AVX2, and the unit needs no special flags. NEON needs nothing: Advanced SIMD is baseline
AArch64.

### No AVX-512 kernel

The contract's `compiled_paths()` lists AVX-512 on x86-64, and the table has no kernel for it,
so `create()` refuses it as `kConfiguration` — refused, never replaced. An AVX-512 kernel
would be measured only on runners that happen to have AVX-512, and GitHub's x86-64 runners
do not all do (AMD EPYC Zen 3 does not); the coverage gate would then pass or fail with the
runner it was given. It belongs in a later change with a runner that guarantees it, and the
brief made it optional.

### Byte buffers are read by copy or by alias-safe vector loads, never through a `double*`

No `double` object is ever created in the runner's buffers. Scalar code copies with
`std::memcpy`; AVX2 uses `_mm256_loadu_pd`/`_mm256_storeu_pd`, which GCC and Clang define
through a `may_alias` type; NEON copies whole `float64x2_t` with `std::memcpy`, which compiles
to the same `ldr q`/`str q` without depending on how an implementation types its load
builtin's pointer. All loads are unaligned, since the tests' buffers promise less than the
runner's. `panel.hpp` asserts `std::endian::native == std::endian::little`; `generate()`
writes bytes by shifting, so it does not depend on the machine.

### No packing, no scratch

`Plan::scratch_bytes` is 0: tiles read A and B in place. At n = 256 each matrix is 512 KiB
and a panel of B is at most that; packing would add a copy per tile and a second
implementation of the layout for little at this size. The contract's "scratch prefilled with
ones" test then passes trivially, and nothing in the output can depend on a slot.

### The plan's figures

`logical_bytes` is the compulsory traffic, A and B read once and C written once: 3 n² × 8 =
1.5 MiB. The per-panel reload of C and the re-reading of B by each tile are properties of
this blocking, not of the algorithm, and depend on caches the figure must not need.
`operations` is 2 n³, an FMA counted as the multiply and add it replaces, so the rate is
comparable across paths.

### `create()` takes the kernel table as an argument

`create(config)` uses the build's table; `create(config, kernels)` takes any. The second is
how the tests reach the refusal of a table without its scalar kernel, a descriptor listing a
table's own paths, and the driver itself: a recording kernel shows that each tile is zeroed
and then takes exactly n/kc panels in increasing k, at every depth.

### Classes and tiers: P4 and T5b removed, T5 kept

IMPLEMENTATION §3 lists P4 (verification outcomes) and T5b (unrun checks) for every
workload. This module performs no verification: the oracle and the tolerance are the
verification streams' (`src/verification/oracle/dense_linear/`, `src/verification/residual/`),
and they owe P4, T5 and T5b. The fragment removes P4 and T5b with that reason. It keeps T5,
because what the workload owes the doctrine is that corruption reaches the output where the
arithmetic carries it — a row for an entry of A, a column for an entry of B, eight columns for
a cache line, a NaN row for a NaN — and no further, and that the bounded check then sees it;
the fault-injection tier asserts each, including the §4a case where the corrupted input makes
the corrupted output self-consistent.

### The panel kernels share one suite, written from `panel.hpp`

`tests/unit/workloads/dense_linear/panel_kernel_suite.hpp` holds a typed suite every kernel
is instantiated against — scalar here, AVX2 and NEON in their modules — comparing every entry
bit for bit with a one-loop model (`std::fma`, or product then sum). That is ADR 0003's
provider-side suite for the one interface between the workload and its kernels: three
implementations, one statement. Because `std::fma` is correctly rounded everywhere, a kernel
equal to the fused model on its machine is equal to the other ISA's kernel too.

### Tests of a path the machine cannot run fail; they do not skip

An x86-64 machine without AVX2 would build the AVX2 kernel and could not run it. The suites
assert `runs_here()` and the contract subject refuses creation with that reason, so such a
machine reports failures, not a clean run that never touched the path. Every CI runner has
AVX2.

### Golden digests are committed for both kinds of path

The determinism tier reads `output-crc32.txt`: one scalar and one fused CRC-32 per depth.
They are tripwires (verification.md §4), generated by the code, and say so in the file. The
test prints every line it computes before comparing, so a new machine's run shows all of its
lines in the file's own format.

### Coverage on both architectures

The coverage job runs on `ubuntu-24.04` and `ubuntu-24.04-arm`. The completeness check reads
the architecture from `uname -m` (overridable for its tests by `LOADFORGE_COVERAGE_ARCH`) and
excuses `src/primitives/simd/arm/` on x86-64, `src/primitives/simd/x86/` on AArch64, and both
elsewhere; every other source is still required everywhere. The NEON unit is guarded by
`__aarch64__` and the AVX2 unit by `__x86_64__`, so a tool that reads every file in `src/` on
the other architecture (clang-tidy in the format job) sees an empty unit.

### A proposed ADR

The way vector kernels are built, guarded, tested and measured is what every later workload
will copy, and three parts of it look removable to someone who was not here (the target
attribute, the explicit `vzeroupper`, the per-architecture coverage expectation):
`docs/adr/proposed/06-workload-dense-linear-vector-kernels.md`.

## Contract questions

1. **`for_each_index` cannot be used from `src/` under the project's clang-tidy.**
   (`src/worker/executor.hpp`.) Its lambda `(void*, std::size_t, std::size_t)` fails
   `bugprone-easily-swappable-parameters`, reported at line 79 of the frozen header to every
   `src/` unit that instantiates it. *Reading taken:* call `Executor::run` with a named
   function and suppress the check there. *Proposed correction:* a `NOLINTNEXTLINE` on that
   line of the header, or the slot passed as a distinct type.
2. **Selecting a path a workload has no kernel for.** (`src/primitives/simd/dispatch.hpp`.)
   Unrequested, `select_path()` prefers AVX-512 wherever the machine and build have it, and
   this workload has no AVX-512 kernel, so a run on an AVX-512 machine that requested nothing
   would be refused at creation. The path is chosen before the workload exists, from sets that
   do not include the workload's kernels. *Reading taken:* refuse, as the contract requires.
   *For the integration stream:* the unrequested choice needs the workload's
   `descriptor().paths` (or `kernel_paths()`) intersected into `compiled`, which the current
   signature allows but no document asks for.
3. **"Each C entry accumulates panel by panel."** (`spec.hpp`.) Whether a panel's sum starts
   at zero and is then added to c, or continues c's chain, is not stated; both are "panel by
   panel in increasing k". *Reading taken:* from zero, then added (Decisions, first entry).
   The oracle is unaffected: the bound holds for any order.
4. **A path the build has and the machine cannot run.** (`tests/contracts/workloads/workload_contract.hpp`.)
   The suite runs `compute()` on whatever `Subject::config()` names; a subject for AVX2 on a
   machine without it would die by `SIGILL`. *Reading taken:* the subject's `create()` refuses
   with a `kConfiguration` naming the reason, so the suite fails readably. A `runs_here()` on
   the subject would let the suite say so itself.
5. **`Plan::logical_bytes` for a blocked algorithm.** (`src/workloads/workload.hpp`.) "The
   memory traffic one unit's computation performs by its algorithm" could mean the compulsory
   traffic or the traffic of this blocking. *Reading taken:* compulsory (Decisions, the plan's
   figures).
6. **Coverage instrumentation of concurrent code.** (`cmake/Coverage.cmake`, frozen to
   streams.) `loadforge_enable_coverage()` compiles with `--coverage` alone, whose counters
   race under threads (Established by experiment). *Reading taken:* this stream's fragments
   add `-fprofile-update=atomic` to its own libraries in coverage builds. *Proposed
   correction:* add it in the shared function, for every target.
