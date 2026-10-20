# Journal fragment 26 — workload: particles

**Written:** 2026-10-20 17:25 UTC
**Stream:** 26, branch `feature/workload-particles`
**Change:** `src/workloads/particles/`, its tests under every tier directory it owes, its
ledger fragment.

## Established by experiment

### A 32-byte vector passed by value is a compile error in a unit built without AVX

The lane kernel is one template, instantiated with four lanes (`Lanes4`) and with two
(`Lanes2`). The first version gave each lane type a `static Mask index()` and a
`static double sum(Real)`. The AVX2 unit compiled; the unit test, which instantiates the
four-lane kernel without `-mavx2` so that the code is tested on a machine and in a build
that need not have AVX2, stopped with `AVX vector return without AVX enabled changes the
ABI [-Werror=psabi]` at both functions. Passing the vector by `const&` and making the
index a `static constexpr` member removed both. The rule that follows: nothing in
`lane_kernel.hpp` takes or returns a vector by value across a function boundary.

### `-mavx2` on one source file puts AVX encodings in every inline function that file emits

Checked with `objdump -d` on the objects of the component: 93 `ymm` instructions in
`kernel_vector.cpp.o` and none in the other five. That holds because the unit includes
nothing but `kernel.hpp` (types and declarations) and `lane_kernel.hpp` (templates in an
unnamed namespace, calling no standard function). Had the unit included `<span>`,
`<array>` or `dispatch.hpp` and used one of their inline functions, the unit would have
emitted its own copy of that function with VEX encodings, as a weak symbol the linker may
choose over the other units' copies, for every caller — an illegal-instruction fault on a
machine without AVX2, in a function that has nothing to do with AVX2. `std::bit_cast` is
such a function; the kernel uses `__builtin_bit_cast`, which is not.

### The pruning of the cell stencil is exact in cells and was proven by two independent routes

Cells of edge `rc / c`: two cells `d` apart along an axis are at least `max(|d| - 1, 0)`
edges apart, so a cell offset can hold a neighbour only if the sum of the three squared
gaps is below `c^2`. That leaves 27 of 27 cells for `c = 1`, 125 of 125 for `c = 2` and
613 of 729 for `c = 4` (the four rows `(±4, ±4)` vanish, and the rows at gaps 4 and 3
shorten to three and two cells). The unit test derives the covered set from this rule
independently of `make_row_table` and compares them for every offset; the property test
places a pair at the nearest corners of the two cells of every offset (inset by 2^-20) and
asserts the force on each is the single term of the specification, to the bit. To see the
tests can fail, `dx_max` was made one smaller in the table builder: 21 of the 48 pair
tests and 2 of the grid tests failed. The mutation was reverted.

### A pair marginally outside the cutoff in exact arithmetic can have a computed `t` just above zero

The pruning keeps a cell if its nearest corner is closer than `rc` in exact arithmetic. A
pair in a *dropped* cell has exact `q >= 4`; its computed `q^` can be a few units of
`gamma_5` below 4, making `t^` about `2^-51` and the term about `2^-102`. The all-pairs
oracle includes that term; the cell search does not. The term is more than 80 orders of
magnitude below `E` (the error of one term, 3.8e-15, which the tolerance already carries
for every candidate), so the omission is inside the derived bound. The pair test
`AtTheCutoffThereIsNoForceAndJustInsideThereIsATinyOne` shows the size: the double below 2
gives a force of about `-2^-102`.

### The residual is under one percent of the tolerance

Over every 32nd particle of unit 2 (seed 5), in all three decompositions and both paths,
the worst residual against the direct sum is 0.0061 of the derived tolerance (0.0050 to
0.0061; measured with a throwaway program linked against the debug library). The bound is
a worst case over `M_p` terms and the observed error is rounding, so a hundredfold margin
is as expected; `EveryThirtySecondParticleAgreesAndTheBoundIsNotLoose` asserts under 0.1.
The T5 test relies on that margin: a flipped bit that moves a force by more than twice the
tolerance must be caught and one that moves it by less than a quarter must go unseen, for
each of the 64 bits of each of the 48 entries, on both paths. The bits above about 2^-12
relative are caught; the lowest three or four mantissa bits are not, which is the "what
the bound does not claim" paragraph of `spec.hpp` made concrete.

### A gtest test body is a member function of a `testing::Test`

`Run` (the first name of a range of sorted particles) and `Setup` (a test-local struct) both
collided with members of `::testing::Test` inside `TEST` bodies — `Test::Run()` and
`Test::Setup()` are private there, and the compiler reports "private within this context"
rather than a redeclaration. `NeighbourRun` and `Variant` replaced them.
`INSTANTIATE_TYPED_TEST_SUITE_P` also pastes its suite argument into identifiers, so a
qualified `::loadforge::contracts::WorkloadContract` does not compile; the instantiation is
written inside `namespace loadforge::contracts`.

## Decisions

### The vector kernel uses the compilers' vector types, not intrinsics (and so has no FMA)

The brief names scalar, AVX2 (FMA3) and NEON kernels. Intrinsics may appear only under
`src/primitives/simd/x86/` (the portability workflow refuses `_mm256_*` anywhere else), a
directory the dense-linear stream owns, and an ARM-only source file is not compiled on the
x86-64 coverage run, so the coverage gate would not see it (ADR 0004 gives that problem to
the dense-linear stream in wave 1). Options:

1. An intrinsics file under `src/primitives/simd/x86/` — rejected: not this stream's
   directory, and its CMake fragment is another stream's.
2. GCC and clang builtins for the fused operation — rejected: compiler-specific names that
   would be the only such lines in the tree, for a gain the specification does not ask for.
3. One template over the portable vector types (`double __attribute__((vector_size(N)))`),
   instantiated with four lanes for AVX2 and two for NEON. Taken.

The consequences, which are the cost of the choice: the AVX2 path needs AVX2 only (it
carries the name of the path that the contract describes as AVX2 *with* FMA3, a superset
of what it uses), the kernel does not saturate the FMA units, and the specification's
permission to fuse the last product into the sum is not used. `-ffp-contract=off` holds, so
no operation is fused behind the code's back. The same template is instantiated in the
unit tests with two lanes on x86-64, so the NEON arrangement is exercised on every CI
machine and not only on ARM64.

### What the lanes add, and in what order

Each lane keeps its own accumulator per component; candidate `j` of a run goes to lane
`(j - run.begin) mod lanes`; a run's last vector is masked, so lanes past its end add
nothing; at the end of a particle the lanes are combined `(l0 + l1) + (l2 + l3)` or
`l0 + l1`. The unit tests carry a model of exactly this and compare it with the kernel bit
for bit, so the order is pinned, not merely described. The scalar kernel has one
accumulator and takes the terms in the order of the runs.

The runs are the rows of cells of the stencil (increasing z, then y), each a contiguous
range of the cell-sorted particles; empty runs are dropped. A particle's terms are thus
added in an order that depends on the decomposition and the path and on nothing else,
which is what the class asks for.

### A term is active unless `t <= 0`, so a NaN is active

The specification says "if `t^ <= 0` the term is zero; otherwise …". For a NaN `t` the
comparison is false, so the term is formed and is NaN. Both kernels follow that wording
(the vector one by `~(t <= 0)` on the mask), so a NaN or infinite coordinate propagates as
NaN into the forces of the particles that meet it, deterministically, instead of being
dropped. An infinite separation gives `t = -inf` and no term. This is only reachable
through corrupt input, which the contract requires `compute()` to take; the tests assert it
is taken, is deterministic, and is the same at any thread count and scratch content.

### The self term is included

The sum is over `j != i`. The kernel includes `j = i`: `d` is exactly `+0`, `q` is 0, `t` is
1, and the term is `1 * +0 = +0`, which adds nothing, so for every finite coordinate the
result is the same bits as excluding it, and skipping it would cost a compare and a mask in
the innermost loop. For a NaN or infinite coordinate the self term is NaN, so such a
particle's force is NaN even with no other particle near it; that is the same class of
corrupt-input behaviour as above and is covered by the same totality tests.

### Corrupt coordinates are clamped to the nearest cell

`cell_coordinate` sends NaN and anything below 0 to cell 0 and anything not below
`kBoxEdge` to the last cell. Every particle is therefore in a cell, `cell_start` is a
permutation of `[0, n]`, the work is at most `n` terms per particle (`n^2` for the case of
all particles in one cell, which the property tests run) and no index leaves its array,
whatever the bytes. The pair set is no longer exact for such a particle, but a unit with
such a coordinate is `NotChecked` by the oracle, and the input-integrity bracket is what
reports it.

### The scratch is one shared structure, built before the tasks run

`scratch_bytes` is the layout of the sorted structure (cell starts, particle order, the
three sorted coordinate arrays with `kMaxLanes` zeros of padding, and the forces in sorted
order) plus 63 bytes so that the carving can start at the first 64-byte-aligned address.
The calling thread bins the particles into the first piece of the scratch before it calls
`Executor::run()`, the tasks only read it and write disjoint force slots, and the calling
thread scatters the forces back to the input's order afterwards. The reading of "slot `s`
owns the `s`-th piece" under which this is allowed: the pieces belong to slots *while tasks
run*; between `run()` calls the calling thread owns the whole span. `scratch_bytes` does
not depend on the thread count, so the plan is the same for every executor, and a run with
seven threads uses the first piece of seven.

### Tasks are rows of cells; the executor is called with a named function

A task is a row `(iy, iz)` of the grid: 64, 256 or 1024 tasks for `c = 1, 2, 4`, enough to
balance seven threads at `c = 1` too. The results are per particle, so no partition can
change a bit. The task is a free function taking its context through `void*` and passed to
`Executor::run()` (the frozen `for_each_index` trips `bugprone-easily-swappable-parameters`
in the header, a lesson of wave 1).

### `Plan::operations` and `Plan::logical_bytes`

`operations` is 17 floating-point operations per candidate pair (3 differences, 3 squares
and 2 sums for `q`, a product and a difference for `t`, one square, 3 products and 3 sums
into the force), times the candidate pairs: `n` particles, each meeting the `n / cells`
particles a cell holds on average in each of the cells its stencil visits (27, 125 or 613).
It is a function of the decomposition only, as the contract requires of a plan, and not of
the data. `logical_bytes` is the input read once, the output written once, and the scratch
written once and read once.

### `Descriptor::paths` is derived from the kernel table

`simd::kernel_paths(cell_kernels())`, so the paths a descriptor claims and the paths the
factory accepts cannot disagree; `WorkloadContract` checks the other direction (a path
without a kernel is refused, not replaced). `LOADFORGE_SIMD=scalar` builds have the scalar
kernel only; x86-64 has AVX2 (not AVX-512, which this stream does not write); AArch64 has
NEON.

### What T5 and T5b mean for a workload that owns no verification path

The ledger table lists P4 and T5/T5b for every workload. The particles code owns no
verification path (the runner and the oracle stream own them) but its output is what they
classify. The stream's tests therefore implement the *check of the specification* — the
direct sum, the derived tolerance, the verdict `Agreement`, `BoundedMismatch` naming the
first failing entry, and `NotChecked` — in `tests/oracle/workloads/particles/direct_check.hpp`,
from `spec.hpp` alone, and use it as a verification path would be used: T3 shows the
workload's output agrees with it; T5 injects faults (every bit of every checked entry,
two corrupt entries in the reverse of the checking order, a zeroed, NaN, infinite,
shifted, input-valued and other-unit output, an input corrupted between generate and
compute) and asserts the classification; T5b asserts the check that could not run is not a
pass. This is a second reading of the check, independent of the oracle stream's, which is
the point of ADR 0003; it is not the oracle and nothing in `src/` uses it.

### P5 is absent

The workload starts no thread, holds no atomic and no lock. The Executor contract's
happens-before (everything each call wrote happens-before `run()` returns) is the only
synchronisation it relies on and it relies on it twice: tasks read what the calling thread
wrote before `run()`, and the calling thread reads what the tasks wrote after. The tests
that run it on a thread pool run under ThreadSanitizer in the gate. The brief's table lists
P1, P3, P4 and the ledger says the same.

### The golden units

`tests/fixtures/determinism/workloads/particles/golden.txt` holds, for four units per
path, the FNV-1a digest of the generated input and of the computed output and the bits of
the first force. It was written by running the workload itself (the fixture's header says
how and on what machine), is a tripwire and not an oracle, and has rows for the scalar and
AVX2 paths. The scalar rows are the cross-architecture check of determinism.md §3: the
ARM64 job reads the same file. There is no NEON row: one would have to be produced on an
ARM64 machine and this session has none; the fixture says so rather than inventing it, and
the NEON path is covered by the thread sweep and the tolerance tests.

### The registry is not this stream's

`particles::create()` is the factory. The registry stream links it by `kName`.

## Contract questions

### Does `create()` check that the machine can run the path?

`WorkloadContract` says a workload refuses an ISA path it has no *kernel* for. The workload
has no `IsaProbe`, so `create()` with `kAvx2` succeeds on a CPU without AVX2 and
`compute()` would then fault. Reading taken: the worker has already done `select_path()`
(dispatch.hpp: "chosen once, before a workload is created") and a workload is entitled to
trust it. The tests make the same assumption: `runnable_paths()` in the tests asks the CPU
before using the AVX2 kernel, and the typed-suite subject falls back to the scalar path on
a machine without it. A suite that asked a workload for a path the machine lacks would
need the contract to say what is expected.

### The specification header's own test names its default decomposition as "One"

`tests/contracts/workloads/spec_particles_test.cpp` names a test
`CellsPerCutoffAreOneTwoAndFourAndTheDefaultIsOne` and asserts `kDefaultCellsPerCutoff == 2`
(and the header says 2). The assertion and the header agree; the name is wrong. Worked to
2. Not mine to edit.

### "Scratch … slot `s` owns the `s`-th piece" and a structure built before the tasks

See "The scratch is one shared structure". The wording of `workload.hpp` could be read to
forbid the calling thread from using the whole span between `run()` calls; the reading
taken is that ownership by slot is for tasks. If it was meant to forbid it, a workload with
a global pre-pass needs `scratch_bytes` to include a shared region and a way to say so.

### Does the spec's AVX2 path mean "uses FMA"?

`dispatch.hpp` describes `kAvx2` as "AVX2 with FMA3". The specification says the sum may
be done "with or without fused multiply-add". The path here needs AVX2 and uses no FMA
instruction; a later FMA kernel for the same path would change the bits of that path and
move `Descriptor::version`. Recorded so the choice is not mistaken for an oversight.

### Self term in `spec.hpp`

The sum is over `j != i`; the kernel includes `i` and the bits are the same for every
finite coordinate (see Decisions). If the oracle stream reads "exclude" as a requirement
for a corrupt coordinate (a NaN particle with no neighbours), the two disagree on a verdict
that is `NotChecked` anyway.

## Left open

- No NEON golden row, and the NEON build has not been compiled by this stream: there is no
  ARM64 machine here. The code is the same template as the AVX2 kernel with two lanes, the
  two-lane instantiation is tested on x86-64, and the ARM64 CI job is the first run of
  `kernel_vector.cpp` with `LOADFORGE_PARTICLES_NEON` defined.
- The mutation workflow has not been run on this module (it is the integration streams').
