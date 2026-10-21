# ADR (proposed) — How vector kernels are built, tested and measured

**Status:** proposed
**Date:** 2026-10-20 11:20 UTC
**Stream:** 06, workload: dense linear algebra (the manager numbers this when it lands)

## Context

The dense-linear workload brings the first ISA-specific code into the tree: an AVX2/FMA3
kernel under `src/primitives/simd/x86/` and a NEON kernel under `src/primitives/simd/arm/`,
beside a portable scalar kernel, chosen through `simd::KernelTable`. Every later workload
with vector kernels — FFT, stencil, particles, Monte-Carlo — will copy what this one does.
Three of its choices look removable to someone who was not there when they were made, and
each removal is a defect that no gate would catch at once:

- the AVX2 code is enabled by a function attribute, not by compiling its unit with `-mavx2`;
- the AVX2 kernel ends with an explicit `_mm256_zeroupper()` that an optimising build would
  have inserted anyway;
- the coverage gate expects each architecture's kernel directory only on that architecture,
  and runs on both.

## Decision

1. **ISA code is enabled per function.** An x86 kernel and its helpers carry
   `__attribute__((target("...")))`; their translation unit is compiled with the project's
   ordinary flags. A unit compiled with `-mavx2` also compiles, under AVX2, every inline
   function it instantiates from a header, and the linker may keep that copy for every
   caller — so a scalar-path caller on a machine without AVX2 would execute AVX2. AArch64
   needs no flag for Advanced SIMD, which is baseline.
2. **A kernel that uses 256-bit registers ends with `_mm256_zeroupper()`.** GCC inserts
   `vzeroupper` only when optimising; in the unoptimised builds every test, coverage and
   sanitizer job uses, the dirty upper halves made the SSE code after the kernel some twenty
   times slower (measured: 14 s against 0.6 s).
3. **Kernel directories are built only on their architecture, and each unit is guarded by
   its architecture's macro**, so that a tool reading every file under `src/` on the other
   architecture sees an empty unit. The library defines a macro
   (`LOADFORGE_SIMD_X86_KERNELS`, `LOADFORGE_SIMD_ARM_KERNELS`) for its dependants, and a
   kernel table names a kernel exactly when the macro is defined. Neither directory is built
   with `LOADFORGE_SIMD=scalar`. Each directory carries a `.clang-tidy` that inherits the root
   configuration less `portability-simd-intrinsics`, the one check whose purpose is to keep
   intrinsics out of the directories that are not these.
4. **Coverage runs on x86-64 and ARM64**, and the completeness check expects
   `src/primitives/simd/x86/` only on x86-64 and `src/primitives/simd/arm/` only on AArch64;
   every other source on both. A kernel is therefore measured to 100% on the one machine
   that can run it, and nowhere is a file excused that the machine compiles.
5. **Kernels are tested against a portable model, bit for bit**, and a vector kernel's model
   uses `std::fma`, which is correctly rounded on every machine. A kernel equal to the model
   on its own architecture is then equal to the other architecture's kernel too, and a
   golden digest can hold the two to each other across CI's runners. A kernel whose order
   cannot be stated as such a model (a horizontal reduction across lanes, say) states its
   own, per path, and its digests are per path.
6. **A test of a path the machine cannot execute fails rather than skips**: a path that was
   never run has not been tested, and the coverage gate would not see the skip until later.

## Consequences

- Each new vector kernel brings its target attribute, its guard, its `zeroupper` where it
  uses ymm or zmm registers, and an instantiation of its workload's kernel suite in its own
  directory's tests.
- An AVX-512 kernel cannot be held to 100% coverage until a CI runner guarantees AVX-512;
  until then a workload refuses that path (`kConfiguration`), as the dispatch contract
  requires.
- The coverage job costs a second runner. In exchange the rest of `src/` is held to 100% on
  ARM64 too, where it was not measured before.
- `select_path()` without a request can choose a path a workload has no kernel for; the
  journal fragment records this for the integration stream.

## Alternatives rejected

- **`-mavx2 -mfma` on the kernel's unit** — the ODR hazard of decision 1.
- **Function multiversioning (`target_clones`)** — the path is chosen once, before the
  workload exists, and recorded (dispatch.hpp); an ifunc resolver choosing again at load
  time would be a second, unrecorded choice.
- **Excusing the ARM kernels from coverage on x86-64 by an exclusion list** — an exclusion
  removes a file from the denominator everywhere, including on the machine that can measure
  it. The architecture-scoped expectation measures it there and nowhere else.
- **Cross-compiling and emulating ARM64 in the coverage job** — slower by about thirty
  times, and an emulator's view of memory ordering and timing is not the hardware's; the
  real ARM64 runner exists.
