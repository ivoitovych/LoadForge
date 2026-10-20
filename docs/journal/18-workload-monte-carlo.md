# Journal fragment — stream 18, workload: Monte Carlo

**Written:** 2026-10-20 15:55 UTC

## Established by experiment

- **The vector order is reproduced by an implementation in another language.**
  `tests/fixtures/workloads/monte_carlo/make_golden.py` (Python floats, its
  own Philox, checked first against the Random123 vectors in
  `tests/fixtures/rng/philox4x32-10.kat`) produces the 64 batch sums of one
  unit for 1, 2 and 4 accumulators and every chunk length. The scalar kernel
  (1 lane) and the AVX2 kernel (4 lanes) match all of it bit for bit on this
  machine (x86-64, Intel Xeon with AVX2 and AVX-512, GCC 13.3). The 2-lane
  rows are for the NEON kernel; they run on the ARM64 CI jobs and nowhere
  else, so that is the first time they are compared.
- **Detection floor of the specified check.** Flipping mantissa bits 0 to 13
  of a batch sum is not seen by the tolerance 2^-38; bits 15 and up, the
  exponent and the sign always are (bit 14 is on the boundary and is neither
  asserted nor relied on). Pinned in `tests/fault_injection`.
- **Input corruption is invisible to the specified check by design.** The
  output computed from a corrupted key agrees with that corrupted input and
  disagrees only with the regenerated one; the input-integrity bracket is what
  catches it. Tested both ways.
- `tools/install-git-hooks.sh`, named in the stream instructions, does not
  exist in this tree; nothing was installed.

## Decisions

1. **No AVX-512 kernel (descriptor paths are scalar plus the build's one
   vector path).** Options: (a) an 8-lane kernel behind `target("avx512f")`;
   (b) none. Its lines cannot run, hence cannot be covered, on a runner
   without AVX-512 (GitHub's x86 runners are a mix), and the rule is 100%
   with zero exclusions. Chose (b); `create()` refuses `kAvx512` as
   `kConfiguration`, as the contract requires of a path without a kernel.
   Adding it needs a coverage runner that is guaranteed to have the feature.
2. **Lane-structured loops instead of intrinsics.** Intrinsics may live only
   under `src/primitives/simd/x86/`, which this stream does not own. Each
   path is the same lane loop (`lane_sum<Path>`) with the path's lane count;
   the bits depend on the lane count, not on whether the compiler uses
   vector registers. The AVX2 entry carries `[[gnu::target("avx2")]]` on a
   thin wrapper and the shared body is `always_inline`, so it is compiled for
   AVX2 only there; a whole-file `-mavx2` would let the linker keep an
   AVX2-compiled copy of an inline Philox function for the scalar path and
   fault on a CPU without AVX2. The instantiation is keyed on a type local to
   the translation unit so its copy is never merged with another's.
   `-fno-math-errno` on this target removes the errno arm of `sqrt`; results
   are unchanged. NEON has no attribute (baseline on AArch64): 2 lanes.
3. **Parallelism is per batch** (64 tasks), not per chunk: scratch is per
   executor slot, so chunk sums of a batch could not be shared across tasks
   without scratch the contract does not give. `scratch_bytes` is 0.
4. **Plan numbers.** `logical_bytes` = input + output (the samples are
   generated, never stored); `operations` = samples, 2^20 per unit.
5. **Order of refusals** in `create()`: path, then decomposition, then
   footprint. The worker has already checked the CPU supports the path; the
   workload does not check again.
6. **T5/T5b for a workload with no verifier.** The oracle stream owns the
   real check. This module's tests contain a copy of the specified check
   (`tests/oracle/.../replay.hpp`, exact 128-bit reference sum) and inject
   corruption into output, input and work, asserting its classification;
   T5b is evidence of the wrong size → NotChecked, nothing compared.
7. **Preconditions are not checked.** Spans of the wrong size are a runner
   bug the contract lets the workload ignore; a guard would be a branch only
   a death test could reach, and a forked child's coverage is lost (JOURNAL
   §1.9).
8. **AVX2 availability in tests** is read with `__builtin_cpu_supports`; on a
   CPU without AVX2 the tests use the scalar path for the "native" subject, so
   the suite passes but the AVX2 lines go unexecuted there (coverage would
   read below 100% on such a machine).

## Contract questions

- `spec.hpp` says an unsupported value is refused "by the workload and by the
  oracle" and the plan "reports the resolved C"; read as: `Plan::decomposition`
  is never 0. Done.
- `WorkloadContract` takes its `Subject::config()` footprint as holding two
  units; a vector-path subject is only meaningful where the CPU runs it, and
  the suite has no way to skip. Worked around by letting the subject fall back
  to the scalar path (decision 8). A `Subject::supported()` hook would be
  cleaner.
- The INSTANTIATE macros must be written in the contract's namespace and take
  a type-list alias (a bare `Types<A, B>` splits into macro arguments);
  worth a line in the contracts' README.
