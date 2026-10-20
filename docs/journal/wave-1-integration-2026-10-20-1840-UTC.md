# Wave 1 integration: the contract questions, decided

**Date:** 2026-10-20 18:40 UTC
**Branch:** `chore/contract-corrections-wave1`
**Authority:** ADR 0004, "Changing a contract" (header, suite and reference change together,
every in-tree instantiation still passing, the reason in the header and in the journal),
and ROADMAP's prerequisite for wave 2: "any contract corrections wave 1's streams recorded".

Each line is one question a wave-1 fragment recorded under "Contract questions": the
decision, and where it lives. Numbers are the fragment's own. **Taken** means the change is
in this branch; **Deferred** names the stream that owns it; **Accepted** means the
fragment's reading stands as recorded and no contract text changes; **Reading stands** is the
same for questions the decision list did not name (classified here, on the ground that the
fragment's reading is the narrowest one and no consumer depends on another).

## Taken now

1. One suite per contract header, in `tests/contracts/`. `workload_contract.hpp` and the
   new `registry_contract.hpp`; `oracle_contract.hpp` and `oracle_registry_contract.hpp`
   (`OracleCase` and its helpers moved to `oracle_case.hpp`, which registers no suite, so a
   test that only builds units does not meet an uninstantiated suite); `source_contract.hpp`
   and `timeline_contract.hpp` (was `telemetry_contract.hpp`); `fp_check_contract.hpp` and
   `fp_control_contract.hpp` (was `fpenv_contract.hpp`); `select_path_contract.hpp` and
   `isa_probe_contract.hpp` (was `dispatch_contract.hpp`). Every include in `tests/` follows;
   every `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST` that existed only for a two-suite
   header is gone. Kept: `SummarizerContract` in `tests/property/statistics/` (a one-suite
   header, included for its helpers), and `PanelKernelSuite` / `PanelKernelDeterminism`
   (the dense-linear stream's own headers, not contracts).
2. `src/worker/executor.hpp`: `NOLINTNEXTLINE(bugprone-easily-swappable-parameters)` with
   its reason on the `for_each_index` lambda.
3. `src/verification/oracle.hpp`: `is_valid(Verdict, Contract)` and `blocks_of()` end in
   `std::get<NotChecked>`, not a dereferenced `get_if`. `tests/contracts/headers/verification_oracle.cpp`
   now calls both over a verdict of unknown alternative, and the `static-headers` preset
   (Release, static) with a step in `.github/workflows/release-static.yml` compiles the
   header units under the static artifact's flags.
4. `src/workloads/dense_linear/spec.hpp`: range stated as [-1, 1); `kName`; the worst
   failing sampled entry is named and `blocks_compared` counts the whole sample; the underflow
   term n * 2^-1074 per evaluation; the version note. The workload's own `kName` is removed
   from `dense_linear.hpp` in favour of the specification's.
5. `src/workloads/compression/spec.hpp`: the last match is twelve bytes before the end of
   the decoded data; the last token's match field is 0; a decomposition of 0 or 1 is
   accepted and anything else refused; `kName`; the version note. The workload's own `kName`
   is removed from `compression.hpp`.
6. `src/verification/isolation.hpp` states stream 10's four readings; the reference
   isolator in `tests/contracts/verification/reference_isolation_test.cpp` follows reading 1,
   and `IsolatorContract` has the case
   `AOneThreadOriginalThatComesOutCleanAnswersNoSingleThreadedToo`.
7. `docs/streams/03-oracle-compression.md`, exit criterion 3, reworded as decided.
8. **Not done.** `origin/main` (e7c225c) has no `-fprofile-update=atomic` in
   `cmake/Coverage.cmake`; PR #67 adds it. The per-target copies in `src/` and `tests/`
   `CMakeLists.txt` stay, and are to be removed after #67 lands.

## The questions, by stream

### 01 verification core

1. `kMalformed` DecodeFault: **Deferred** to stream 01 (a later change to `record.hpp`); tested as `kInvalid` meanwhile.
2. Which fault when several apply: **Accepted**, the first problem in byte order (`src/verification/record*`, `tests/contracts/verification/record_contract.hpp`).
3. Text over 4 GiB encodes and decodes as `kInvalid`: **Deferred** to stream 01, with the truncation rule in 5.
4. Offsets of a kStored Difference from a line-aligned origin: **Accepted**; the precondition is in `compare.hpp`, the runner places buffers on lines.
5. A description longer than `kMaxTextBytes`: **Deferred** to stream 01 (a truncation rule); producers bound their own input until then.
6. Golden vectors beyond `plan().units`: **Accepted** ("every index is a unit").
7. The brief's pull-request instruction: **Accepted**; the manager opens pull requests.

### 02 input integrity

1. Memory policy in `RunnerSetup`: **Deferred** to stream 02, the next runner contract change.
2. NUMA node list: **Deferred**, same change.
3. Seed as an argument of `generate()`: **Deferred**, same change.
4. `RunnerFault::kStopped`: **Deferred**, same change.
5. A repairs tally: **Deferred**, same change.
6. Tolerances and checksums owe the same protection: **Reading stands**; each provider that holds one protects it (`src/verification/integrity/`).
7. An oracle of another contract is `kConfiguration`: **Reading stands** (`slice_runner.cpp`).
8. A verdict that breaks `is_valid()` is a `VerificationNotPerformed`, blocks not counted: **Reading stands** (`slice_runner.cpp`).
9. `is_valid` and `blocks_of` in the static release build: **Taken**, item 3; the runner's visit workaround stays valid and can be retired by stream 02.

### 03 oracle, compression

1. End of what: **Taken**, item 5 ("OF THE DECODED DATA").
2. The last token's match field: **Taken**, item 5 (must be 0).
3. Exit criterion 3: **Taken**, item 7.
4. A malformed block with no scratch is NotChecked: **Reading stands** (`src/verification/oracle/compression/`).
5. Registry suite beside the oracle suite: **Taken**, item 1.
6. "Any other configured value": **Taken**, item 5 (0 or 1 accepted).

### 04 bounded verification

1. The input range: **Taken**, item 4.
2. The workload's name: **Taken**, item 4 (`kName`).
3. Which input entries make a unit NotChecked: **Reading stands**, any entry (`src/verification/oracle/dense_linear/`).
4. Which failure a BoundedMismatch names, and `blocks_compared`: **Taken**, item 4.
5. Underflow: **Taken**, item 4.
6. Suites sharing a header: **Taken**, item 1.
7. Coverage under concurrency: **Deferred** with item 8 (the shared flag in PR #67).

### 05 workload, compression

1. `for_each_index` under clang-tidy: **Taken**, item 2.
2. Two suites in the workload header: **Taken**, item 1.
3. `kVersion` against `Descriptor::version`: **Taken**, items 4 and 5 (the version note: the specification's `kVersion` versions its format and check; a workload's bits are versioned by its descriptor).
4. `-fprofile-update=atomic` in `Coverage.cmake`: **Deferred**, item 8.
5. A block that decodes to the wrong size is an ExactMismatch with no difference: **Reading stands** (`src/verification/oracle/compression/`, `src/workloads/compression/`).
6. The fixture directory: **Reading stands** (`tests/fixtures/lz4-workload/`).

### 06 workload, dense linear

1. `for_each_index` under clang-tidy: **Taken**, item 2.
2. `select_path()` intersected with a workload's paths: **Deferred** to stream 11 (integration).
3. Panel sums from zero then added to c: **Reading stands**; the bound holds for any order, the oracle is unaffected (`src/workloads/dense_linear/`).
4. A path the machine cannot run (`runs_here()` on a subject): **Deferred**, to a later suite change; subjects refuse in `create()` meanwhile.
5. `Plan::logical_bytes` of a blocked algorithm: **Accepted**, the compulsory traffic.
6. Coverage instrumentation of concurrent code: **Deferred**, item 8.

### 07 numerics, platform

1. `FpState{}` is not a reset: **Accepted** (`src/platform/fpenv/`).
2. `IsaProbe` architecture with `LOADFORGE_SIMD=scalar`: **Accepted**; `supported()` is the machine, not the build.
3. Who re-asserts the floating-point environment after a library load: **Deferred** to stream 11 (integration).

### 08 executor

1. One thread per CPU of the worker's set: **Accepted** (`one_thread_per_cpu`, `src/worker/`).
2. Calls on the executor's own threads: **Accepted**; a consumer assumes neither.
3. The floating-point check read as "inherited the creator's state": **Reading stands**; one call in `prepare()` if the contract means `check()` (stream 11).
4. One header, two suites: **Taken**, item 1.
5. A count near `SIZE_MAX`: **Accepted**, recorded not guarded.

### 09 telemetry sources A

1. Two suites in one header: **Taken**, item 1.
2. A metric that failed at the probe is not retried at `sample()`: **Reading stands**; a re-probe is the way back.
3. The series of a metric whose whole source is unavailable: **Reading stands**, one status for the expected metric.
4. `Unavailable.path` for a class: **Reading stands**, the directory searched.
5. Series index for a device without a natural number: **Reading stands**; a label field is a later `sample.hpp` change if reports need it.
6. A 32-bit counter's modulus: **Accepted**, recorded; 64-bit targets only.

### 10 isolation

1. A one-thread original's single-thread answer: **Taken**, item 6.
2. A placement of 0 threads: **Taken**, item 6 (passed through; the re-runner may refuse).
3. Which other core: **Taken**, item 6 (the core changes, node and cache sharing held constant where the topology allows).
4. An outcome outside `RerunOutcome`: **Taken**, item 6 (answered `kUnknown`).

### 14 telemetry pipeline

1. Two suites in one header: **Taken**, item 1.
2. `probe()` after a refused probe: **Reading stands**, the first outcome is final.
3. The sampler does not validate a source's `probe()`: **Reading stands**; the source's provider suite owns it.
4. A refused point ends the round: **Reading stands** (`src/telemetry/sampler/`).

### 15 statistics

1. The header registers one suite, included without instantiating it: **Reading stands**; not a two-suite case, the allowance in `tests/property/statistics/` stays.
2. "A pair exists" means `span_ns > 0`: **Reading stands**.
3. The form of the standard deviation and slope: **Reading stands**; the suite's values are exact either way.

### 17 workload, sort

1. Writes before `run()` are visible to the tasks: **Reading stands**; to be stated in `executor.hpp` at the next executor change (the pool already publishes them).
2. `Plan::scratch_bytes` per slot against a unit-wide buffer: **Reading stands**; a per-unit field is a `workload.hpp` change for a workload that needs it.
3. Which of several refusals is reported: **Reading stands**.
4. "The oracle may need kKeys bits": **Accepted**, the oracle's.

### 18 workload, Monte Carlo

1. `Plan::decomposition` is never 0: **Reading stands**.
2. A vector-path subject on a machine without it: **Deferred**, with 06.4 (`runs_here()`).
3. INSTANTIATE macros take a type-list alias, in the contract's namespace: **Reading stands**; a line for the contracts' README at the next documentation pass.

### 19 oracles, wave 2

1. A configured decomposition that differs from the plan's: **Reading stands**, not refused.
2. Sort step 2, window past the region: **Reading stands**.
3. Sort NotChecked order, sizes before scratch: **Reading stands**.

### 28 explore

1. Order of `signature.window` across trials: **Reading stands**, monotonic; to be stated in `trial.hpp` at the next explore contract change.
2. `kCorrectedErrors` is a score: **Reading stands**.
3. `Candidate.duty` held constant: **Accepted**, enforced by explore.

## Deferred, by owner

- Stream 02, the next runner contract change: memory policy in `RunnerSetup`, the node list, the seed as an argument of `generate()`, `RunnerFault::kStopped`, a repairs tally.
- Stream 01: `kMalformed` DecodeFault; a truncation rule for over-long text.
- Stream 11, integration: `select_path` intersected with a workload's paths (06.2); the floating-point re-assert policy (07.3).
- `runs_here()` on a subject (06.4, 18.2): a later suite change.
- Shared coverage flag (04.7, 05.4, 06.6): after PR #67.

The project still needs an outside reviewer; the contracts are the cheapest place for one to start.
