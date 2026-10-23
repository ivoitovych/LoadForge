# Roadmap — the ordered queue from here to 1.0

This is the working queue. Read it at the start of every session, take the first
item that is not done, and update this file in the same PR that lands an item
(ADR 0002). Scope and exit criteria for each milestone are in
`docs/IMPLEMENTATION.md` §4; this file only orders the work and records where it
stands.

**From M2a on, the work runs as parallel streams in waves (ADR 0004).** Each stream
has a self-contained brief in `docs/streams/`, owns its own directories, and records
its state on the `State` line of its brief rather than here, so that ten streams
landing in one wave never edit this file. The manager updates this file when a wave
starts and when it ends.

**Rules of the queue.** Each item lands as one PR, verified locally against the
full gate set before the push, merged when CI is green. A decision met on the way
is researched, taken, and recorded (ADR, journal, or PR description), and the
work continues.

## Standing items

- **Outside review.** The first independent review arrived on 2026-10-20
  (`reviews/review-response-2026-10-20-0352-UTC.md`): three findings, all
  accepted, items 10b to 10d. It covered supervision only; the rest of the tree
  and every stream PR still have had no outside reviewer.
- **Branch protection on `main`.** Owner-only: a repository setting.

## Now — finish M1

| # | Item | Source | State |
|---|------|--------|-------|
| 1 | Land the controller: `controller/`, `reporters/console`, `loadforge stress`, `tests/supervision/` | IMPLEMENTATION §10 | done, #23 |
| 2 | Comprehensive review, reaction plan, ADR 0002, this roadmap | `reviews/` | done, #21 |
| 3 | Seqlock writer: release fence after the odd store; correct the P5 argument | review R-01 | done, #24 |
| 4 | Controller ignores SIGPIPE through the seam; test and catalogue row | review R-02 | done, #28 |
| 5 | Kill the surviving drain-bound mutant with an exact assertion | review R-06 | done, #28 |
| 6 | Hygiene batch: linkage, torn-slot wording, drain bound by wall time, slot layout asserts, fixture references | review R-03..R-05, R-24, R-29 | done, #27 and #28 |
| 7 | Documentation reconciliation in one commit | review R-08..R-13, R-19, R-21, R-25, R-27, R-33 | done, #26 |
| 8 | Fake-worker executable with the misbehaviour catalogue, including a worker that ignores its control pipe (the only real test that the parent-death signal ends a worker); gate: no test-hook symbol in the shipped binary; gate: lock obligations in the ledger | IMPLEMENTATION §10 | done, #29 |
| 9 | Memory-budget reader (`MemAvailable`, cgroup v1 and v2) and OOM classified from the kernel's record | IMPLEMENTATION §10, journal §1.18 | done, #30 |
| 10 | M1 exit: every criterion in IMPLEMENTATION §4 met and marked | IMPLEMENTATION §4 | done, #33, pending the mutation verdict: M1 is complete when the first sweep over `src/` reports zero unexplained survivors |
| 10b | EXT-01: a successful `kill` is not proof the controller caused a SIGKILL death; split attempted from established, never signal a worker that has already exited, report ambiguous provenance, always consult the OOM count; the fake models `kill` on a zombie as the kernel does | external review, reaction plan 2026-10-20 12:30 UTC | done, #79 |
| 10c | EXT-02: startup as its own supervised phase: the controller beats between forks, each worker's allowance anchored at its own launch, the duration's start decided and documented | external review | done, #80 |
| 10d | EXT-03: a crash signal names a fault, not its cause; header, operator and verification documents, and abort and software-SIGBUS catalogue cases | external review | done, #81 |

## Next — prerequisite for M2a

| # | Item | Source | State |
|---|------|--------|-------|
| 11 | Pin a Mull release by verified SHA-256 and run the mutation gate end to end in CI; schedule it weekly on `main` | IMPLEMENTATION §4 M0, review R-31 | done, #32; the first full sweep over `src/` reports on its first scheduled run |
| 11a | Shard the mutation sweep: one job per module under `src/`, each gated on its own (`--scope`), every mutant against the tests that create no process (`RealProcess*` left out and checked), a per-mutant timeout, and a verdict job. The first full run, as one job, was cancelled at six hours | journal §1.26, §2.28 | done, #46; the first sharded run found 56 survivors in six shards (journal §2.28) |
| 11b | Kill the 56 survivors of the first sharded run, or allowlist each equivalent one with its reason. Per module: `platform`, including moving the pid decisions in `fork_process` and `reap_any_nonblocking` above the seam; `controller` and `worker`; then `topology`, `resources` and `config`. M1 is formally complete when the weekly sweep passes | journal §2.28 | |
| 12 | clang-tidy over tests and tools, or the scope documented; trust-chain linter versions pinned | review R-07, R-32 | tests: done, #36 (tests/.clang-tidy); tools and linter pins open |
| 12a | Contracts first: the workload, verifier and telemetry-source contracts, each with the provider's own tests and the cross-execution harness that runs them against consumers' mocks. M2a to M4 then proceed as parallel streams, one component each | ADR 0003, ADR 0004 | done, this PR: twenty-one contract headers, their suites and references, the fragment mechanism, the stream plan below |

## Then — the milestones, as streams in waves

The milestones of `docs/IMPLEMENTATION.md` §4 are delivered by the streams below
(ADR 0004). A stream owns disjoint directories, builds against the frozen contracts
only, and is briefed in full in `docs/streams/<NN>-<slug>.md`. A wave starts when the
previous one has merged and its prerequisites below are in; within a wave, streams
run in parallel and merge in any order.

Tier A: a bounded component built against a written contract. Tier B: concurrency,
supervision, verification oracles, or a pattern every later stream copies.

| Milestone | Delivered by |
|---|---|
| M2a | 01, 02, 03, 05, 08, 09 (capability), 10; exit by 11 |
| M2b | 04, 06, 07; exit by 11 |
| M3 | 09, 12, 13, 14, 15; exit by 21 |
| M4 | 17, 18, 19, 20, 22-27, 30-32 |
| M5 | 16 (journal); exit by 29 |
| M6 | 28; exit by 33 |
| M7 | 34 |

**Prerequisites the manager lands before a wave starts**, each as its own contract
change (ADR 0004, "Changing a contract"):

- before wave 2: the specification headers for sort and Monte Carlo
  (`src/workloads/<name>/spec.hpp`) -- landed in this PR (journal §2.29) -- and any
  contract corrections wave 1's streams recorded under "Contract questions";
- before M2a can exit (stream 11): M1 complete, which waits only on the first mutation
  sweep (item 10); the Mull pin, item 11, is in (#32);
- before wave 3: the specification headers for FFT, stencil, sparse, graph and
  particles;
- before wave 4: the specification headers for ray tracing and adaptive mesh;
- at the end of every wave: the streams' journal fragments folded into
  `docs/JOURNAL.md`, proposed ADRs numbered, `docs/IMPLEMENTATION.md` §3 and §10
  reconciled with what landed, and this file's state brought up to date.

**State on 2026-10-22 22:11 UTC.** Waves 1 to 4 and the integrations 21 (M3, #87), 11 (M2, #90),
29 (M5, #96) and 33 (M6, #99) are merged; the contract corrections #92 and #95 are in; the
streams' journal fragments are folded into `docs/JOURNAL.md` up to M6 (#100). Stream 34's
preparation lands with this line: the planned documents, packaging, the release workflow and
the hardware-validation checklist (`docs/hardware-validation.md`). Its review of the tree
found that the temperature ceiling is still reported NOT ENFORCED and that an interrupted
run writes no report; both are 1.0 work (34a, 34b below), taken before the hardware half,
which waits for the owner's machines.

#### Wave 1

| # | Stream | Milestone | Tier | Depends on |
|---|---|---|---|---|
| 01 | [Verification core: records on the wire, exact comparison, checksums, golden vectors](streams/01-verification-core.md) | M2a | B | contracts |
| 02 | [Input integrity: the slice runner, its bracket, and protected control state](streams/02-input-integrity.md) | M2a | B | contracts |
| 03 | [The compression oracle: an independent LZ4 block decoder](streams/03-oracle-compression.md) | M2a | B | contracts |
| 04 | [Bounded verification: derived tolerances, invariants, and the dense-linear oracle](streams/04-bounded-verification.md) | M2b | B | contracts |
| 05 | [Workload: compression](streams/05-workload-compression.md) | M2a | B | contracts |
| 06 | [Workload: dense linear algebra, and the first vector kernels](streams/06-workload-dense-linear.md) | M2b | B | contracts |
| 07 | [Numerics platform: the floating-point environment, the ISA probe, path selection](streams/07-numerics-platform.md) | M2b | A | contracts |
| 08 | [The executor: a worker's thread pool](streams/08-executor.md) | M2a | B | contracts |
| 09 | [Telemetry sources: thermal, fan, frequency, memory; the capability probe](streams/09-telemetry-sources-a.md) | M2a (capability), M3 (sources) | A | contracts |
| 10 | [Fault isolation: the re-run ladder](streams/10-isolation.md) | M2a | B | contracts |

#### Wave 2

| # | Stream | Milestone | Tier | Depends on |
|---|---|---|---|---|
| 11 | [Integration: compression and dense linear algebra end to end](streams/11-integration-m2.md) | M2a, M2b (exits) | B | streams 01-10 merged; M1 complete (roadmap item 10: its first mutation sweep clean) before M2a exit criterion 6 |
| 12 | [Results, the fingerprint, and the JSON and CSV reporters](streams/12-results-and-reporters.md) | M3 | A | contracts |
| 13 | [Telemetry sources: power, performance counters, EDAC](streams/13-telemetry-sources-b.md) | M3 | A | contracts |
| 14 | [The timeline and the sampler](streams/14-telemetry-pipeline.md) | M3 | A | contracts |
| 15 | [Statistics: the Load Signature](streams/15-statistics.md) | M3 | A | contracts |
| 16 | [The crash journal](streams/16-journal.md) | M5 (component) | B | contracts |
| 17 | [Workload: sort](streams/17-workload-sort.md) | M4 | A | the sort specification header landed before the wave |
| 18 | [Workload: Monte Carlo](streams/18-workload-monte-carlo.md) | M4 | A | the Monte Carlo specification header landed before the wave |
| 19 | [Oracles: sort and Monte Carlo](streams/19-oracles-wave2.md) | M4 | B | the two specification headers landed before the wave |
| 20 | [Primitives: the calibration engine](streams/20-primitives.md) | M4 | B | contracts |

#### Wave 3

| # | Stream | Milestone | Tier | Depends on |
|---|---|---|---|---|
| 21 | [Integration: full telemetry and the Load Signature](streams/21-integration-m3.md) | M3 (exit) | B | streams 11-16 merged |
| 22 | [Workload: FFT](streams/22-workload-fft.md) | M4 | A | the FFT specification header landed before the wave |
| 23 | [Workload: stencil](streams/23-workload-stencil.md) | M4 | A | the stencil specification header landed before the wave |
| 24 | [Workload: sparse (conjugate gradient)](streams/24-workload-sparse.md) | M4 | A | the sparse (conjugate gradient) specification header landed before the wave |
| 25 | [Workload: graph (BFS, PageRank)](streams/25-workload-graph.md) | M4 | A | the graph (BFS, PageRank) specification header landed before the wave |
| 26 | [Workload: particles](streams/26-workload-particles.md) | M4 | A | the particles specification header landed before the wave |
| 27 | [Oracles: FFT, stencil, sparse, graph, particles](streams/27-oracles-wave3.md) | M4 | B | the five specification headers landed before the wave |
| 28 | [Explore: search, objectives, drift, confidence](streams/28-explore.md) | M6 (core) | B | contracts |

#### Wave 4

| # | Stream | Milestone | Tier | Depends on |
|---|---|---|---|---|
| 29 | [Integration: the mixed workload, cycling, worker control, the journal](streams/29-integration-m5.md) | M5 | B | streams 16, 21 merged; the wave-3 workloads merged |
| 30 | [Workload: ray tracing](streams/30-workload-raytrace.md) | M4 | A | the ray tracing specification header landed before the wave |
| 31 | [Workload: adaptive mesh](streams/31-workload-adaptive-mesh.md) | M4 | A | the adaptive mesh specification header landed before the wave |
| 32 | [Oracles: ray tracing and adaptive mesh](streams/32-oracles-wave4.md) | M4 | B | the two specification headers landed before the wave |

#### Wave 5

| # | Stream | Milestone | Tier | Depends on |
|---|---|---|---|---|
| 33 | [Integration: explore on the controller](streams/33-explore-integration.md) | M6 (exit) | B | streams 28, 29 merged |

#### Wave 6

| # | Stream | Milestone | Tier | Depends on |
|---|---|---|---|---|
| 34 | [Hardening and 1.0](streams/34-release-1-0.md) | M7 | B | every earlier stream merged |

Found by stream 34's preparation (`docs/journal/34-release-1-0.md`); each lands as its own PR:

| # | Item | Source | State |
|---|------|--------|-------|
| 34a | Enforce `limits.max_temperature_c` from the sampled telemetry: a reading at or above the ceiling stops every worker, the run ends with its own exit code and says which sensor and what it read; the banner stops saying NOT ENFORCED where a temperature can be read, and says why where it cannot | PLAN (safety limits, emergency stop); `src/controller/safety.cpp` | done, this change |
| 34b | SIGINT and SIGTERM stop the run gracefully: workers stopped, the report and the journal written, exit code stated | stream 34 finding 2 | done, this change |
| 34c | Documents that disagree with the tree: the hardware-failure issue template's `loadforge selftest`, `benchmark`/`selftest` in PLAN and verification.md, ShellCheck over `packaging/`, IMPLEMENTATION §10's M1 state | stream 34 finding 3 | done, this change |
| 34d | The hardware half of M7: the checklist run on the owner's machines, both architectures, live media | stream 34 brief | waits for the owner |
| 34e | loadforge selftest: the golden digest and one short verified unit per workload with its oracle, a triage command | PLAN F2; reviews/reaction-plan-2026-10-23-0030-UTC.md | after 34b |
