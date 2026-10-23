# Review F: design documents against the tree

Scope: docs/PLAN.md, docs/IMPLEMENTATION.md, docs/verification.md, docs/determinism.md, and the other
docs/*.md not assigned elsewhere (platforms.md, result-schema.md, DESIGN-DRAFT.md). Tree: `main` at 720fdbb.
Read-only. Every finding below was checked against the file named; "doc wrong" means the document should change,
"code wrong" means the tree departs from a normative statement and the document is right.

Severity: H = a reader would act on a false statement; M = stale or false but unlikely to mislead an
implementer; L = detail.

---

## A. Stale status (documents that say what is not built, or not done, when it is)

A1 (H) PLAN.md:2047-2087 (§11 "Status and next step"). Says "M0 (foundation) has landed", "What does not exist yet is any
workload: those begin at M2a", "Next is M1", "eight CI workflows", and (2066-2076) the mutation gate "is specified, not
demonstrated ... `tests/tools/mutation_integration.sh` has never been executed ... Mull is not pinned". Tree: all eleven
workloads, mixed and cycling, explore, journal, reporters exist (`src/workloads/*`, `src/explore`, `src/controller/journal`);
ROADMAP.md:60-90 says waves 1-5 and integrations M2, M3, M5, M6 are merged; Mull 0.34.2 is pinned by SHA-256 in
`.github/workflows/mutation.yml:30-31` and scheduled weekly (`:7-8`); IMPLEMENTATION.md:329-331 and ROADMAP item 11 record the
pin and the demonstrated gate as done. Ten workflows exist, not eight (`.github/workflows/`: also `release.yml`, `soak.yml`).
Document wrong. PLAN also contradicts itself: §6 tree (1510-1511) already describes the pinned, weekly, per-module mutation job.

A2 (H) PLAN.md:1056-1061 (F21 "The honest limitation"). "the integration test has not been run. Mull is not pinned ... the mutation
gate is specified correctly, not demonstrated". Same evidence as A1. Document wrong (should become history).

A3 (M) PLAN.md:1946-1954 (§8). M0 row says "eight CI workflows" and "two items carried: the mutation gate is specified but
undemonstrated". M1..M6 rows carry no completion mark although ROADMAP shows them delivered (M1 pending only the first clean
weekly sweep). Document wrong.

A4 (H) IMPLEMENTATION.md:758-762 (§10 Status). "The contracts for M2a to M7 are written and the work after M1 is planned as streams
in waves." Tree and ROADMAP.md:75-82: waves 1-5 built and merged, streams 11, 21, 29, 33 integrated, stream 34's preparation
in. ROADMAP.md:171 (item 34c) already lists "IMPLEMENTATION §10's M1 state" as a known disagreement. Document wrong.

A5 (M) IMPLEMENTATION.md:797-800 (§10). "five survivors are expected in `src/platform/syscalls.cpp`" and "Mull ... has not yet swept
`src/`". JOURNAL §2.28 (first sharded run): 56 survivors in six of nine shards (platform 23, controller 10, topology 10, worker 8,
resources 4, config 1); all later killed or removed, none allowlisted (JOURNAL §2.28 follow-ups; ROADMAP item 11b State cell is
empty). Document wrong on the count and the expectation; the remaining fact (no clean weekly sweep recorded yet, scheduled
2026-10-26 03:17 UTC, matching `mutation.yml` cron `17 3 * * 1`) is consistent.

A6 (H) IMPLEMENTATION.md:246-288 (§3 module ledger). Rows for `controller/orchestration`, `safety`, `supervision`, `reporters/console`
(264-266, 281) are not marked shipped; every row from `controller/results` (268) to `explore/` (288) is unmarked though all
those directories exist. The ledger header (3) says "revision 3 -- reconciled against the tree"; it was reconciled for M1 only.
ROADMAP.md:64-66 says §3 and §10 are reconciled at the end of each wave; that was not done. Document wrong.

A7 (H) IMPLEMENTATION.md:257-260. Rows `platform/sysfs`, `platform/procfs`, `platform/perf_events`, `platform/edac` (M3). No such
directories; the telemetry sources are `src/telemetry/sources/{thermal,fan,frequency,memory,power,counters,edac}` and
`src/telemetry/sampler`. Line 238 says "a planned row that does not match a shipped directory is a plan, not a discrepancy", but M3
is delivered, so the rows are now wrong, and the module list also lacks `worker/control`, `worker/executor`, `platform/isa` (the
last two are mentioned in prose at 295-297). Same drift in PLAN.md:1546 (`platform/{sysfs,procfs,perf_events,edac}`), 1545-1551
(`platform/fs/`, `affinity/`, `memory/`, `process/`, `clock/` as directories; they are flat files), 1554-1558 and 1561 (controller
and worker sub-directories `orchestration/ safety/ supervision/ scheduler/` do not exist; they are flat `.cpp` files), 1591
(`reporters/console/` is `console.cpp`). Document wrong.

A8 (H) IMPLEMENTATION.md:231 and 238-239 and 741. "adding a module without adding a row here is itself a gate failure", "the gate
catches it", "§3 and §6 are tables a gate can check". `tools/check-test-obligations.py` reads `tests/obligations.toml` and
`tests/obligations.d/` only; no tool or workflow reads IMPLEMENTATION.md's tables (grep of `tools/`, `.github/workflows/`). The
table that drifted in A6/A7 has no gate. Document wrong (the mitigation named in §8 risk row 6 does not exist for §3 or §6).

A9 (M) IMPLEMENTATION.md:739. "`tools/capture-sysfs.sh` lands with M3 so users can contribute fixtures". M3 exit is merged
(ROADMAP.md:79); the script is absent (`tools/` listing). Still referenced as future in `.github/ISSUE_TEMPLATE/unsupported-platform.yml:16`
and CONTRIBUTING.md:275. Code/plan not met; document should say it slipped or drop the promise. PLAN.md:1624 lists it in the tree without a
planned mark.

A10 (M) IMPLEMENTATION.md:568-569 (M3 exit criterion 7) "Mutation gating extended ... to `verification/`, `config/`, `safety/` and
`controller/supervision/`". Tree: every module under `src/` is a shard (`tools/mutation-shard.py`, IMPLEMENTATION.md:672-679). Same stale
scope in PLAN.md:1797-1799 ("run nightly ... gating on verification/, config/, safety/, controller/supervision/"; tree is weekly, all
modules). Document wrong.

A11 (M) IMPLEMENTATION.md:263. "`tests/fault_injection` (T5) does not exist until M2a creates it". It exists
(`tests/fault_injection/{verification,worker,workloads}`). Stale statement of fact.

A12 (L) PLAN.md:1501 "all eight present"; tree has ten workflows, `release.yml` and `soak.yml` not listed. PLAN.md:1517
`○ bug-report.yml  M1` is still absent although M1 is done. PLAN.md:1525 marks `architecture.md`, `telemetry-sources.md`,
`result-schema.md`, `safety.md` as planned (○); all four exist. Tree also has `docs/ROADMAP.md`, `JOURNAL.md`,
`hardware-validation.md`, `history/` unlisted. Document wrong.

A13 (L) PLAN.md:1613, 1619-1626. Tree shows `fixtures/sysfs/` containing intel, amd, arm64, no-hwmon, eacces, edac, wrapping
(also IMPLEMENTATION.md:149 as an example row). Actual `tests/fixtures/sysfs/`: `edac fan frequency powercap thermal`; `procfs/`:
`meminfo stat`. Also listed and absent: root `benchmarks/`, `tools/capture-sysfs.sh`, `tools/gen-golden/`, `config/stress-5h.toml`,
`config/explore-temperature.toml` (only `config/quick.toml` exists). None carries a "planned" mark. Document wrong; see also E2.

A14 (L) PLAN.md:3 "Status: revision 14" and IMPLEMENTATION.md:3 "revision 3" headers describe none of the changes after M1.

## B. Commands and modes stated as present or required that the tree does not have

B1 (H) verification.md:404-419 (§9 `loadforge selftest`), PLAN.md:282 (F2 "`loadforge selftest` -- reference vectors checked before
any long run begins", under "Required, not optional"), PLAN.md:1101 (CLI box `selftest | benchmark | stress | explore`), PLAN.md:1460
(issue template question). Tree: `src/main/cli.cpp:1071-1123` dispatches only `--worker`, `--rerun`, `--check-config`, `stress`
(`--config` or `--test NAME`), `explore`, `journal`, `--version`, `--selftest-digest`, `--help`. No `selftest` command, no
`benchmark` command. README.md:22 lists both as known gaps. `src/verification/golden/golden.hpp:20-24` also refers to "selftest (§9)". Doc phrased as
fact ("Runs the golden vectors and a fast oracle comparison before any long run begins"). Document wrong (or the feature is a missing
1.0 item); ROADMAP.md:171 (34c) acknowledges it but no queue item builds it.
Related internal gap: IMPLEMENTATION.md §1 (28-48) "1.0 requires" and "Explicitly not in 1.0" mention neither selftest nor benchmark mode,
and no milestone M0-M7 schedules them, though PLAN F2 calls selftest required.

B2 (M) PLAN.md:1161-1173 (§4.4), 783-806 (F17), 883-886 (F18 benchmark paragraph). Benchmark mode is described as a working
experiment type (settle to a defined idle band, repeats with variance, expensive oracle work outside windows). Tree:
`config::Mode::kBenchmark` parses (`src/config/config.cpp:29,41`) but `stress --config` refuses any mode but `stress`
(`src/main/cli.cpp:577-578`); nothing implements the benchmark discipline. Explore takes flags, not a configuration file, and the mode is set
internally (`src/main/trial_runner.cpp:266`; JOURNAL §2.57). Design text, not built; report only because it is stated without a
"not built" mark. Related: PLAN.md:1626 `explore-temperature.toml` contradicts JOURNAL §2.57. Document wrong on the file.

B3 (L) PLAN.md:1168, 1173 "(stress) `dT/dt` ... recorded, labelled non-comparable"; "its recorded slopes are labelled contextual". No such label in
`src/` (grep "contextual", "comparable" next to slope: none). `result-schema.md` gives `slope_per_second` and `mode`, nothing marks it
non-comparable. Doc ahead of code.

B4 (H) PLAN.md §4.1, F9, F13, 2002 and safety. PLAN.md:1104 "Safety: limits · watchdog · emergency stop", F13 (659) "uncorrected
errors ... candidate emergency stop", risk register 1989 ("Damage to poorly-cooled hardware ... Mitigation: Conservative defaults, controller
safety"), PLAN.md:521 "software thermal protection is a backstop". Tree: the temperature ceiling is parsed and reported NOT ENFORCED
(`docs/safety.md:19-50`, ROADMAP.md:166 item 34a); no emergency stop on uncorrected ECC either. Duration/stop escalation and the memory
ceiling are enforced. Design text; the register entry presents an unbuilt mitigation as present. Document should say so until 34a lands.

## C. Verification doctrine against the oracles that were built

C1 (M) verification.md:61 and PLAN.md:579 (FFT oracle "Direct O(n^2) DFT on small blocks, plus Parseval, plus round-trip"). Tree:
`src/workloads/fft/spec.hpp:100-103`: round trip "is not checked"; oracle is direct DFT at sampled bins plus Parseval
(`src/verification/oracle/fft/fft_oracle.hpp:1-12`). Document wrong.

C2 (M) verification.md:62 and PLAN.md:580 (Sparse "Parallel CG with preconditioner"). Tree: `src/workloads/sparse/spec.hpp:64-66`:
conjugate gradients without a preconditioner. Document wrong.

C3 (M) verification.md:68 and PLAN.md:586 (Graph oracle "Scalar BFS on a subgraph; triangle inequality"). Tree: the oracle recomputes the whole
output, contract exact (`src/workloads/graph/spec.hpp:104-111`; `src/verification/oracle/graph/graph_oracle.hpp:1-9`); a subgraph is
explicitly not used and the triangle inequality is only a cross-check in tests. Document wrong. (verification.md:170 also lists exact
only for "integer arithmetic, sorting, compression ... discrete structures"; the graph workload's PageRank is floating point yet exact
by pinned arithmetic.)

C4 (M) verification.md:60 / PLAN.md:578 (Dense linear: "GEMM / factorization", oracle "residual ‖Ax-b‖"); verification.md:181-183 uses the
solve form as the example derivation; PLAN F18 table (840) uses `A'x = b`. Tree: the workload is C = AB only
(`src/workloads/dense_linear/spec.hpp:14-29`), oracle is dot products on sampled entries of C
(`src/verification/oracle/dense_linear/dense_linear.hpp:1-5`); no solver, no factorization, no residual norm of a solve, so the
`condition_number` field (`record.hpp` NumericalVerificationFailure) has nothing to carry. Document wrong on the pairing; the F18 hazard
it illustrates is real in general but not this workload's.

C5 (L) verification.md:56-69 pairings table has no row for adaptive mesh (bounded residual, oracle `src/verification/oracle/adaptive_mesh`) or
for the mixed run. Particles row says "Barnes-Hut / neighbour lists"; tree uses a cutoff neighbour search and explains why Barnes-Hut was
rejected (`src/workloads/particles/spec.hpp:25-31`). Doc imprecise.

C6 (M) verification.md:149-152 and PLAN.md:891-895, 1999, 2000 (control state "controller-owned", "in the controller's address space",
"seeds, indices, tolerances, and expected checksums", "re-validated on every use"). Tree: the three copies live in the runner, which runs in the
worker process (`src/verification/integrity/control_state.hpp:1-50`; `slice_runner.cpp`); they protect seed, next index, the two duty periods
and the line size only, not tolerances or expected checksums; and the vote is once per unit, not on every use (JOURNAL §2.39,
`docs/journal/02-input-integrity.md` "The vote happens once per unit", the per-step vote rejected). Placement rules (separate mappings,
per-copy CRC-32, exclusion before voting, no single copy operation, disagreement stops the run) do match. Document wrong on owner, scope
and frequency. The "separated by more than one page so they do not share a DRAM row" requirement (PLAN.md:902) is not enforced; the header
states it is the kernel's choice (`control_state.hpp:39-43`).

C7 (L) verification.md:296-297 "Transients are counted, rate-tracked across the run, and correlated with temperature, frequency and ECC
events". Tree: a non-reproducible transient is one isolation finding (`src/verification/isolation.hpp:196-209`) with the telemetry attached to the
finding; no per-run transient rate or trend exists (grep "transient" in `src/`: only the isolation finding). Doc ahead of code.

C8 (L) verification.md:239-243 "automatically re-runs the failing unit". The tree caps it at 16 wrong results per worker
(`src/controller/isolate.hpp:36`, `src/main/cli.hpp:120,149`), and `explore` never isolates (JOURNAL §2.56). Neither limit is stated.

C9 (M, internal contradiction among documents) Bit fields of a bounded failure. PLAN.md:91 (rev. 7), verification.md:379-380 and
IMPLEMENTATION.md:530 say the XOR, popcount and bit-classification fields of `NumericalVerificationFailure` are "null and reported as
not-applicable"; IMPLEMENTATION.md:530 even owes "a T5 asserting the null fields are null". Tree and result-schema.md:134-146: the type
has no such fields at all (`src/verification/record.hpp:42-48` header comment: "not null ones, none"); the JSON writes only the listed members.
result-schema.md is right; the other three should say "absent by construction".

C10 (L) verification.md:358-367 and IMPLEMENTATION.md:504 name three classes (single-bit, multi-bit in one word, whole-cache-line). Tree has
four, `kSingleBit kMultiBitInWord kWithinCacheLine kAcrossCacheLines` (`src/verification/record.hpp` `BitClass`); "whole-cache-line"
is not one of them (record.hpp's own comment notes the mapping). Also verification.md:361 lists "expected bytes, actual bytes, XOR delta" as
the record; tree stores a window of at most 64 bytes with offsets and a popcount over the whole comparison, derives the XOR, and may carry
no difference at all when the output could not be turned into bytes (`Difference` is optional); a `Domain::kDerived` difference holds decoded
rather than stored bytes. Doc imprecise: ":genuinely known" expected bits is only true for kStored.

C11 (L) verification.md:97 and PLAN.md:1624 name `tools/gen-golden/` as the regeneration route. It does not exist
(`src/verification/golden/golden.hpp:28` says "the integration stream's", `docs/streams/11-integration-m2.md:56` lists it, ADR 0004:119
assigns it); stream 11 is merged and the directory is absent. No other regeneration tool is in the tree; the only golden file is the
mock vector `tests/fixtures/golden/verification/golden/golden_mock-scalar.golden`.

C12 (L) verification.md §8 field list (336-341) omits `physical_package`; the tree adds it, and `physical_core` is the id within the package
(`record.hpp` Provenance, result-schema.md:30-31). Also adds `decomposition` and `footprint_bytes`. Doc incomplete.

## D. Determinism and flags (mostly consistent)

D1 (L) determinism.md:190-198 ("CI asserts it", "the `LOADFORGE_SIMD=scalar` CI job ... comparing its outputs turns the portability job into a
cross-architecture determinism check"). What CI compares is `loadforge --selftest-digest`, which folds the core corpus and the scalar paths
of compression and dense-linear only (`src/main/selftest.hpp:17-33`, `.github/workflows/portability.yml:53-82`). Sort and the other
BitExact/Fixed workloads are not in the cross-architecture comparison. Doc overstates coverage.

D2 (L) PLAN.md:1234 "SSE2 / AVX2 / AVX-512" and determinism.md:160-165 AVX-512 lanes. Tree: `IsaPath` includes `kAvx512` but no workload has an AVX-512 kernel and
`create()` refuses it (JOURNAL §2.35); there is no SSE2 path. PLAN.md:1235 "Timing: `rdtsc` ... `cntvct_el0`": no use anywhere in `src/` (clocks are
`clock_gettime`, IMPLEMENTATION.md:252). Design text phrased as the platform table. Document wrong or aspirational.

D3 (L) determinism.md:44-89 class assignments are consistent with the descriptors: compression, sort BitExact; dense_linear, fft, stencil, monte_carlo, graph, particles,
sparse, raytrace BitExactFixedDecomposition; adaptive_mesh BoundedResidual (`src/workloads/*/*.cpp` Descriptor lines). Graph is declared at the weaker class for the whole
workload (`src/workloads/graph/graph.cpp:361`), not split by output as determinism.md:51-52,66-69 reads. Minor.

## E. Other contradictions inside or between documents

E1 (M) Controller threading. PLAN.md:1303-1304 (§4.6) "the controller *is* multithreaded"; PLAN F9 (513-515) "workers are spawned before the
controller starts any background threads". Tree: no `std::thread`/`pthread_create` in the controller; one poll loop, sampler driven in rounds
(IMPLEMENTATION.md:264, 295-297; JOURNAL §2.18 "at M1 there are none"; threads exist only in `src/worker/executor/thread_pool.cpp`). The rule remains
valid but the factual sentence is false today.

E2 (L) IMPLEMENTATION.md:413 "Survive writing the stop byte to a pipe nobody reads". JOURNAL §2.49: "the stop byte is gone"; control is `ipc` frames of type 32 with
acks as type 33. Also the control channel is no longer a plain EOF signal (a malformed control frame is counted as the controller lost, exit 124). Document wrong in wording;
the EPIPE/SIGPIPE behaviour is still tested.

E3 (L) IMPLEMENTATION.md:420-436 "The worker's exit codes, in one place". Omits the re-run switch's codes 20, 21, 22 (`src/worker/entry.hpp:56-71`) and the stress/explore run exit
codes (`src/main/outcome.hpp:24-45`: 0, 3, 4, 5, 6; explore 1, 2 in JOURNAL §2.56). The 119-125 table itself matches `entry.hpp:45-47` and `platform/process.hpp:55-58`.

E4 (L) PLAN.md F7 (399-407). "verify no swap is in use and abort if swap-in begins mid-run" and "huge-page strategy ... an explicit recorded choice". Tree: `SwapTotal`/`SwapFree` are
sampled (`src/telemetry/sources/memory/memory.hpp:21-22`) but nothing aborts on swap-in; the verified runner always maps ordinary locked pages (JOURNAL §2.39, `docs/journal/02-input-integrity.md`), and the
fingerprint records the host's THP policy, not a choice of the run (`fingerprint.hpp`). `platform/memory.hpp:17-30` supports `kNone/kTransparent/kExplicit`, unused by the runner. Design ahead of code.

E5 (L) PLAN.md:744 and 1996 "Optional remote sink (a socket)" for the journal. None in the tree (grep "socket" in `src/`: topology only). Marked optional; listed as a mitigation in the risk register without a "not built" note.

E6 (L) PLAN.md:1316-1323 (implementation note). "`--worker-executable PATH` lands when a live-medium user first needs it": still absent; consistent with the tree. `platform::ExecutablePath` exists and refuses without fallback (`src/platform/process.cpp:68`).

E7 (L) IMPLEMENTATION.md:35 "Twelve integrated workloads, each with an independent oracle and a declared determinism class". The registry holds eleven (README.md:20: "eleven verified workloads and two scheduled runs"); `mixed` and `cycling` schedule the eleven and have no oracle or class of their own (`src/workloads/mixed/`).

E8 (L) platforms.md:157-158 telemetry table row "Memory bandwidth -- uncore IMC counters" and PLAN F12 "measured DRAM bandwidth". No source reads uncore IMC (`src/telemetry/sources/` has none); only logical bandwidth (`Plan::logical_bytes`, result `logical_bytes`) is reported. Marked "capability-probed" so it is design text, but the table lists it beside shipped sources.

E9 (L) result-schema.md:104-106 (signature summary, counter): `{ type, samples, delta, rate_per_second | null }`. `src/controller/results/document.cpp:317-324` also writes `resets` (the stated absence rule and the verdict depend on it, JOURNAL §2.44). Document wrong (incomplete).

## F. Checked and found consistent

- determinism.md §2 class definitions, §3 fixed-shape reduction, Philox as the counter-based RNG (`src/primitives/rng/philox.hpp`), no `std::cos/std::sin/std::exp` etc. on the code paths grepped
  (twiddles are a committed table `src/verification/oracle/fft/twiddle_table.inc`), `long double` prohibition, NaN canonicalisation statement.
- determinism.md §4 flag policy: `-ffp-contract=off` is applied to every library and executable created through `cmake/Components.cmake:84,105` and the core/platform targets in
  `CMakeLists.txt`; fast-math flags are rejected at configure time (`cmake/CompilerWarnings.cmake:37`). Explicit FMA by target attribute (JOURNAL §2.35) is compatible with §4.
- determinism.md §3 FpEnvironment table: `src/platform/fpenv/fpenv.hpp` models rounding, FTZ, DAZ (x86 only), DN (AArch64 only), establishes it before threads start and re-asserts it
  (`src/worker/verified_run.cpp:133,167`); failure is a startup/configuration error, not a verification record.
- determinism.md §5 thread counts {1,2,3,4,7}: `tests/determinism/verification/golden/golden_determinism_test.cpp:45,85` and the per-workload `tests/determinism/workloads/*`.
- verification.md §5 three outcomes and §8 record kinds `ExactBitCorruption`, `NumericalVerificationFailure`, `VerificationNotPerformed` with all provenance fields optional; blocks-compared recorded;
  isolation takes only the first two, by type (`src/verification/isolation.hpp:27-52`); ladder order same core, other core, single thread; clean first rung is a non-reproducible transient; `VerificationNotPerformed` never isolated.
- verification.md §4a bracket (opening check, consume, verify, closing check), `Side` input/output, `InputIntegrity` with corrupt-before/during-consumption, per-copy checksums, exclusion before voting, disagreement stops the run.
- verification.md §7a crash signals as unattributed faults (supervision fates, fake-worker catalogue).
- PLAN F9/§4.6: one executable re-executed with `--worker`, fork then exec of `/proc/self/exe`, pdeathsig then parent check, own session per worker, exit 124 for a lost controller, robust-lock rule with no process-shared lock in `src/` (grep of `PTHREAD_PROCESS_SHARED`, `sem_init`, futex: none), seqlock heartbeat region.
- PLAN F12 fingerprint fields: all present in `src/controller/results/fingerprint.hpp`; result-schema.md fingerprint table matches that header; `schema_version` 3 matches `src/controller/results/result.hpp:50`; F17 `observers` fields exist and `controller_overhead` is computed (`src/main/run_result.cpp:214`).
- PLAN F16 journal: ~200-byte fixed-width records, 5 s interval, fsync not retried, unterminated journal detected and kept (JOURNAL §2.48, §2.53; `src/controller/journal/format.hpp`).
- PLAN F5 explore: successive halving, interleaved reference, replicates, variance, objective bound to the capability report before the first trial (`src/explore/search.hpp`, `objective.hpp`).
- PLAN §7.3 coverage mechanics: `--exclude-throw-branches --exclude-unreachable-branches` in `tools/coverage.sh:40-41`; exclusion count zero in `tools/coverage-exclusions.txt`; mutation allowlist empty and keyed `path:line:col:mutator`.
- PLAN §5 runtime deps allowlist (`tools/check-runtime-deps.sh:39-42`), toml++ vendored, GPL-3.0-or-later.
- IMPLEMENTATION §4 worker exit codes 0/119/124/125 and launcher 120-123 match `src/worker/entry.hpp` and `src/platform/process.hpp`; "no process-shared lock today" matches the tree; `stress --test NAME`, `--check-config FILE` exist; M1 exit narrative matches JOURNAL §2.18-2.27.
- platforms.md toolchain rows, cgroup v1/v2 handling, mull 0.34.2 sharded weekly, static build rows are consistent with `CMakePresets.json`, `.github/workflows/*`, `src/resources/`.
- DESIGN-DRAFT.md is preserved verbatim with an editorial note pointing to PLAN.md; its `loadforge benchmark`, `stress --hours`, `explore --objective` examples are historical and were not treated as findings.

## G. Summary of what to change first

1. PLAN.md §11, F21's closing limitation, and §8's M0 row (A1-A3); IMPLEMENTATION.md §10 (A4, A5).
2. IMPLEMENTATION.md §3 ledger: mark shipped modules, replace the `platform/{sysfs,procfs,perf_events,edac}` rows with `telemetry/sources/*`, add `worker/control`, `worker/executor`, `platform/isa`; and either build a gate for the table or stop claiming one (A6-A8).
3. verification.md §9, PLAN F2 and the CLI box (B1) and the issue-template question: decide whether `selftest` is a 1.0 item and put it in IMPLEMENTATION §1 and ROADMAP, or state it is not built.
4. Pairings table (C1-C5) and the control-state paragraph (C6); the "null and not-applicable" wording (C9).
5. PLAN §6 tree (A7, A12, A13).
