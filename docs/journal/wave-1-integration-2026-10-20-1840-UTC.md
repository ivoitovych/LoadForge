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
3. `src/verification/oracle.hpp`: `is_valid(Verdict, Contract)` and `blocks_of()` are a
   visit over per-alternative overloads (`detail::keeps_rules`), not a dereferenced `get_if`
   (`std::get` was tried first and trips `bugprone-exception-escape`; the two NOLINTs say why a
   visit cannot throw here). `tests/contracts/headers/verification_oracle.cpp`
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
8. **Deferred to one sweep after wave 3.** PR #67 (merged 2026-10-20 20:18 UTC) sets
   `-fprofile-update=atomic` for every target in `loadforge_enable_coverage()`, so the
   per-target copies in `src/` and `tests/` `CMakeLists.txt` are redundant and harmless.
   They sit in some twenty directories, several owned by streams whose pull requests are
   open (#69 to #74, #77), which add copies of their own. Removing them now would conflict
   with each of those branches for no change in behaviour; one sweep after wave 3 merges
   removes all of them at once (2026-10-20 22:40 UTC).

## Deferred, by owner

- Stream 02, the next runner contract change: memory policy in `RunnerSetup`, the node list, the seed as an argument of `generate()`, `RunnerFault::kStopped`, a repairs tally.
- Stream 01: `kMalformed` DecodeFault; a truncation rule for over-long text.
- Stream 11, integration: `select_path` intersected with a workload's paths (06.2); the floating-point re-assert policy (07.3).
- `runs_here()` on a subject (06.4, 18.2): a later suite change.
- Shared coverage flag (04.7, 05.4, 06.6): after PR #67.

The project still needs an outside reviewer; the contracts are the cheapest place for one to start.
