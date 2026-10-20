# Comprehensive project review

- **Purpose:** whole-project read-through of every crafted file (sources, headers, tests, tools, workflows, configuration, documentation), to record what each component is, what it is for, the impression it leaves, and every defect, contradiction or risk found. Bulky generated material (vendored `toml.hpp`, the LICENSE body, mutation-tool fixture captures, logs) was deliberately skipped.
- **Date and time:** 2026-10-19 04:49 UTC.
- **State reviewed:** branch `feature/controller` at commit 115fb9e with the uncommitted controller increment and documentation reconciliation in the working tree (27 dirty entries); `main` at 0a3c325.
- **Companion:** `reviews/reaction-plan-comprehensive-review-2026-10-19-0449.md` holds the plan of reaction to the findings below.
- **Convention:** findings are numbered `R-nn` in the order they were recorded. Severity is one of *major* (a correctness defect that can produce a wrong result in the field), *medium* (a robustness defect with a plausible trigger), *minor* (hygiene, wording, or an oddity with no field consequence), *doc* (documentation contradicts code or itself), *info* (descriptive: what the thing is and how it reads). Informative findings are written down as the component is touched, so that they are not lost.

## 1. Defects and risks found during the read-through

### R-01 — major — seqlock writer in the heartbeat region is under-ordered on weak memory

- Where: `src/worker/ipc/heartbeat.cpp`, `publish()`.
- What the code does: it loads the slot sequence relaxed, stores `start + 1` with release, then stores the timestamp, progress, phase and pid fields relaxed through `std::atomic_ref`, then stores `start + 2` with release.
- Why it is wrong: a release store orders the operations *before* it; it does not prevent later relaxed stores from being moved *ahead* of it. On ARM64 (and under the C++ memory model in general) the field stores may become visible before the odd sequence store. A reader that loads the sequence (even), reads the fields, fences, and re-loads the sequence (still even and equal) may therefore have read fields that belong to the *next* publication, i.e. a torn sample is accepted as clean. The reader side is correct (acquire load, relaxed field loads, acquire fence, re-load); the writer side is the half that is missing a fence.
- Correct shape: store the odd sequence (relaxed is enough), then `std::atomic_thread_fence(std::memory_order_release)`, then the field stores, then the even sequence store with release (or a release fence followed by a relaxed store). This is the classic seqlock writer; the documented argument in `tests/unit/worker/ipc/heartbeat_test.cpp` (the "P5 synchronisation" comment) describes the release store as if it ordered both directions and needs correcting at the same time.
- Why no gate caught it: x86 TSO never reorders stores, so every test, every TSan run and every CI job on x86 passes. ThreadSanitizer reports data races, and every access here is atomic, so there is no race for it to report; it does not judge whether a seqlock has the fences it needs. The arm64 jobs run on native runners and pass too: the window is a reader sampling exactly while a field store has overtaken the odd sequence store, and a 20 000-beat test not hitting it is not evidence that it cannot happen. This is a defect that only review finds. (Corrected 2026-10-20: the first version of this paragraph said the arm64 job was emulated; it is native.)
- Field consequence: the controller's staleness and progress checks on a worker could act on a mixed sample: a timestamp from one beat and progress from another. On the current slot layout the worst case is a spurious "stalled" or a missed "stalled" verdict for one tick; with the planned richer slot contents it would grow.

### R-02 — medium — the controller process is not immune to SIGPIPE

- Where: `src/controller/orchestration.cpp`, `Run::request_stop`; `cli::run_stress` and `Controller::run` never call `ignore_signal(SIGPIPE)`. Only `worker::run` does.
- What happens: `request_stop` writes the stop byte to `worker.control.write_fd` for every worker that has not been reaped and is still at step `kNone`, tolerating only `EPIPE` as an error. If a worker dies between one tick's reap and the next tick's deadline-driven `request_stop`, the write hits a pipe whose read end is closed. The kernel delivers SIGPIPE *before* `write` returns `EPIPE`, and with the default disposition the controller dies with signal 13, taking the run (and all other workers, now orphaned until their own controller-lost detection fires) with it.
- Trigger: any worker that exits at or just before the configured deadline, which the "stop at deadline" path is precisely designed to meet. The exhaustive `tests/unit/controller/orchestration_test.cpp` cannot see this because the fake syscalls return `EPIPE` without a signal.
- Fix: ignore SIGPIPE through the seam in the controller (either at the top of `Controller::run` or in `run_stress`), add a fake-driven test that the call is made, and add a row to the behaviour catalogue for "worker closes its control pipe before the deadline stop".

### R-03 — minor — `whole_document_fault` has external linkage without a declaration

- Where: `src/config/config.cpp`.
- `ConfigError whole_document_fault(...)` is defined outside the anonymous namespace and is not declared in `config.hpp`. It is therefore an exported symbol with no header, which `-Wmissing-declarations` would flag if it were enabled for this translation unit, and it widens the module's surface for no reason. Move it into the anonymous namespace.

### R-04 — minor — wording of the torn-slot detail

- Where: heartbeat observe path and the controller's escalation message.
- A torn slot after 64 retries produces a detail of the form "retries 64, expected 0". The phrase "expected 0" reads as if the retry count were a measured quantity against a budget; the real statement is "the slot was never observed consistent in 64 attempts". Reword so the operator reads the right thing.

### R-05 — minor — `drain_after_death` worst case is long

- Where: `src/controller/orchestration.cpp`.
- The post-death drain is bounded by 1024 drains times the stop grace. With a 1 s stop grace that is about 17 minutes of a dead worker's channel being drained before the controller gives up. The bound exists and is documented, so this is not a hang, but the product of the two constants is larger than an operator would expect. Consider bounding by wall time instead of by drain count, or lowering the count.

### R-06 — minor — one surviving mutant from the earlier controller mutation sweep

- A mutation that changes the loop bound in the read-count assertion survived because the test only asserts `read_count > 1000`. The surviving mutant is not in the allowlist and the sweep was run with the scratch script, not the gate, so this is a test-strength gap to close before the controller increment is gated.

### R-07 — minor — clang-tidy covers `src/*.cpp` only

- `.clang-tidy` and the workflow invoke the tool on sources under `src/`; tests and tools are not analysed. This is a deliberate scope reduction but is not written down anywhere; either widen or document it.

### R-08 — doc — Q10 is closed in the decision log but still open in two places

- `docs/PLAN.md` §11, final paragraph, and the `docs/IMPLEMENTATION.md` §9 table both say Q10 (worker process model) is open and blocking and recommend a single process. The decision log (PLAN rev 14) closed Q10 in favour of multiple processes, which is what the code implements. `IMPLEMENTATION.md` §4, M5 criterion 4 ("Q10 revisited") is the same stale thread.

### R-09 — doc — NOTICE and MANIFEST still say nothing is vendored

- `NOTICE` says "No component is vendored under third_party/ yet" and the header of `third_party/MANIFEST.toml` says "Nothing is vendored yet", while the same manifest lists the vendored toml++ entry and the licensing workflow checks it.

### R-10 — doc — CONTRIBUTING status paragraph is stale

- `CONTRIBUTING.md` still describes the project as at M0 with "the first core module" ahead; the tree has core, platform, topology, config, worker, controller and reporters.

### R-11 — doc — platforms.md misdescribes the mutation job and cgroup support

- `docs/platforms.md` says mull is "CI-installed, nightly"; the mutation workflow is `workflow_dispatch` only. It also says cgroup "v1 detected and reported unsupported"; the CI container runs cgroup v1, the memory budget reader reads it, and `docs/JOURNAL.md` §1.18 says both hierarchies must be read.

### R-12 — doc — issue template and CONTRIBUTING reference a tool that does not exist

- `.github/ISSUE_TEMPLATE/unsupported-platform.yml` and `CONTRIBUTING.md` ask reporters to attach the output of `tools/capture-sysfs.sh`. That script is planned for M3 and is not in the tree. CONTRIBUTING says so; the template does not.

### R-13 — doc — five contradictions already logged in JOURNAL §6 remain open

- The journal's own list (README claims vs. tree, the PLAN fallback-vs-override wording, and three others recorded there) is still unresolved in the working tree. Not re-derived here; the reaction plan carries them as one item.

### R-14 — info — things that are in good shape and should be kept

- Syscall seam discipline: every kernel interaction goes through `platform::Syscalls`, the fake is *stricter* than the kernel (it refuses calls the real kernel would tolerate), and decisions live above the seam. This is what makes 100 % branch coverage with zero exclusions honest rather than cosmetic.
- The exhaustive corruption test for the record channel, and the real-kernel supervision tests (T8) that corroborate the fake-driven ones.
- Every gate is fail-closed and has its own tests, including the mutation gate's fixtures for the tool misbehaving.
- The journal is a genuine engineering record; the numbered sections are cited from code comments and tests, which keeps the reasons next to the rules.

## 2. Component-by-component findings (recorded as touched)

The sections below were written while re-touching each component after the full read-through; each records what the component is, what it is for, and the impression it leaves, with any additional findings numbered in sequence.

### 2.1 `src/core` — value types and parsers with no platform knowledge

- **What it is:** `Result<T,E>` (a C++20 stand-in for `std::expected`, variant-backed, misuse throws `bad_variant_access` rather than being undefined), `Ok`, `ByteSize` (absolute bytes or a 1..100 % share, the share deliberately *not* resolved here), `Duration` (`std::chrono::milliseconds` alias with a single-unit parser and a round-tripping renderer), `ParseError` (six specific reasons with a `describe`), `parse_number` (the one place that decides empty/sign/overflow for every suffixed value), `crc32` (IEEE reflected, with `crc32_update` for header+payload without copying), and a CMake-generated `version.hpp`.
- **Purpose:** keep every decision that can be made from text alone out of the platform layer, so that it is testable with no machine in the loop.
- **Impression:** small, exact, and the header comments carry the reasons (why `noexcept` on the Result constructors is load-bearing for coverage, why binary units only, why a CRC and not a hash). The `noexcept(is_nothrow_move_constructible_v<T>)` story is the clearest example in the tree of the project's "make the edge reachable rather than excuse it" rule.
- **R-15 — info:** `Result::value_or` copies out of a `const&` and so is unusable for move-only `T`; nothing uses it that way today, and a `&&` overload can be added when needed. Not a defect.
- **R-16 — info:** `ByteSize::percent` is a factory returning `Result<ByteSize, ParseError>`, which means the header depends on `parse_error.hpp`; a core type that cannot be mis-constructed is worth that dependency.

### 2.2 `src/platform/syscalls.hpp` — the seam

- **What it is:** one abstract class with 26 pure virtual methods, each a translation of exactly one system call (open/read/close/readlink, fork/exec/prctl/getppid/waitpid/kill, clock_gettime, affinity get/set, mmap/munmap/mlock/madvise/mbind, memfd_create+ftruncate, shared mmap, fcntl clear-CLOEXEC, pipe2, write, poll, sigaction SIG_IGN, getpid, non-blocking waitpid). Raw ids and flags cross the seam (clock id, madvise advice, mmap extra flags) so the seam never decides anything. `RealSyscalls` is errno-to-`SyscallError` and nothing else.
- **Purpose:** make the P2 paths (each documented failure mode of each fallible call) forceable from a test.
- **Impression:** the header is also the best piece of documentation in the project for *why* the architecture is what it is. The comments record verified kernel facts (root can pin pid 1; munmap of an unrounded hugetlb length is EINVAL; readlink cannot report truncation) next to the method that depends on them.
- **R-17 — info:** `exec` returns a bare `SyscallError` because success does not return; `parent_pid` and `process_id` return bare `pid_t` because they cannot fail. The interface is honest about which calls have a failure arm, which is what lets the obligation ledger count them.
- **R-18 — minor:** `poll_readable` takes `std::vector<int>` by const reference and returns a `std::vector<Readiness>`, so every controller tick allocates. At the controller's tick rate this is harmless; note it so it is not "optimised" blindly later by someone who thinks it is a hot path.

### 2.3 `src/platform` — the decisions above the seam

- **What it is:** `SyscallError` (errno + call name + owned subject, with `result_or_error` to keep the fork branch testable); `ExitStatus` (raw waitpid word decoded on demand into exited/signalled/stopped/continued/unrecognised, never collapsed); `FileSystem` (a `/sys` and `/proc` reader: no stat-then-read, short reads, EACCES on read, 128-EINTR bound, 1 MiB ceiling); `Clock` (three clocks per reading: RAW for durations, MONOTONIC for timestamps, BOOTTIME minus MONOTONIC for suspend with a 1 s threshold, backwards is an error); `Affinity` (mask helpers, `missing_from` for cpuset-vs-topology, kernel answers classified rather than pre-empted); `Memory` (map → bind → advise → populate → lock, each step's refusal classified, explicit 2 MiB huge pages with rounded length remembered because munmap needs it; `Mapping` is move-only and `release()` reports what the destructor cannot); `WorkerLauncher`/`ForkOutcome`/`ExecutablePath` (fork then exec of the same binary resolved from `/proc/self/exe`, PR_SET_PDEATHSIG = SIGKILL with the fork-to-prctl race closed by re-reading the parent pid, startup failures as exit codes 120..123); `WorkerPool`/`SpawnOutcome` (N ≥ 2 as a testing rule, partial spawn unwinds itself, strangers reaped and skipped, no restart by design).
- **Purpose:** every kernel fact that influences a verdict is decided in a pure function or above a substitutable seam, so both arms are tested.
- **Impression:** consistently excellent. The pattern "classify the kernel's answer, never pre-empt it" is stated once in `Affinity` and then referenced from `Memory` rather than restated, which is how the comments stay accurate. The measured clock numbers (426 ms NTP slew over 140 s) are the kind of evidence that makes the design argument hold.
- **R-19 — doc:** `fs.hpp` says "the controller is multithreaded by construction". The controller that exists runs one loop on one thread; the statement describes the M3 telemetry sampler that is still planned. The thread-safety note itself is still correct and useful; the justification should say "will be".
- **R-20 — minor:** `WorkerLauncher::kParentDeathSignal` is the literal `9` with a trailing comment rather than `SIGKILL`; `supervision.cpp` likewise names the startup exit codes as literals 120..125 to avoid a header dependency, with the worker's tests pinning them. Both are deliberate and documented; recorded so that nobody "fixes" one without the other.
- **R-21 — info:** `WorkerStartupFailure` codes 120..123 and the worker's own 124 (controller lost) and 125 (bad arguments) form one contiguous reserved band that `classify_exit` in the controller depends on. The band is not written down in one place; `docs/IMPLEMENTATION.md` or the worker header is the place for a single table.

### 2.4 `src/topology` — discovery that cross-checks and refuses

- **What it is:** `CpuList` (the kernel's cpumask text format, strict: ascending, non-overlapping, empty is a reading; its own `MalformedValue` type rather than `core::ParseError` because the remedy is different); `Source` (one sysfs attribute remembered across readings so that `kVanished` can be told from `kAbsent`; the six-state `Availability` is the capability model made into a type; signed `integer()` because `core_id` can be −1); `CpuTopology` (online, siblings, core and package ids cross-checked against each other, `DiscoveryFailure` distinguishes "would not answer" from "answered and contradicts"); `CacheHierarchy` (identity is kind + sharer set, not `id`, because arm64 does not publish `id`; nesting checked; heterogeneous depth deliberately not refused; `parse_cache_size` separate from the config parser on purpose); `NumaTopology` (not exposed is a topology with no nodes, never an invented node 0; CPU lists must partition `online`; node memory deliberately not summed because a real VM undercounts).
- **Purpose:** produce a description of the machine that later numbers can be attributed against, or refuse loudly once.
- **Impression:** the most mature module in the tree. Each "NOT checked, deliberately" list is as valuable as the checked list, and each item cites the machine it was learned on. This is the standard the rest of the project should hold to.
- **R-22 — info:** `CacheHierarchy::kMaxCacheIndex = 16` and `CpuList::kMaxCpuId = 65535` are the two bounds that keep a hostile or broken sysfs tree from turning discovery into an unbounded walk; both are documented with the real-machine range. Nothing to change.

### 2.5 `src/config` — the file a user writes

- **What it is:** a TOML schema with a required `schema_version`, a required mode (benchmark/stress/explore), a required duration, optional limits (temperature ≤ 150 C, memory as bytes or share), and a workload list (name, threads all/count, enabled, memory). Unknown keys anywhere are refused by name. `to_toml` renders a document `parse` accepts (round-trip rule). `MemoryBudget`/`ceiling_of`/`resolve_memory` do the arithmetic of the F7 memory ceiling (lower of MemAvailable and cgroup limit) without reading anything.
- **Purpose:** no silent defaults for anything that bounds a run.
- **Impression:** tight. The explanation for the out-of-line `operator==` on `MemoryBudget` (journal §1.22, COMDAT folding hiding counters) is the kind of note that prevents a well-meaning revert.
- **R-23 — info:** the config module reads its file through ordinary `std::filesystem` I/O and not the seam, as PLAN §4.2 draws the line. That is correct and should stay so; it is also why the one external-linkage leak (R-03) is in this file: it is the module with the least seam discipline because it needs the least.

### 2.6 `src/worker` — the process on the other side of exec

- **What it is:** `entry` (the `--worker` switch, `parse_args`/`argv_for` as a round-tripping pair, exit codes 0/119/124/125, `run` wiring attach → report started → loop → report finished/error); `scheduler` (`Pacing` from the controller, the null workload tick: read clock, beat, judge duration, judge controller via slot 0, then one poll on the control pipe for the interval; stop reasons for duration, request, hang-up, stale and unreadable controller); `ipc/heartbeat` (a memfd region, 64-byte header + 64-byte seqlock slots, slot 0 the controller, worker i at slot i+1, `kTorn` after 64 retries, `liveness` and `progressed` as pure functions); `ipc/record` (CRC-32 framed records "LFRC", 16-byte header, 64 KiB payload cap, a reader that never resynchronises, writer and source that do not own their descriptor).
- **Purpose:** a worker that cannot outlive a lost controller, cannot deadlock a survivor, and cannot deliver garbage that looks like a finding.
- **Impression:** the design is right and the comments say why at every step. The one correctness defect of the whole review (R-01) sits here, and it sits precisely where the header says "the synchronisation argument, in full, is in the tests (P5), where it can be checked against a run under TSan": that sentence over-promises, because TSan cannot check writer-side fence placement, and the argument in the test is the one that is wrong.
- **R-24 — info:** `Heartbeat.pid` is `uint32_t` while `pid_t` is signed; the conversion is explicit and pids are non-negative, so this is fine, but the slot layout is a wire format and should be pinned by a `static_assert` on `sizeof(Heartbeat)` and offsets next to the magic, if it is not already.
- **R-25 — info:** a worker that sees the controller's beat go stale exits 124 and the controller reports it as `kControllerLostReported` "the fault is the controller's". A controller that was merely stopped (SIGSTOP, a debugger, a 2 s scheduling stall under memory pressure) will also produce this. The wording is correct for the tool's purpose; the operator-facing message should mention the stall case so that it is not read as a crash.

### 2.7 `src/controller` — the loop that supervises

- **What it is:** `safety` (`StopStep` ladder none → requested → SIGTERM → SIGKILL, `escalation_due` pure in time with `Grace`, `limit_statuses` naming every configured limit and whether this build enforces it, `check_memory` against the budget); `supervision` (`WorkerFate`, 11 values, `classify` pure over `Evidence`, corruption first, exited vs signalled, controller-sent signal vs fault signal vs other); `report` (`RunPlan`, `WorkerReport`, `RunReport` with the `worker_exit_code` path for a failed exec in the child, and the `Reporter` interface the console and the tests implement); `orchestration` (`RunOptions` defaults 100 ms interval, 2 s stale, 1 s + 1 s grace; the one loop: beat, deadline stop, watch and escalate, poll, drain, non-blocking reap; `drain_after_death`).
- **Purpose:** M1 exit criterion 1, with every fate in the misbehaviour catalogue reachable from a fake and corroborated on a real kernel.
- **Impression:** the increment is complete and the classification table is exactly the thing to test as a table. R-02 (SIGPIPE) is the one behavioural gap; R-05 (drain bound) is a tuning question. The orchestration file is the longest in `src/` and the only one whose reading needed two passes; it would benefit from splitting the per-worker state (`Run`'s worker records) into its own header once increment E adds to it.
- **R-26 — info:** `RunOptions.budget` is "none until the reader lands", so today `check_memory` always passes and `limit_statuses` reports memory as not enforced. That is honest, and the console banner says so; it is also the item an operator will most often ask about, so the M2 reader is the natural next increment after the two fixes above.

### 2.8 `src/reporters`, `src/main` — the person-facing edge

- **What it is:** `ConsoleReporter` (banner with the plan and enforced limits, one line per event with time into the run, a summary per worker); `cli` (`collect_args` guarding argc == 0, `run` dispatch, `stress` loading the config and resolving `/proc/self/exe`, `run_stress` public so that fork and exec failures are drivable with a fake); `selftest` (FNV-1a core digest for cross-architecture determinism, with the story of the wrong nineteen-digit basis that self-consistency tests could not catch); `main.cpp` as a three-line shim.
- **Purpose:** keep `main` untestable-but-trivial and everything else in-process testable.
- **Impression:** right-sized. The selftest comment is a good example of the project recording its own mistakes where the next reader will look.
- **R-27 — info:** `--help` does not mention `--worker`, by design (it is not a command a person runs). The worker switch is still discoverable from `strings` on the binary, which is fine; a line in `docs/platforms.md` or the README's operator section saying "you may see a `--worker` process per CPU in `ps`" would pre-empt a support question.

### 2.9 `tests/` — the obligation ledger and the suites

- **What it is:** `tests/obligations.toml`, nine module rows each declaring which path classes (P1 decision, P2 error, P3 boundary, P4 verification, P5 concurrency, P6 lifecycle, P7 capability) the module claims and which tiers (T1 unit, T2 fake-driven, T5, T7, T8 real-kernel) prove them, with a note per row explaining the choice; `tests/support/fake_syscalls.hpp` (a scripted fake that refuses anything not programmed, stricter than the kernel); unit suites per source module under `tests/unit`; `tests/supervision` (T8, real processes, always N ≥ 2); `tests/fixtures/config` (three small TOML files); `tests/tools` (shell suites for the gate scripts, with captured tool-output fixtures under `tests/tools/fixtures/mull`).
- **Purpose:** turn "every execution path" from a slogan into a ledger a script checks: declaring a class without the tier that proves it fails the build.
- **Impression:** the ledger notes are honest about what is not claimed (config has no T2 because file I/O is not behind the seam; core has no P2 because it has no syscalls). The fake is the strongest single test asset in the project.
- **R-28 — info:** the heartbeat test's P5 argument is the place the fix for R-01 must also land; the ledger row for `src/worker/ipc` claims P5, so the correction is in scope of an existing obligation, not a new one.
- **R-29 — minor:** the three config fixtures are the only file fixtures in the tree and are not referenced from the ledger (T8 for config is "a real missing path and a real directory"). Either list them in the row note or move them next to the test that uses them, so a future cleanup does not delete them as unused.

### 2.10 `tools/` — the gates

- **What it is:** `coverage.sh` (gcovr 8.6, throw and unreachable branches excluded, 100 % line, branch and function, zero exclusions enforced by `check-exclusions.sh` against `coverage-exclusions.txt` whose count may only go down); `check-coverage-complete.sh` (every source file appears in the report); `mutation-gate.py` (reads the tool's text and SARIF renderings, requires them to agree, zero unexplained survivors, allowlist keyed by path:line:col:mutator, not-covered mutants never allowlistable); `check-test-obligations.py` (the ledger); `check-dependencies.py` and `check-fetch-centralization.sh` (every fetch goes through one CMake file, pinned); `check-runtime-deps.sh` (the shipped binary links only the C and C++ runtime); `check-dco.sh` (sign-off on every commit).
- **Purpose:** fail closed. Each gate has fixture-driven tests of its own misreadings, including the mutation tool producing malformed, disagreeing, or empty output.
- **Impression:** unusually thorough for a project of this size; the gate tests are what make the gates trustworthy rather than decorative.
- **R-30 — minor:** `mutation-allowlist.txt` is currently empty apart from its header, which is the right state, and R-06's surviving mutant should be killed by a stronger assertion rather than allowlisted; recorded so the easy option is not taken.

### 2.11 `.github/` and `cmake/` — CI and build

- **What it is:** eight workflows: `ci` (runner × compiler matrix, build, test, runtime-deps check, and a trust-chain tooling job running ShellCheck, Ruff, clang-format and clang-tidy), `coverage` (the 100 % gate plus the gate's own tests), `sanitizers`, `portability` (arm64), `mutation` (`workflow_dispatch` only), `licensing` (SPDX and the vendored manifest), `release-static`, `canary-2604` (advisory forward-compatibility). A PR template and two issue templates. CMake presets, warnings-as-errors, floating-point flags per `determinism.md`, sanitizer and coverage toggles, and `Dependencies.cmake` as the single fetch point.
- **Purpose:** every doctrine in `docs/` has a job that enforces it.
- **Impression:** coherent. The advisory canary is a good pattern: it informs without blocking.
- **R-31 — info:** the mutation job being manual-only is a reasonable cost decision, but it means the gate is only as current as the last person to press the button; a scheduled weekly run on `main` would keep the allowlist honest without blocking PRs. This is the same fact behind R-11.
- **R-32 — minor:** clang-tidy's scope (R-07) and the trust-chain job's linter versions are not pinned in the workflow the way the CMake fetches are; a toolchain bump upstream can turn the job red with no change in the tree.

### 2.12 `docs/` and the root documents

- **What it is:** `PLAN.md` (eleven sections: provenance, requirements review, principles, architecture, technology, layout, testing strategy, milestones, risks, open questions, status; revision log inside); `IMPLEMENTATION.md` (what "complete" means, the coverage doctrine, the module ledger, milestones in build order, sequencing, gate evolution, cross-review, risks, decisions waited on, status); `DESIGN-DRAFT.md` (the original design, kept as the design of record); `JOURNAL.md` (numbered engineering findings cited from code and tests, plus the §6 list of known contradictions); `verification.md`, `determinism.md`, `platforms.md`, one ADR; `README.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `NOTICE`, `third_party/MANIFEST.toml`.
- **Purpose:** the reasons live next to the rules, and the status documents say what exists.
- **Impression:** the journal is the best document in the project and the plan is the one most in need of maintenance. Every documentation finding above (R-08 to R-13, R-19) is a case of a status sentence outliving the state it described; none is a disagreement about design.
- **R-33 — doc:** `README.md` "Project status" and `IMPLEMENTATION.md` §10 "Status" are two status sections that must be edited together; today they agree for `main` and will diverge the moment the controller increment lands. One should point at the other.

## 3. Summary

- One major correctness defect (R-01, seqlock writer ordering), one medium robustness defect (R-02, controller SIGPIPE), seven minor items, eight documentation items, and seventeen informative findings recorded next to the component they describe.
- Nothing found contradicts the architecture; every defect is local to one function and fixable without a design change.
- The reaction plan in the companion file orders the work: the two code fixes with their tests first, then the documentation reconciliation as one commit, then the owner decisions that were already pending.
