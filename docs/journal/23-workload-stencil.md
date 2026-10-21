# Journal fragment 23 — workload: stencil

**Written:** 2026-10-20 17:15 UTC
**Change:** `src/workloads/stencil` (the workload and its sweep kernels) and its
tests at every tier it owes.

## Established by experiment

### The kernels give the same bits for every tile edge and every path

Every path evaluates one expression in one association,
`u/2 + ((N + S) + (E + W))/8`, so a cell's result does not depend on where it sits
in its tile, on the lane it lands in, or on the path. Measured, not argued:
`StencilDeterminism.FindingTheseKernelsGiveTheSameBitsForEveryTileEdgeAndPath`
compares the output of scalar and AVX2 at tile edges 16, 32, 64 and 128 for two
seeds, byte for byte, and passes. The scalar digests the fixture commits are
likewise equal across the four tile edges (see
`tests/fixtures/determinism/workloads/stencil/scalar-crc32.txt`: the output CRC
of a (seed, unit) is the same in all four rows). The declared class stays
`BitExactFixedDecomposition`, as the specification requires; see "Contract
questions".

### A fused multiply-add changes nothing here

Every product in the expression is a power of two times a double, so it is exact
and `fma(sum, 1/8, u/2)` rounds exactly as the multiply followed by the add does.
Which path fuses is therefore not a reason for bits to differ; only the
association of the four-neighbour sum could be, and it is fixed. This is why
the vector kernels need no FMA instruction to be what the specification
describes, and why no intrinsic is used (see Decisions).

### The error against an exact integer reference is far under the bound

The oracle tier carries every cell of the grid, not four windows, against the
exact result: the input is a multiple of 2^-52, each sweep divides by 8 exactly,
and 52 + 3 × 8 = 76 bits is held in a signed 128-bit integer with no rounding
anywhere. For five (seed, unit, tile) cases on each path this machine runs, the
largest |output − exact| is at most half of 2 γ₄ₜ M0, which is the share the
derivation gives each computation. (The test asserts the bound; the margin it
leaves is the room a kernel change has.)

### Impulse response is exactly representable

One cell of 1.0 far from the ring gives, after eight sweeps, cells that are
multiples of 8⁻⁸ = 2⁻²⁴ — all exact doubles — that sum to 1.0 exactly. Every
output cell of that test is equal to the integer reference, not within a
tolerance of it.

### Properties that hold bit for bit, and ones that need the tolerance

Bit for bit, on every path and two tile edges: negating the input negates the
output (round-to-nearest-even is symmetric); scaling the input by a power of two
scales the output (no underflow in range); raising the input never lowers the
output (rounded addition and exact halving are monotone); a cell influences only
cells within Manhattan distance eight (checked against the cell's output); a
non-negative input gives a non-negative output; the ring is the input's ring
for arbitrary ring bit patterns; a NaN spreads to exactly the cells within
distance eight. Reflections and the transpose move the output within the bound
rather than bit for bit, because that is what the specification promises: the
association is the kernels' choice, and the test does not pin it.

### Intrinsic-free vector kernels compile to the vector instructions

GCC 13 compiles `double __attribute__((vector_size(32)))` arithmetic inside a
`[[gnu::target("avx2")]]` function to 256-bit instructions, using an
`always_inline` template that is instantiated in the target function so the
lowering happens under AVX2; memcpy-based loads become unaligned vector loads.
On AArch64 the same template with `vector_size(16)` is NEON at baseline. No
`-Wpsabi` or `-Wignored-attributes` diagnostic arises (the vectors are never
passed or returned). Whether the AVX2 path really uses `ymm` registers was
checked on the object file of the release preset, not assumed: `objdump -d` of
`avx2_kernel` shows `vmovupd`, `vaddpd`, `vmulpd` and `vbroadcastsd` on `ymm`
registers, and no `vfmadd`. AArch64 was not available to this stream; its
NEON path is the same template at 16 bytes and is exercised by the CI ARM64 jobs.

## Decisions

### No intrinsics, and so no file under `src/primitives/simd/x86` or `arm`

`dispatch.hpp` says kernels that use an ISA's intrinsics live under
`src/primitives/simd/x86/` or `arm/`, and the portability workflow refuses `_mm*`
names elsewhere. Those directories belong to the dense-linear stream (06); this
brief owns `src/workloads/stencil` only. Options:

1. Add `stencil_avx2.cpp` and `stencil_neon.cpp` to the other stream's
   directories — edits a directory this stream does not own, and a second
   `CMakeLists.txt` there collides with theirs.
2. Write intrinsics under `src/workloads/stencil` — the portability gate fails.
3. Write the kernels with the compilers' portable vector types and a function
   target (chosen).

The sweep's multiplications are by powers of two, so an explicit FMA would not
change a bit (Established above); the property the spec's "AVX2 (FMA3)" names —
the instruction set a path needs — is the probe's, not the kernel's. The cost is
that this stream's AVX2 path does not execute `vfmadd`, and a stress run on it
loads the vector ALU and the load/store ports, not the FMA units. If the project
wants this workload to stress FMA, that is a contract change (the specification
would name the fused form), and the kernel change is a few lines.

### No AVX-512 kernel

The specification names scalar, AVX2 and NEON. `descriptor().paths` is the
table's `kernel_paths()`, so asking for AVX-512 is refused with `kConfiguration`
and a detail naming the path.

### One executor run per sweep, one task per tile

A sweep reads a grid and writes another, so the return of `Executor::run` is the
barrier between sweeps, with nothing for the workload to synchronise itself. A
task is a tile of the configured edge: 256 tasks at edge 16, 4 at edge 128, with
the ring cells it covers copied by the task that covers them. An alternative —
a task per row band — would make the decomposition independent of the tile
edge; the specification makes the tile edge the decomposition, so tiles it is.
Tasks are called by `Executor::run` with a named function, not `for_each_index`
(the frozen header's own overload trips `bugprone-easily-swappable-parameters`).

### Scratch is one grid per slot, and one slot's is used

`compute()` is given `scratch_bytes × threads()`. The sweeps need one more grid
to alternate with (eight sweeps, an even count, so the last lands in the output),
which cannot be per slot: every tile of a sweep writes into the same grid. The
plan therefore says `scratch_bytes = kGridBytes`, `compute()` uses the first
grid's worth of whatever it is given, and the rest is unused. Wasteful at seven
threads (3.5 MiB for 0.5 MiB used), harmless at the footprint's scale, and the
runner sizes the footprint to leave room for scratch as it is. A contract that
allowed a total rather than a per-slot figure would remove the waste.

### Reads and writes of doubles go through `memcpy`

The runner guarantees cache-line alignment for input and output, nothing for
scratch, and the contract suite's own buffers are `std::vector<std::byte>`
(16-byte alignment). `std::memcpy` into a local double or a vector value is
alignment-free, strict-aliasing-clean and compiles to the same loads.

### Plan figures

`logical_bytes = kSteps × 2 × kGridBytes` (each sweep reads a grid and writes
one: the algorithm's traffic, not the cache's). `operations = kSteps × 254² × 6`
(three additions for the neighbour sum, one addition and one multiplication for
`u/2`, one multiplication for `/8`, counted as the expression is written).

### Tests share one support header across the module's tiers

`tests/unit/workloads/stencil/stencil_support.hpp` holds the executors, the
exact reference and the sampled-window check, and is included by the module's
five test executables. ADR 0003 is about sharing between components; the
support is written from `spec.hpp`, never from the kernels, which is the
independence that matters (verification.md §3).

## Contract questions

1. **The class is weaker than the algorithm supports.** The kernels' bits do not
   depend on the tile edge or on the path (Established by experiment), so the
   declared class could be `BitExact`: the same bits across thread counts,
   decompositions and ISA paths on one architecture, and across architectures on
   every path whose lanes only repeat the scalar expression. `spec.hpp` says what
   to do with this finding — record it, revisit by contract change, never
   strengthen inside the stream — and that is all this entry does. The
   workload declares `kBitExactFixedDecomposition`.
   The finding holds for this association. A kernel that changed the sum's
   association with the lane (a reduction tree over a tile, say) would lose it.
2. **`Plan::scratch_bytes` is per slot, the stencil needs one shared grid.** See
   Decisions. A total-bytes figure, or a statement that scratch may be used
   across slots, would let a workload that needs shared scratch say so
   without paying for every thread.
3. **`WorkloadFault::kConfiguration` for a path compiled but not supported.**
   `create()` checks that the build has a kernel for `config.isa` and nothing
   about the machine: selecting a path the CPU cannot run is `select_path()`'s
   job, before the workload exists (dispatch.hpp). A test of this module runs
   AVX2 only when `__builtin_cpu_supports("avx2")`.
4. **The portability rule and the brief's ownership disagree** (Decisions): the
   rule names directories that belong to another stream, the specification says
   the stencil kernels live "in src/workloads/stencil/", and the two cannot both
   hold for an intrinsic kernel. Intrinsic-free vector kernels satisfy both.
