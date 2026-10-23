# Review E: root Markdown, NOTICE, ADRs (read-only, tree at main 720fdbb)

Scope found: the only root Markdown besides README.md and CONTRIBUTING.md is CODE_OF_CONDUCT.md (no SECURITY.md, no CHANGELOG.md; nothing in the tree links to either). NOTICE (no extension) was reviewed too. ADRs: 0001-0004 and six under docs/adr/proposed/. docs/streams/README.md does not exist, and nothing links to it.

## Findings

### F1. Six proposed ADRs were never numbered, though all were acted on and the roadmap promises numbering
- docs/ROADMAP.md:87-90 ("at the end of every wave: ... proposed ADRs numbered") and docs/adr/0004-contracts-fragments-and-streams.md:140 ("numbered by the manager when it lands").
- True: waves 1-4 and the M2, M3, M5, M6 integrations are merged (ROADMAP.md:93-99), yet docs/adr/ holds 0001-0004 and docs/adr/proposed/ still holds six files, none numbered:
  - 06-workload-dense-linear-vector-kernels.md (Date 2026-10-20 11:20 UTC)
  - 02-input-integrity-working-set-memory.md (11:25 UTC)
  - 27-oracles-wave3-scratch-and-sampling.md (17:40 UTC)
  - 21-m3-integration-sampling-points-and-phase-windows.md (21:40 UTC)
  - 21-m3-integration-contract-disagreements-as-expected-failures.md (21:41 UTC)
  - 29-q10-worker-granularity-under-a-mixed-workload.md (2026-10-21 22:46 UTC)
- Each decision is followed by the code (checked: F2-F5 below, and for the others: per-function `target("avx2,fma")` at src/primitives/simd/x86/gemm_avx2.cpp:45; `zeroupper` in the x86 kernels; `LOADFORGE_SIMD_X86_KERNELS`/`_ARM_KERNELS` in src/primitives/simd/{x86,arm}/CMakeLists.txt; per-directory `.clang-tidy` with `-portability-simd-intrinsics`; arch-scoped coverage expectation in tools/check-coverage-complete.sh:36-49 and `ubuntu-24.04-arm` in .github/workflows/coverage.yml:29; `ScratchFactory` at src/verification/oracle/graph/graph_oracle.hpp:50,70; `Switch`/`Rebind` in src/worker/control.hpp:84,94; tests/supervision/journal_kill_test.cpp, tests/soak/workloads/mixed and tests/fault_injection/workloads/mixed exist).
- Fix: `git mv` them into docs/adr/ as 0005 to 0010 in date order (06 -> 0005, 02 -> 0006, 27 -> 0007, 21-sampling -> 0008, 21-disagreements -> 0009, 29 -> 0010); set `Status: accepted` (with the amendment notes from F2-F4); retitle "ADR 0005 -" etc.; replace the stream-number prefix; update the path citations: docs/JOURNAL.md:2600, :2852, :2922; docs/journal/02-input-integrity.md:122, 06-workload-dense-linear.md:244, 21-integration-m3.md:126,188, 27-oracles-wave3.md:113, 29-integration-m5.md:177; remove docs/adr/proposed/. Update ADR 0004:140 only by a note (ADRs are immutable per ADR 0001:32-34).

### F2. docs/adr/proposed/02-input-integrity-working-set-memory.md:24-32, 36-39, 46-48: "no fallback" and "superseded when" are overtaken
- Says: the working set is always locked; "There is no fallback: a run that cannot lock its working set does not start"; superseded when the setup gains a controller-chosen memory policy.
- True: src/verification/runner.hpp:135-153 defines `LockPolicy { kRequire, kTry, kNone }` ("Contract change of the M2 corrections, 2: until then the runner always locked"); `RunnerSetup::lock` at runner.hpp:310 (default kRequire); the CLI selects among the three (src/main/cli.cpp:268, src/main/explore_command.cpp:355); the result records the choice (`WorkloadResult::lock_policy`, `memory_lock`, src/controller/results/result.hpp:121-124). Landed in commit 92b9a98 (2026-10-21 18:02 UTC). So: lock is now optional by policy, an unprivileged user under an 8 MiB allowance can run with kTry/kNone, and the "superseded when" condition is met for locking. What still holds: ordinary pages, populate by `MADV_POPULATE_WRITE` (src/platform/memory.cpp:234-238), bound to NUMA node, no huge-page policy, and kRequire is the default and refuses on failure.
- Fix: when numbering (F1), mark "accepted; amended 2026-10-21 18:02 UTC by the M2 contract corrections: locking is now a controller-chosen `LockPolicy` (default kRequire, no silent fallback; kTry falls back and says so)"; rewrite the 6.6 MB consequence as applying to kRequire only.

### F3. docs/adr/proposed/21-m3-integration-sampling-points-and-phase-windows.md:6-10 and :2-5
- Says: "`src/main`'s own `phase_windows()` is to be replaced by them (recorded follow-up)".
- True: replaced. src/main/run_result.cpp:50 builds each window with `statistics::phase_window(marks, at, end_ns)` and :191 uses `statistics::run_window(...)`; src/main/run_result.hpp:38 documents it. `phase_windows()` remains only as the labelling wrapper. (The follow-up is at docs/journal/m3-contract-corrections-2026-10-21-1145-UTC.md:262-264.)
- Also line 6: "Outcome, 2026-10-21" has no time (CONTRIBUTING.md:280-284). The commit adding it is 2026-10-21 14:02 UTC. Line 3 "Status: proposed ... the manager numbers it" is stale (F1).
- Fix: rewrite the outcome as "decision 3 is stated in statistics/signature.hpp and used by src/main/run_result.cpp; the follow-up is done (<time> UTC)", with a time.

### F4. docs/adr/proposed/21-m3-integration-contract-disagreements-as-expected-failures.md:28-31 and :6
- Says the tests live in the executable `loadforge_integration_m3_disagreements`, registered `WILL_FAIL`. The outcome box already says the executable was removed and nothing is `WILL_FAIL` now, which is accurate (tests/integration/m3/corrections_test.cpp:3-12 and tests/integration/m3/CMakeLists.txt:3-7 confirm; no `WILL_FAIL` in tests/integration). Remaining defects: line 6 "Outcome, 2026-10-21 (branch `fix/m3-contract-disagreements`)" carries no time; Status still "proposed" (F1). The mechanism is not retired, only unused, so the decision stands.
- Fix: add the time to the outcome (the corrections journal is named 2026-10-21-1145-UTC; the M2 round, 1802 UTC); number it (F1); state "mechanism available, currently empty".

### F5. docs/adr/proposed/29-q10-worker-granularity-under-a-mixed-workload.md:59-60
- Says: "The run's result names one workload per worker (`WorkerWork`, frozen in `result.hpp`)".
- True: `WorkerWork` is defined at src/controller/report.hpp:53 (used in src/controller/plan.hpp:86), not in result.hpp. result.hpp's `WorkerResult` (src/controller/results/result.hpp:128-133) has slot, fate, cpu, termination and no workload; the result names workloads in `WorkloadResult`. docs/JOURNAL.md:3872 repeats the `WorkerWork` wording, so check that too.
- Fix: say "the plan's `WorkerWork` (src/controller/report.hpp) records the workload a worker started as, and the result's `WorkerResult` does not carry the rotation"; correct "frozen in result.hpp". Status "proposed" -> accepted (M5 merged, #96, docs/streams/29-integration-m5.md:3) (F1).

### F6. docs/adr/0004-contracts-fragments-and-streams.md:119: `tools/gen-golden/` does not exist
- Says: golden-vector regeneration (`tools/gen-golden/`) is assigned to the integration stream.
- True: `ls /home/user/LoadForge/tools` has no gen-golden. Regeneration is done by per-fixture scripts: tests/fixtures/determinism/workloads/sort/make_golden.py and tests/fixtures/workloads/monte_carlo/make_golden.py, with golden.txt files beside them (the particles one has no script in that directory). The same dead path is cited at src/verification/golden/golden.hpp:28, docs/verification.md:97 and tests/fixtures/lz4-workload/text.bin:97.
- Fix: do not edit the immutable ADR body; add an "Amended" line giving the actual regeneration route, or build the tool and keep the path. Decide which, then fix the other citations.

### F7. docs/adr/0004...:139 (State line mechanism) versus the briefs
- Says each stream records its state on the `State` line of its brief, and the manager updates the roadmap per wave.
- True: ROADMAP.md:93 summarises waves 1-4 and the integrations as merged, but most briefs' State lines are still pre-merge, e.g. docs/streams/02-input-integrity.md:3 "complete on `feature/input-integrity`, awaiting review (2026-10-20 11:25 UTC)", 08-executor, 10-isolation, 12, 13, 17-27, 30-32 ("pull request not yet opened"). Only 29, 33, 34 say merged. This is outside the files I was asked to review; reported because ADR 0004 makes the State lines the status record.
- Fix: one pass setting each State line to "merged, #N, <date> <time> UTC" (the roadmap's own merged list gives the PRs).

### F8. Dates without a UTC time (CONTRIBUTING.md:278-284)
- docs/adr/0002-continuous-development.md:8 "between 2026-08-24 and 2026-10-19" (the two ends are first-commit and review dates); docs/adr/0003-components-share-contracts-not-tests.md:23 "review of 2026-10-19" (the file is reviews/comprehensive-project-review-2026-10-19-0449-UTC.md, so "2026-10-19 04:49 UTC"). Lower severity: they are period/reference dates inside accepted ADRs. If the rule is applied to prose, append times (ADR 0002's range ends: first commit 2026-08-24 20:35 UTC, which is ADR 0001's own timestamp); otherwise state in CONTRIBUTING that prose date ranges are exempt.
- The two "Outcome, 2026-10-21" boxes: see F3, F4.

## Checked fully, accurate (no finding)
- CODE_OF_CONDUCT.md: standard Contributor Covenant 2.1 text; links are absolute external; the enforcement contact is the owner's address (matches CONTRIBUTING.md:342 reference and the configured identity). No stale claims.
- NOTICE: GoogleTest v1.15.2 and SHA variable `LOADFORGE_GTEST_SHA` (cmake/Dependencies.cmake:16-18); toml++ v3.4.0, commit 30172438..., third_party/tomlplusplus holds only LICENSE and toml.hpp; third_party/MANIFEST.toml exists with exactly those two components; Mull 0.34.2 / LLVM 18 pinned by SHA-256, weekly cron plus `workflow_dispatch` (.github/workflows/mutation.yml:8-9,30-31); tools/check-dependencies.py run in licensing.yml:140; `LOADFORGE_BUILD_TESTS` option (CMakeLists.txt:24). Accurate.
- docs/adr/0001: accurate (docs/PLAN.md, determinism.md, verification.md, platforms.md exist); timestamp matches its first commit (2026-08-24 20:35 UTC).
- docs/adr/0002: historical figures are immutable by design; the roadmap (docs/ROADMAP.md) exists and is used as stated. Only F8.
- docs/adr/0003: tests/support/fake_syscalls.hpp exists; contracts-first step landed as ROADMAP item 12a. Only F8.
- docs/adr/0004: all 21 headers exist with "CONTRACT (ADR 0004)" marker, every named suite (SelectPathContract ... TrialRunnerContract) is present under tests/contracts/, headers compile units in tests/contracts/headers/, loadforge_add_fragments in cmake/Components.cmake:40 and CMakeLists.txt:232, tests/obligations.d fragments, docs/journal fragments, 34 stream briefs, M1 stop byte retired on both sides (src/worker/control.hpp:14-21, kControlFrameType in src/worker/control_channel.cpp and src/controller/orchestration.cpp). Only F6, F7, and the unnumbered-ADR mechanism (F1).
- Proposed ADR 06 and 27: every claim matched (see F1 evidence); content accurate. Only the numbering/status (F1).
- No link in any file in scope is a relative Markdown link to a missing target (the ADRs use code paths, all verified above except those named in F5 and F6).

Reminder: the project still needs an outside reviewer.
