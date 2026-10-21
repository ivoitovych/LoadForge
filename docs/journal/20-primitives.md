# Journal fragment 20 — primitives (the calibration engine)

**Written:** 2026-10-20 16:30 UTC
**Change:** stream 20: src/primitives/{integer,floating,cache,memory,branch,atomic}.

## Established by experiment

- **A defaulted `operator==` in a header reads as uncovered** (§1.22 again): `Tally`
  and `Outcome` each lost two branches until the comparison was defined in the module's
  .cpp. A short-circuit `&&` chain there still has an arm per field a test must reach,
  so the comparison is a xor/or of the fields tested against zero: no branch, no bool
  arithmetic (which clang-tidy refuses).
- **`Result::value()`/`error()` can throw, so a `noexcept` function calling them trips
  bugprone-exception-escape.** The modules check with a private `std::optional` refusal
  and compute the value separately; the public `Result` API is built from the same helpers.
- **A `[[nodiscard]]` on a friend declaration that is not a definition is an error under
  -Werror=attributes**; it sits on the out-of-line definition instead.
- **Every module runs on the executor's threads in its tests**, so every primitives
  library and test target takes `-fprofile-update=atomic` under the coverage build; without
  it gcov counters can go negative. Coverage: 100% line, branch (4552/4552) and function.
- 30 of 30 repeated runs of all primitives tests pass (`ctest --repeat until-fail:30`).
  GCC ThreadSanitizer: three runs of each atomic/cache/integer suite clean. Clang
  TSan could not be run here (no `libclang_rt.tsan` in this container, no apt candidate);
  the TSan job in CI is the check for that toolchain. The `tsan` preset's GCC build also
  fails in `heartbeat.cpp` (`atomic_thread_fence`), which is not this stream's.
- The branch kernel's branch may be compiled to a conditional move; the primitive records
  that rather than promising a mispredict per element.

## Decisions

- **Oracles live in the tests, not in src.** A reference in src would need its own coverage
  and would sit beside the code it checks. Each module's T1 test carries an oracle derived
  from the definition: integer squares the affine map instead of stepping; floating replays
  accumulator by accumulator and bounds the rounding by an exact 128-bit integer sum;
  cache builds the cycle on a plain vector; memory recomputes digests from Philox blocks;
  branch filters and reduces with standard algorithms.
- **No allocation in src.** Scratch, partials and storage are caller-owned spans, so there
  is no allocation failure to inject and no landing-pad branches (§1.13). Sizes are
  validated and refused with a named `ConfigError`, never assumed.
- **Determinism classes.** integer, cache, memory, branch, atomic: BitExact. floating:
  BitExactFixedDecomposition (eight accumulators, fixed tree, units added in index order).
  Atomic: totals and ticket sum are BitExact; which unit drew which ticket is not, and is
  deliberately neither compared nor asserted to differ (a scheduler test would be flaky).
- **Floating uses `std::fma`**, not intrinsics: calibration, not a stress kernel (the SIMD
  stream's); correctly rounded, so bits agree across x86-64 and ARM64.
- **Cache geometry** comes from a `topology::Cache` through `geometry_for_level`; an
  unpublished size or a missing/zero line size is refused (`GeometryError`), the first data
  or unified cache at the level is used, instruction caches skipped. P7 is not claimed: that
  state belongs to topology, which reads the source. The real machine's sysfs is also read
  in a test.
- **Atomic orderings** (full arguments in atomic.hpp): counters relaxed (RMW atomicity needs
  no order; nothing else is published through them; results reach the caller through the
  Executor's happens-before); handoff release/acquire on a flag guarding a plain payload.
  Each also runs at seq_cst as the baseline. The handoff never waits (a flag not yet set is
  skipped), because a serial executor would otherwise hang. The reader's check is its own
  branch-free function so a stale read can be shown to be counted. Per PLAN §4.5 the ordering
  test corroborates and cannot prove: a weakening is a TSan race and an ARM64 stale read.
- **`atomic::Cell::payload` is deliberately a plain integer** so a weakened ordering is a
  detectable race rather than a silent pass.
- Line size for sharded counters is an argument; `aligned_storage` aligns by address and
  `contend` refuses misaligned storage.

## Contract questions

- **Executor: does everything the caller wrote before `run()` happen-before every task?**
  The contract states only that tasks' writes happen-before run's return. This stream relies
  on the converse (zeroing atomics and cells before `run`): any executor must publish the
  task and its context to its threads, which publishes earlier writes. Please state it.
- **`Executor::for_each_index` trips bugprone-easily-swappable-parameters**, so named task
  functions with `Executor::run` are used (the task signature itself needs a NOLINT).
- The executor suite header registers one suite; no uninstantiated-suite allowance was needed.
- `workloads/workload.hpp` is included only for `DeterminismClass`; it pulls in
  the whole workload contract. A separate small header for the enum would be lighter.
- `tools/install-git-hooks.sh` does not exist yet; identity was configured by hand.

## Not done

- Clang TSan locally (see above). No process-shared locks, no fixtures: nothing here reads an
  external tool or the kernel except the one sysfs test.
