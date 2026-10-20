# Journal fragment 30 — workload: ray tracing

**Written:** 2026-10-20 18:22 UTC
**Change:** `src/workloads/raytrace/` (the workload: scene decoding and generation, the
hierarchy, the packet kernels, the factory), its tests at T1, T3, T4, T5, T5b and T6, the
independent Python model and its digests (`tests/fixtures/workloads/raytrace/`), and the
ledger row `tests/obligations.d/workloads/raytrace.toml` (stream 30, IMPLEMENTATION M4).

## Established by experiment

### The workload's bytes equal an independent model of the specification, on both architectures

`tests/fixtures/workloads/raytrace/reference.py` is written from the text of `spec.hpp` and
`philox.hpp` alone: pure Python floats (binary64, no fused operation), `math.sqrt`, Philox
written out again, every primitive tried for every pixel. It printed one FNV-1a 64 digest of
the whole 66,048-byte output for each of three (seed, unit) pairs — one with the seed's and the
unit's high words nonzero — at each of the three tile edges (`golden.txt`, with the command
and the machine in its header). The C++ workload's output has the same nine digests on the
scalar path and on the AVX2 path (T6, `determinism_test.cpp`). The same nine digests came out
of an `aarch64-linux-gnu-g++-13 -O1` build run under `qemu-aarch64` 8.2, on the scalar path
and on the NEON path: all eighteen lines equal. That covers the specification's definitions
as code (a typo shared by `spec.hpp` and the workload would have shown) and the claim that the
two architectures agree. The emulator is IEEE-exact for these operations but is not hardware;
CI's ARM64 runner checks the same digests on a real core on every run.

### No fused multiply-add anywhere, on either architecture, with both compilers' flags

`objdump -d` of `kernels.cpp` at `-O2 -ffp-contract=off`: 0 `vfmadd`/`vfnmadd`/`vfmsub`/
`vfnmsub` on x86-64 (44 packed `vdivpd`/`vsqrtsd`/`vmulpd` instructions present, so the AVX2
function is real), and 0 `fmadd`/`fmla`/`fmls`/`fmsub`/`fnmadd` on AArch64 (139 `fdiv`/
`fsqrt`/`fmul`). The AVX2 function is marked `target("avx2")` and not `fma`, so the compiler
could not fuse even without the flag. The debug library, 0 as well.

### A function that takes a 32-byte vector by value is not safe to call across targets

The first build of the wide kernel used `std::bit_cast<M>(V)`. It compiled and crashed:
`SIGSEGV` inside `std::bit_cast<long __vector(4), double __vector(4)>` at `-O0`, called from
`wide::select` inlined into the AVX2 row function (gdb, frames #0 to #4). `std::bit_cast` is an
ordinary function emitted for the baseline target; the likely mechanism (not confirmed by
disassembly) is that a 32-byte vector is passed in the registers the *caller's* target
names, which differ from the baseline's. GCC's `-Wpsabi` note names that difference, and
`-Wno-psabi`, which this library sets, only hides it. The rule now written at the top
of `wide.hpp`: every function that touches a vector is `always_inline` into the one function
that chose the target, and the reinterpretation is `__builtin_bit_cast`. Lambdas that took
or returned a vector were replaced by `always_inline` functions and by lane assignment.

### Hierarchy against brute force: the margin was never needed by a generated scene

Sixty units at seed 9, every pixel compared with brute force, the scalar kernel, the boxes
grown by a margin before building:

| margin | wrong pixels of 245,760 |
|---|---|
| +2⁻³⁰ (the one used) | 0 |
| 0 | 0 |
| −10⁻¹², −10⁻⁹, −10⁻⁶, −10⁻³ | 0 |
| −0.04 | 2,254 (0.9%) |

So no generated scene is sensitive to the rounding of the box test at the scales that matter,
and the margin of 2⁻³⁰ is not what keeps the tests green; it is the argument below, and the
`−0.04` row is the evidence that the comparison would notice a hierarchy that skipped
something (T1 `AShrunkenHierarchyCullsWhatBruteForceFinds`, T5 the same with its direction
checked: every wrong pixel is a hit that was skipped, `stored t >= true t`, never an invented
one).

### The generated scene hits 44.4% of its pixels

Two hundred units at seed 1: 0.4440 of 4,096 pixels hit (the specification says about 45%).
The T4 test holds each of 40 units to the specification's band of a tenth to nine tenths, and
the mean to 0.3–0.6.

### Times, for planning the tiers and the stress run (x86-64 Xeon, `-O2`, one thread)

| | per unit |
|---|---|
| scalar path (packets of four, hierarchy) | 0.87 ms |
| AVX2 path | 0.59 ms |
| brute force (the tests' reference) | 5.4 ms |

At `-O0` (every test, sanitizer and coverage build) brute force is about 0.4 s a unit, which
is why the T3 sample is a few dozen units rather than thousands. The wide path is 1.5 times
the scalar one, not 4: the hierarchy's traversal and the node tests are most of the time, and a
packet visits a node when any lane may reach it.

### Coverage

`gcovr` over `src/workloads/raytrace/` with the gate's flags, the five test binaries run
together: lines 438 of 438, functions 60 of 60, branches 116 of 116, spec.hpp's own
functions included. Both the scalar and the wide kernel are instantiated and measured; the
NEON function is not compiled on x86-64 and the AVX2 one is not on AArch64, so each
architecture's job measures its own (the table is the only place the two differ, and it has
no branch).

## Decisions

1. **The scene and the hierarchy live on the calling thread's stack; `scratch_bytes` is 0.**
   `SceneStorage` is about 47 KB: 128 spheres, 128 triangles, at most 511 nodes, the order
   array. *Chosen* because the workload contract lets scratch be 0, because a hierarchy built
   per unit is read-only for the executor's tasks (they all finish before `run()` returns),
   and because it removes every question of the alignment and the lifetime of typed objects
   in a byte buffer. *Rejected:* building in the first slot's piece of scratch — it makes the
   plan's scratch three or four times the hierarchy for every thread count and needs
   `memcpy` access to a buffer of unknown alignment, for nothing the stack does not give.
2. **One executor task per row of pixels (64 tasks), each writing its own 1,024 bytes.**
   *Rejected:* a task per packet (1,024 calls through a function pointer for 4 rays each) and
   a task per tile (ties the work split to the decomposition, which must not change the
   pixels). More than 64 threads on one unit would idle; the runner spreads units too, and
   T6 runs 100 threads to show nothing breaks.
3. **The tile sums are taken after `run()`, on the calling thread, from the stored records.**
   That is what the specification says (a function of the records the unit wrote) and it makes
   the thread count irrelevant to the sums by construction. Their cost is 4,096 reads.
4. **A halving hierarchy, leaves of at most four, built by sorting on the widest centroid
   axis.** The shape depends only on the number of primitives (256: 127 nodes, 64 leaves, 7
   levels), never on values, so the traversal stack is bounded at compile time
   (`static_assert(tree_levels(kPrimitives) + 1 <= kStackDepth)`) for any input, a corrupt one
   included, which `compute()` must be total over. *Rejected:* a surface-area-heuristic
   build (cost grows with values, shape and depth depend on them — a bound on the stack needs
   a proof per input) and a binned build (the same). The sort key is the doubles' total order
   as an integer, ties to the primitive index, so no comparator is ever inconsistent, whatever
   the values, and the order is the same on every standard library.
5. **A box is conservative by 2⁻³⁰ on each side, and a node is visited when any lane of the
   packet may reach it.** The slab test divides nothing and rounds a few times, so a box test
   can reject a ray by about 10⁻¹⁴ that the exact intersection arithmetic would accept; 2⁻³⁰ is
   five orders above that and four below the smallest feature (a radius is at least 2⁻⁴). A
   primitive tried that a lane's own box would have skipped changes nothing, because trying more
   of the specification's tests is what brute force does. The argument is in `packet.hpp`. The
   experiment above shows the margin was not exercised by any generated scene, which is why
   the argument, not the tests, is what it rests on; the residual risk is a triangle whose
   Moller–Trumbore determinant is within rounding of zero and whose computed hit lies outside
   its own box: two independent one-in-10¹³ events at once, and not reachable by any input
   the generator makes in a run's lifetime (about 5·10¹¹ ray-triangle pairs per 10⁶ units).
6. **The wide kernel is portable C++ with the compilers' vector types, in this module, not
   intrinsics under `src/primitives/simd/x86/` and `arm/`.** Four doubles in a lane-wise
   vector; on x86-64 the row function is marked `target("avx2")` (and not FMA), on AArch64 the
   same text is NEON with no attribute. *Why:* the specification's arithmetic is a fixed
   sequence of `+ - * /`, a square root and comparisons, all of which the vector types
   express exactly; one text means the scalar and wide paths cannot drift in structure; and
   there is one file measured on both architectures instead of two kernels each measured on
   one (the completeness gate expects an architecture's kernel directory only there). *Cost:*
   the compiler, not a person, picks instructions (the objdump above shows packed double
   operations on x86-64, not scalar ones). *Rejected:* intrinsics in the kernel directories — a
   second, third and fourth text of the same sequence, none of which could be checked against
   the others except by test, for a speed-up the experiment above says is 1.5 times. The
   `-Wno-psabi` this needs is explained in the experiment above, with the rule that keeps it
   honest.
7. **Lanes evaluate the scalar functions' early returns as masks, never as branches, and
   never divide by zero or take the root of a negative number.** The wide `ray_sphere` takes
   the root of `max(disc, 0)` in the lanes where `disc < 0`; `ray_triangle` divides by `1.0`
   in the lanes whose determinant is zero or NaN; both discard those lanes with the mask. The
   results are the scalar results, and no floating-point exception flag is raised by a
   healthy scene.
8. **`-fno-math-errno` on this library.** A square root taken one lane at a time compiles to
   the instruction plus a branch to libm for the errno of a negative argument, and that branch
   can never be taken by a lane the mask has made non-negative: it would be an uncoverable
   branch (JOURNAL §1.13). The option removes it and changes no result (a square root is
   correctly rounded either way). It applies to the library's translation units only; the
   specification's inline `ray_sphere` is compiled both with and without it in different
   units, with the same bits.
9. **`Descriptor::paths` is the kernel table's paths** — scalar and AVX2 on x86-64, scalar and
   NEON on AArch64, scalar alone in the scalar-only build — and a request for another (AVX-512
   included, on every machine) is `kConfiguration` naming the path. See the contract questions.
10. **The plan:** `logical_bytes` is the scene read once, the output written once, and the
    records' t read again for the tile sums; `operations` is `kPixels * kPrimitives`, the
    number of ray–primitive pairs resolved as brute force would count them, so that a rate is
    comparable between a hierarchy that tries few and one that tries all. `scratch_bytes` is 0
    (decision 1).
11. **The order of refusals is the decomposition, then the path, then the footprint.** Any
    order satisfies the contract; this puts what the user wrote wrongly before what the
    machine lacks before how much room there is.
12. **The factory is `workloads::raytrace::create(const WorkloadConfig&)`**, in
    `src/workloads/raytrace/raytrace.hpp`. The contract does not name the function a
    provider exports; the integration stream's registry will call this one.
13. **The tie rule is one function, `replaces()`:** a candidate must be a hit and lexicographically
    below the best by `(t, primitive index)`. It is the whole of what brute force's "strictly
    less in index order" means when the visiting order is not the index order, and it is tested
    as a truth table and, end to end, with identical primitives at a lower and a higher index
    visited in both orders.
14. **P5 is not claimed in the ledger.** IMPLEMENTATION §3 lists P5 for every M4 workload; this
    one holds no atomic, lock or thread. The executor owns the threads and each task writes
    its own row. The test mocks use `seq_cst` throughout. If a concurrency path appears, the
    row gains P5 and its ordering test with it.
15. **T5 and T5b for a workload that classifies nothing.** The oracle that classifies
    corruption is stream 32's. The workload's T5 injects the faults that can reach *its*
    output — an executor that drops a task, boxes too small, a bit flipped in a record, its
    padding or a tile sum — and asserts which items of a comparison with brute force name
    each (the pixels of the dropped row and the tiles over it, and only those; a flipped record
    bit names that pixel and not its tile, because the stored sum is not touched). T5b is the
    comparison that cannot be made — a buffer of the wrong size, the wrong tile edge — which
    answers not-performed or a mismatch, never agreement. The comparison is the workload's own
    (`tests/oracle/workloads/raytrace/reference.hpp`), shares the specification's arithmetic and
    nothing of the hierarchy, and is not offered as the independent oracle.
16. **Tests that run a path the machine cannot execute fail instead of skipping**
    (`EveryPathTheBuildHasIsRunnableOnThisMachine`): a skipped AVX2 path is a path no test
    ran, and the gate would show the hole later, on another machine.
17. **The wide path's functions are `always_inline` into a row function that carries the
    target, and no header function is compiled for AVX2 outside it.** A unit compiled with
    `-mavx2` would compile every inline function it instantiates under AVX2 and the linker
    could keep that copy for a scalar-path caller (the numerics stream's proposal 06 records
    this); the attribute is per function, so the only AVX2 code is inside `avx2_row`.

## Contract questions

1. **The declared class.** The roadmap and the specification declare ray tracing
   `kBitExactFixedDecomposition`; the specification itself notes that the pixel records are
   identical at every tile edge, on every path and on both architectures, and only the tile
   sums depend on the edge. T6 measured exactly that (pixel records equal across edges 8, 16
   and 32; the output equal across 1 to 100 threads and both paths; equal to an independent
   model). *Reading taken:* the workload is declared as the roadmap declares it and gives
   more than the class names. If the manager prefers `kBitExact`, the tile sums and the
   decomposition leave the header and the workload; nothing else changes. (JOURNAL §2.31
   already carries the question; this is the measurement behind it.)
2. **`select_path()` without a request can choose AVX-512**, for which this workload has no
   kernel and, per the contract, refuses. A run that requested nothing on an AVX-512 machine
   would be refused at creation. *Reading taken:* refuse, as the contract requires; the path
   is chosen before the workload exists, from sets that do not hold the workload's kernels.
   The integration stream will meet this for every workload without an AVX-512 kernel. (Stream
   06 recorded the same.)
3. **`WorkloadContract` does not name the factory's function**, nor how a provider is
   registered; stream 30 exports `workloads::raytrace::create` (decision 12).
4. **The specification's `ray_sphere` is an inline function with `std::sqrt`, compiled with
   different floating-point options in different units.** This library builds with
   `-fno-math-errno` (decision 8) and the test units with the default, so the one inline
   function exists in two forms and the linker keeps one (JOURNAL §1.22 describes the same
   mechanism for a defaulted comparison). The bits are the same; the coverage of `spec.hpp` was
   100% in the measured run. A `-fno-math-errno` for every unit that includes the header, or
   a note in `spec.hpp`, would remove the dependency on which copy is kept; it is not this
   stream's to change.
5. **`tests/oracle/` is named `oracle` and holds T3, but `src/verification/oracle/` is also
   called the oracle.** This stream's T3 tests compare with the workload's own brute force; the
   independent oracle of stream 32 is a different thing, and the ledger note and
   `reference.hpp` say which is which. A name for the tier's reference in the documents would
   remove the doubt.
