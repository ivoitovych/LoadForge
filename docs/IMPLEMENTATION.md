# LoadForge — Implementation Plan

**Status:** revision 3 — the module ledger reconciled against the tree on `main`
(§3, §10); revision 2 — Q10 closed (multiple worker processes); X1, X2 and the
obligation ledger landed
**Covers:** what "complete" means, how per-path test coverage is made enforceable rather
than aspirational, the module ledger, and the milestone-by-milestone build order to 1.0.

---

## How to read this, and what it is not

[`PLAN.md`](PLAN.md) is the **design of record**: what LoadForge is, why each decision was
made, the findings F1–F21, the architecture, the risk register. It answers *why*.

This document answers *what gets built, in what order, and what tests it owes*. Where the
two disagree, `PLAN.md` wins on design and this document wins on sequencing — and any
disagreement is a defect in one of them, so §7 records the cross-review that removed the
ones present when this was written.

Three documents are normative and this plan is subordinate to all of them:
[`determinism.md`](determinism.md) (the F1 contract), [`verification.md`](verification.md)
(oracles, contracts, error records), and [`platforms.md`](platforms.md) (supported
components). This plan schedules work; it does not relax a contract.

---

## 1. What "complete" means

**1.0 is the point at which a competent stranger can put LoadForge on an unfamiliar
machine, run it for five hours, and act on what it says.** Concretely:

| # | 1.0 requires | Milestone |
|---|---|---|
| 1 | Twelve integrated workloads, each with an independent oracle and a declared determinism class | M2a–M5 |
| 2 | Both verification contracts demonstrated — exact and bounded — plus `VerificationNotPerformed` | M2a, M2b |
| 3 | A controller that survives every way a worker can die, and reports the telemetry leading up to it | M1, M5 |
| 4 | Full Load Signature from every telemetry source, degrading correctly where a source is absent | M3 |
| 5 | A versioned result schema and run fingerprint, so two runs can be compared honestly | M3 |
| 6 | Worst-case search that reports variance and confidence, never bare verdicts | M6 |
| 7 | Evidence that survives the machine — crash journal | M5 |
| 8 | Static and dynamic release artifacts, validated on real hardware on both architectures | M0, M7 |
| 9 | **Every execution path covered to the standard in §2**, with the gates in §6 enforcing it | all |

**Explicitly not in 1.0:** SSD and discrete-GPU testing, generic PCIe saturation, SVE
(runtime-variable vector length is a separate problem), Windows or macOS, and any
distributed multi-machine mode. These are recorded so that "not yet" is never mistaken
for "forgotten".

---

## 2. The coverage doctrine — what "every execution path" means operationally

The requirement is stated in the foundation documents — [`PLAN.md`](PLAN.md) §7.3 and
[`CONTRIBUTING.md`](../CONTRIBUTING.md):

> **Every line of working code is covered comprehensively.** Executing a line is the
> floor, not the goal. For each line the tests must exercise the distinct *meanings* its
> inputs can take — every boundary of every value domain it touches, every way each call
> it makes can fail, every corner case where its behaviour changes — and they must
> **fail** if that behaviour changes.

That is only useful if it is **decidable**: someone must be able to look at a module and
say whether the obligation is discharged. This section makes it decidable.

### 2.1 Line and branch coverage is the floor, not the definition

The project already gates 100% line and 100% branch coverage. That is necessary and
nowhere near sufficient, for three reasons that matter more as the codebase grows:

1. **Coverage stops carrying information at 100%.** It can no longer distinguish a
   thorough test from one that executes a line and asserts nothing. (`PLAN.md` §7.3.)
2. **Whole classes of path are invisible to it.** An `EACCES` from `open()`, a short
   `read()`, an `EINTR`, an allocation failure, a counter that wraps mid-sample — none of
   these is a branch the compiler can see as uncovered, because the branch *is* covered by
   the success case. The failure arm simply never runs, and gcov reports 100%.
3. **A branch is not an interleaving.** Two threads with fully covered branches still have
   orderings nobody exercised.

So the gate stays, and three further mechanisms complete it.

### 2.2 The path taxonomy

Every execution path in LoadForge falls into one of seven classes. The classification is
the point: each class is *enumerated* differently and *proved* differently, and a module's
obligation is discharged only when every class present in it has been handled.

| # | Path class | How the paths are enumerated | How coverage is proved |
|---|---|---|---|
| **P1** | **Decision** — each outcome of each conditional, each `switch` arm including the default, loops at zero / one / many iterations | Mechanically, by the compiler | 100% branch gate (`tools/coverage.sh`) |
| **P2** | **Error** — each failure mode of each fallible call: every `errno` the syscall documents, allocation failure, short read, `EINTR`, malformed input | By reading the syscall's man page and listing what it can return | Injected through the platform seam; a test per mode (T2, T5) |
| **P3** | **Boundary** — value-domain edges: empty, one, maximum, overflow, negative, zero-length, subnormal, NaN, wrap-around | From the type's domain and the function's contract | Boundary and property tests (T1, T4) |
| **P4** | **Verification outcome** — pass; each corruption *classification*; and not-performed | From `verification.md` §8's three record types | Fault injection (T5) and unrun-check tests (T5b) |
| **P5** | **Concurrency** — the orderings the design depends on: publish-before-read, heartbeat versus timeout, shutdown versus in-flight work | From the written synchronization argument each such site is required to carry | A dedicated ordering test per protocol, plus TSan and ARM64 CI as **corroborating evidence, not proof** (`PLAN.md` §4.5) |
| **P6** | **Lifecycle** — startup, steady state, each abnormal termination (crash, hang, OOM-kill, parent death, signal), restart, teardown | From the process state machine in §4 of this document | Supervision tests with really killed children (T7) |
| **P7** | **Capability** — per telemetry source: present, absent, permission-denied, malformed, disappears mid-run, counter wraps | From the capability model (F3): one row per source × state | Hostile `/sys` and `/proc` fixtures (T2) |

P1 is the only class a coverage tool enumerates for you. The other six are enumerated by a
person, which is why §2.5 makes the enumeration a reviewable artifact rather than a memory.

### 2.3 The obligation: executed, asserted, falsifiable

> **A path is covered when it is (a) executed by a test, (b) asserted on its observable
> result, and (c) demonstrated to fail when the behaviour it guards is broken.**

(a) alone is what a coverage percentage measures, and it is the weakest of the three.
(b) is what an assertion-free test omits. (c) is the one almost always skipped, and it is
what separates a test suite from a decorative one. How (c) is discharged depends on the
class:

| Class | (c) is discharged by |
|---|---|
| P1, P3 | **Mutation testing** — a surviving mutant is proof the tests do not detect a change on that path |
| P2 | The injection itself: the test *forces* the failure and asserts the handling, so the path cannot pass by accident |
| P4 | **Fault injection** — corruption is introduced deliberately and the classification asserted; and for not-performed, the check is made impossible and the report asserted |
| P5 | The ordering test fails if the memory order is weakened — a test that still passes with `seq_cst` substituted everywhere has not tested the protocol |
| P6 | The child is really killed, really hung, really OOM-ed — not simulated by a flag |
| P7 | The hostile fixture: the source really is missing or really returns `EACCES` |

### 2.4 Reading (c) honestly for P5

Point (c) is achievable for six of the seven classes. For **P5 it is not**, and the plan
says so rather than implying otherwise. TSan detects data races; it does not enumerate
permitted executions. ARM64 CI gives a reordering an *opportunity* to appear; a
rare-but-legal one may never occur in a run. So for concurrency the obligation is:

- prefer not to have the path at all — mutexes and `seq_cst` by default;
- where a weaker order is justified by measurement, a **written synchronization argument**
  plus a dedicated ordering test;
- and the residual risk stays in the register at Medium, not marked away.

This is the one place in the plan where "comprehensive coverage" is knowingly not
achievable, and the mitigation is to minimize the surface rather than to claim the
coverage.

### 2.5 The path inventory, and the gate that enforces it

Enumeration for P2–P7 is human work, so it must leave a reviewable trace or it will be
skipped under deadline. The mechanism, **in place since this plan was written**:

**`tests/obligations.toml`** declares, per module, which path classes it contains and
therefore which tiers it owes:

```toml
[[module]]
path    = "src/platform/sysfs"
classes = ["P1", "P2", "P3", "P7"]
tiers   = ["T1", "T2"]
fixtures = ["sysfs/intel", "sysfs/amd", "sysfs/arm64", "sysfs/no-hwmon", "sysfs/eacces"]
```

**`tools/check-test-obligations.py`** fails the build when a module in `src/` is absent
from the ledger, when a ledger entry names a module that no longer exists, when a declared
class is not one of the seven, when an entry omits P1 (every module has decision paths, so
an entry without it was filled in rather than classified), when a declared tier has no
tests under it, when a named fixture does not exist, or when a process-shared lock in
`src/` is not declared with its robustness test (below). It is built to the F20 standard:
it fails closed on a ledger it cannot parse or a source file it cannot read, and an empty
ledger with a populated `src/` is a failure, not a pass. It runs in the coverage
workflow, next to the gate whose blind spots it covers, and has 39 tests of its own.

**Since ADR 0004 the ledger is the root file plus one fragment per module**,
`tests/obligations.d/<module path below src>.toml`, so that components built in parallel
never edit one file; a fragment declaring any module but the one it is named after is
refused. A row may also declare the contract suites its module **provides** and
**consumes** (ADR 0003): each must be a suite under `tests/contracts/`, and the module's
own tests must instantiate it — against the implementation, or against the module's own
mock. That instantiation is the cross-execution ADR 0003 requires, and the gate now
refuses a declaration without it.

**The `EOWNERDEAD` obligation is recorded per lock.** A source file that names a
process-shared or robust lock primitive (`PTHREAD_PROCESS_SHARED`,
`pthread_mutexattr_setrobust`, `sem_init`, a futex and the like — matched in the raw
text, comments included, so the gate errs towards refusing) must be named by a
`process_shared_locks` entry in its module's row; each entry names the lock, its file and
a robustness test under `tests/supervision/` that carries `LOADFORGE EOWNERDEAD TEST:
<name>`, and the module must declare P5. A declaration whose file uses no such primitive
is refused as stale. There are none today, by design (PLAN §4.6; the heartbeat region is
a seqlock with nothing to die holding), and the gate prints that it found none rather than
printing nothing.

The classes that cannot be tested without something to force the state carry their own
rules, and the two are forced by **different** means — conflating them was this gate's
first real bug. **P2** owes T2 or T5, because an errno is forced through the substitutable
syscall seam and has no on-disk fixture at all; demanding one of `src/platform/fs` was
wrong. **P6** owes T2, T7 or T8, because a lifecycle transition needs a process to
actually start and die. **P7** owes `fixtures` *or* `fixture_builders`:

```toml
fixture_builders = ["tests/unit/topology/source_test.cpp"]
```

A committed tree holds the states a tree *can* hold — a malformed value, an unexpected
layout, a dangling symlink. It cannot hold the other two. *Vanishes mid-run* is an event,
not a state: only a test that reads a file and then removes it produces it. And a mode-000
file arrives from a clone **readable**, because git records the execute bit and nothing
else — so a committed `eacces` fixture would be read successfully and the test would pass
having demonstrated the opposite of its claim. A named builder must exist and must carry
the marker `LOADFORGE P7 FIXTURE BUILDER`, so the alternative is an escape hatch with a
lock on it rather than "some file exists" wearing a new field name.

This does not prove the enumeration is *complete* — nothing can, since P2–P7 are
judgement. It proves the enumeration was *made*, is *current*, and that the tests it
promises exist. That is the difference between a convention and a gate, and this project
has been bitten enough times to know which one survives.

### 2.6 Unreachable code, and the ladder before an exclusion

An exclusion is the last resort, and the project's own history is the argument: every
exclusion it has been tempted by so far turned out to be a real defect in disguise — an
`assert` that vanished under `NDEBUG`, an `argc == 0` branch made unreachable by living
inside `main()`, a ternary pair no test could enter. The ladder, in order:

1. **Make it reachable.** Move the logic into a free function a unit test can call. This
   fixed two of the three cases above.
2. **Make it injectable.** If it is an error path, route the call through the platform
   seam so the failure can be forced. This is why the seam exists.
3. **Delete it.** Defensive code guarding a condition that cannot occur is not defence, it
   is untested code that will be wrong when someone makes the condition possible.
4. **Only then**, exclude — with an `LF-COV-NNN` ID, a written reason, and a record in
   `tools/coverage-exclusions.txt` reviewed in the same pull request.

The count is zero today. §6 keeps it visible at every milestone.

---

## 3. Module ledger

Every module that will exist, the milestone that introduces it, the path classes it
contains, and the tiers it therefore owes. This is the master list §2.5's gate is built
from; adding a module without adding a row here is itself a gate failure.

**Ledger granularity follows the directory layout**, because that is what
`tools/check-test-obligations.py` can check: one entry per directory under `src/` that
holds source files. A row below may therefore merge with its neighbours or split as the
tree grows — `platform` is one module today and becomes several once `sysfs`, `procfs`
and the rest arrive. A planned row that does not match a shipped directory is a plan, not
a discrepancy; a *shipped* directory that does not match its row is a defect, and the gate
catches it.

**T0 (static), T0b (gate tests) and T9 (sanitizers) are omitted from every row because
they are universal** — T0 and T9 run over the whole tree on every push, and T0b covers
`tools/` rather than `src/`. The "tiers owed" column lists only what a module owes *beyond*
those, so a blank there means a real exemption and never an oversight.

| Module | M | Path classes | Tiers owed | Notes |
|---|---|---|---|---|
| `core/` | M0 ✅ | P1, P3 | T1 | Result, units, duration, byte size — done, 100% |
| `main/` | M0 ✅ | P1, P3 | T1, T8 | CLI dispatch, selftest digest |
| `config/` | M1 ✅ | P1, P3 | T1, T8 | **Shipped.** TOML parse, schema validation, resolution. Corrected against what was built: the failing open is not behind the seam (PLAN §4.2), so it is reached by real files at T8 rather than injected at T2, and the row no longer claims P2 |
| `platform` (seam, `fs`) | M1 ✅ | P1, P2, P3 | T1, T2, T8 | **Shipped.** The substitutable `Syscalls` and the reader above it. Corrected against what was built: the row previously claimed P7 and omitted P3 and T8. `fs` itself has no capability states — a source being present, absent or forbidden is a property of the *telemetry sources* that use it, and those arrive at M3 |
| `platform/clock` | M1 ✅ | P1, P2, P3 | T1, T2, T8 | **Shipped** (JOURNAL §1.11, §1.12). Corrected against what was built: three clocks, not one (`CLOCK_MONOTONIC_RAW` for durations, `CLOCK_MONOTONIC` for timestamps, `CLOCK_BOOTTIME` for suspend detection); P2 because every `clock_gettime` failure is injected; T8 against the real clocks. One directory with `fs`, `process`, `affinity` and `memory`: the ledger row is `src/platform` |
| `platform/process` | M1 ✅ | P1, P2, P6 | T1, T2, T8 | **Shipped** (JOURNAL §2.1): `WorkerLauncher` (fork, `PR_SET_PDEATHSIG`, the orphan race, a session of the worker's own, close-on-exec handover, exec) and `WorkerPool` (spawn N, reap, signal all, partial-spawn unwind, non-blocking reap). Corrected against what was built: the lifecycle paths are forced through the seam (T2) and real children are forked and reaped at T8; the T7 tier is the controller's, which is what a supervision test supervises. The session (JOURNAL §2.20) is what lets the controller kill what a dead worker left running |
| `platform/affinity` | M1 ✅ | P1, P2, P7 | T1, T2, T8 | **Shipped.** The P7 state is a cpuset narrower than the topology, built in the fake (JOURNAL §2.12); the kernel's own answers are asserted at T8 against the calling thread only |
| `platform/memory` | M1 ✅ | P1, P2, P3, P7 | T1, T2, T8 | **Shipped.** mmap, huge pages, NUMA, mlock — each failure mode injected. Corrected against what was built: the huge-page pool is a P7 state (present, exhausted, unreserved) built in the fake, and T8 corroborates each kernel answer the fake models |
| `platform/fpenv` | M2b | P1, P2, P3 | T1, T6 | MXCSR/FPCR assert and re-assert (F15) |
| `platform/sysfs` | M3 | P1, P2, P7 | T1, T2 | |
| `platform/procfs` | M3 | P1, P2, P7 | T1, T2 | |
| `platform/perf_events` | M3 | P1, P2, P7 | T1, T2 | `perf_event_paranoid` is a first-class P7 state |
| `platform/edac` | M3 | P1, P2, P7 | T1, T2 | F13 |
| `topology/` | M1 ✅ | P1, P2, P3, P7 | T1, T2, T8 | **Shipped** (JOURNAL §2.6–§2.11): CPU lists, the capability-classifying `Source`, CPUs with their cross-checks, caches, NUMA. Corrected against what was built: T8 added, because the real `/sys` of every CI runner is read and checked against `sysconf` — a tree the tests wrote is still a tree the tests believe in (F21). The snapshot-before-fork rule is the controller's to honour |
| `worker/scheduler` | M1 ✅ | P1, P2, P3, P6 | T1, T2, T8 | **Shipped** as `src/worker` (JOURNAL §2.17): the internal `--worker` switch, its argument parser, the null workload loop and four exit codes. Corrected against what was built: no barriers (nothing to synchronise in a null workload; they arrive with the first workload), no pool (the pool is `platform/process`); affinity placement and heartbeat emission as planned. T7 arrives with the controller, which is what runs this under supervision |
| `worker/ipc` | M1 ✅ | P1, P2, P3, P4, P5 | T1, T2 | **Shipped** (JOURNAL §2.15, §2.16): the heartbeat region (seqlock slots, no lock, memfd across exec) and the record channel (CRC-32 frames, a reader that never resynchronises). Its own library, `loadforge_ipc`, so the test-only fake worker links the protocol without the real worker's loop (ADR 0003). No process-shared lock, and the ledger gate checks that on every run. Corrected against what was built: the boundary is fuzzed and every single-byte corruption of a multi-frame stream is tried, but at T1 (`tests/unit`), because `tests/fault_injection` (T5) does not exist until M2a creates it — the row claimed a tier before the tier; P4 added because the frame check is a verification with a classification per corruption |
| `controller/orchestration` | M1 | P1, P2, P3, P6 | T1, T2, T7 | Run control: one loop, one blocking call; the deadline's stop request; spawn and teardown; each reaped worker's process group killed with it. T7 is the misbehaviour catalogue against `tests/support/fake_worker` (JOURNAL §2.19) |
| `controller/safety` | M1 | P1, P3 | T1 | The escalation ladder as a function of time; the limits a run states and whether each is enforced |
| `controller/temperature` | 34a | P1, P3, P6 | T1, T7 | The temperature ceiling (`limits.max_temperature_c`): which timeline series are temperatures, what a round showed, a sensor that goes, and what ends a run; `check_temperature` in `controller/safety` refuses a ceiling nothing can hold. Journal fragment `docs/journal/34a-temperature-ceiling.md` |
| `controller/supervision` | M1 | P1, P3 | T1 | Crash classification: the table of fates. OOM is told from a fault and from a plain kill by the cgroup's OOM-kill count, read before the first fork and after the last reap, all or none (JOURNAL §2.19) |
| `resources/` | M1 ✅ | P1, P2, P3, P7 | T1, T2, T8 | **Shipped** (JOURNAL §1.23, §2.19): the memory budget (`MemAvailable`; the cgroup limit under v1 and v2, found through `/proc/self/cgroup` and `mountinfo`) and the cgroup's OOM-kill count, behind the contract the controller consults. Above `topology`, not inside `platform`, because every source is classified with topology's capability model. T8 checks the real machine against the kernel's other figures, including a child really OOM-killed where a v1 cgroup can be made |
| `controller/results` | M3 | P1, P3 | T1, T8 | Schema, fingerprint, aggregation (F12) |
| `controller/journal` | M5 | P1, P2, P6 | T1, T7, T10 | Append-only crash journal (F16) |
| `verification/oracle` | M2a | P1, P3, P4 | T1, T3, T5, T5b | Written **before** the kernel it checks |
| `verification/checksum` | M2a | P1, P3, P4 | T1, T4, T5, T5b | |
| `verification/residual` | M2b | P1, P3, P4 | T1, T3, T5, T5b | Derived tolerances, never chosen |
| `verification/invariant` | M2b | P1, P3, P4 | T1, T4, T5 | |
| `verification/golden` | M2a | P1, P3 | T1, T6 | Regression tripwire, **not** an oracle |
| `verification/isolation` | M2a | P1, P4, P6 | T1, T5, T7 | Re-run ladder; excludes `VerificationNotPerformed` |
| `telemetry/capability` | M2a | P1, P7 | T1, T2 | Probe and report; minimal at M2a, full at M3 |
| `telemetry/sources/*` | M3 | P1, P2, P7 | T1, T2 | One hostile-fixture set per source |
| `telemetry/sampler` | M3 | P1, P3, P5 | T1, T2, T10 | |
| `telemetry/timeline` | M3 | P1, P3 | T1, T4 | Common monotonic timeline |
| `statistics/` | M3 | P1, P3 | T1, T4 | Load Signature, aggregation, ranking |
| `reporters/console` | M1 | P1, P3 | T1, T8 | |
| `reporters/csv`, `json` | M3 | P1, P3 | T1, T8 | Schema-versioned output |
| `primitives/*` | M4 | P1, P3 | T1, T4, T6 | Calibration engine, not user-facing tests |
| `workloads/compression` | M2a | P1, P3, P4, P5 | T1, T3, T4, T5, T5b, T6 | First workload (F14) — exact contract |
| `workloads/dense_linear` | M2b | P1, P3, P4, P5 | T1, T3, T4, T5, T5b, T6 | Bounded contract; AVX2 and NEON together |
| `workloads/*` (3–11) | M4 | P1, P3, P4, P5 | T1, T3, T4, T5, T5b, T6 | Each owes the full set; none is exempt |
| `workloads/mixed` | M5 | P1, P3, P4, P5, P6 | all of the above + T7, T10 | The flagship |
| `explore/` | M6 | P1, P3 | T1, T4, T8 | Search, objectives, drift correction (F5) |

**The contracts between these modules are written (ADR 0004)**, as headers under the
directories above with provider-side suites in `tests/contracts/`, and the modules are
built by the streams of `docs/ROADMAP.md`. Three rows were refined by that work and the
streams follow the refined form: input-integrity re-verification is its own module,
`verification/integrity`, which runs every workload's units inside the bracket (so the
P5 of "verification concurrent with compute" is the runner's, not each workload's); the
sampler is driven in rounds by the controller's loop and has no P5 unless it adds a
thread; and the ISA probe is `platform/isa`. Each contract directory has a ledger
fragment of its own, which the stream that implements it takes over.

Two rules follow from this table and are worth stating separately, because they are the
ones a contributor under pressure will want to bend:

- **No workload is exempt from T3.** A workload without an independently derived oracle is
  not a workload, it is a heater.
- **Every module containing P2 owes an injected test for each error mode**, not one
  representative test for the module.

---

## 4. Milestones in build order

Each milestone lists what is built, the execution paths it introduces, the tests those
paths owe, the fixtures and gates that must land with it, and a measurable exit criterion.
The coverage obligation is identical at every milestone and is therefore stated once:

> **Every milestone exits at 100% line and branch coverage with zero unexplained
> exclusions, with §2.5's obligation ledger current, and with the mutation gate reporting
> zero unexplained survivors over the modules in its scope.**

### M0 — Foundation ✅ complete

Delivered: CMake with seven presets; the 100% coverage gate; the coverage-exclusion gate;
the cross-architecture determinism digest; supply-chain manifest reconciliation; fetch
centralization; SPDX, DCO, attribution and action-pin gates; ShellCheck and Ruff; a
verified-static release artifact; the 26.04 canary; the mutation gate; `core/` and
`main/`. 68 unit tests, 56 trust-chain tooling tests, 100% line and branch, zero
exclusions, CI green on {x86-64, ARM64} × {GCC 13, Clang 18}.

**One M0 item remains open and is carried, not closed** (the other, the mutation gate,
closed on 2026-10-20: Mull 0.34.2 is pinned by SHA-256, the gate's end-to-end test passes
against the real tool, and the job runs weekly on `main`):

1. **`main` is unprotected** — no required status checks; every gate above is bypassable
   by a direct push. Requires owner action in GitHub settings.

### M1 — Core framework and supervision

**Builds:** `config`, `platform/{fs,clock,process,affinity,memory}`, `topology`,
`resources` (the memory budget and the OOM-kill record), `worker/{scheduler,ipc}`,
`controller/{orchestration,safety,supervision}`, `reporters/console`.

This is the milestone that establishes the process boundary, and the one where getting the
seam right pays for the whole project: **every later module's P2 paths are testable only
because `platform/fs` and `platform/process` are injectable.** Building the seam wrong
here means retrofitting testability into ten modules later.

**Execution paths introduced, and what each owes:**

| Class | Paths | Tests owed |
|---|---|---|
| P2 | Every documented failure of `open`, `read`, `mmap`, `fork`, `sched_setaffinity`, `prctl`, `mbind`, `mlock` | One injected test per `errno` per call site, asserting the message and the recovery |
| P5 | Heartbeat publish/observe; shutdown versus in-flight work; the barrier protocol | One ordering test per protocol, each with a written synchronization argument |
| P6 | The full process state machine below | One T7 test per transition, with a really-killed child |
| P3 | Config value domains; duration and size limits; topology degeneracies (1 CPU, no NUMA, no SMT, asymmetric P/E cores) | Boundary tests per domain |

**The process state machine — every transition owes a T7 test:**

```text
                 ┌──────────┐
                 │  START   │
                 └────┬─────┘
       topology snapshot taken (before fork)
                      │
                 ┌────▼─────┐   spawn    ┌───────────┐
                 │ RUNNING  ├───────────►│  WORKER   │
                 └────┬─────┘            └─────┬─────┘
                      │                        │
   ┌──────────────────┼────────────────────────┼────────────────┐
   │                  │                        │                │
worker exit 0   worker SIGSEGV/SIGBUS    worker OOM-killed   worker hangs
   │                  │                        │                │
   ▼                  ▼                        ▼                ▼
 COMPLETE       FAULT (classified)      OOM (classified      TIMEOUT
                                          DISTINCTLY)      (heartbeat stale)
   │                  │                        │                │
   └──────────────────┴────────────┬───────────┴────────────────┘
                                   ▼
                        telemetry preserved, run reported

   And the inverse, which is the dangerous one:
   controller SIGKILLed  ──►  every worker gone within a bounded time
```

The last transition matters more than it looks: an orphaned worker holding every core at
full load with no thermal ceiling enforced is the one genuinely dangerous state the
process split can create. It gets an explicit test, not a hopeful comment.

#### Testing a multi-process supervisor

Q10 is closed: **multiple worker processes, one executable, re-executed with an internal
switch** (`PLAN.md` §4.6). That decision buys fault containment and a surviving telemetry
timeline, and it pays for them in P5 and P6 paths that multiply by worker count. This is
the test structure that makes the trade honest.

**A separate `tests/support/fake_worker` executable, not a test switch on the real binary.**
The controller must be driven against a worker that misbehaves on demand, and the obvious
way to get one — a hidden `--misbehave` flag on `loadforge` — is the wrong way. A tool
whose only product is trust must not ship a binary that can be told to corrupt its own
results. The misbehaviour lives in a test-only executable that is never installed, and a
release gate asserts the shipped artifact contains no test-hook symbols.

The fake worker's **misbehaviour catalogue**, each entry a T7 test:

| Misbehaviour | The controller must |
|---|---|
| Exit 0 | Record completion, reap without a zombie |
| Exit non-zero | Classify as worker error, not as a fault signal |
| `SIGSEGV`, `SIGBUS`, `SIGABRT` | Classify as a process fault, name the signal, preserve the timeline up to it; claim no cause (docs/verification.md §7a) |
| Silent hang — alive, no heartbeat | Detect by heartbeat staleness, not by EOF; a wedged process holds its descriptors open |
| Spin without progress | Distinguish "working slowly" from "not progressing" |
| Allocate until OOM-killed | Classify as OOM **distinctly** from a fault, by reading the kernel's own record |
| Close the record pipe early | Report truncated evidence as truncated, never as clean |
| Exit just before the deadline stop | Survive writing the stop byte to a pipe nobody reads: `EPIPE`, never death by `SIGPIPE` |
| Emit a truncated record | Reject it; a partial record is not a record |
| Emit a record with a bad checksum | Reject and report — a corrupt worker is what this tool exists to detect |
| Die holding a process-shared lock | Recover via `EOWNERDEAD`; survivors must not deadlock |
| Ignore `SIGTERM` | Escalate to `SIGKILL` within the bounded time |
| Fork a grandchild, then die | Leave no orphan holding a core at full load |

**The worker's exit codes, in one place.** Three headers use them and the controller's
classifier depends on all of them, so the band is reserved as a whole:

| Code | Set by | Meaning | Controller's fate |
|---|---|---|---|
| 0 | worker | Completed, or stopped on request | completed, if its finished record arrived |
| 119 | worker | Its own setup failed | worker error |
| 120–123 | launcher (`platform/process.hpp`) | Exec failed; orphaned before arming; death signal unavailable; descriptor not inherited | startup failed |
| 124 | worker | It judged the controller lost: hung up (its control pipe, or a record pipe with no reader left), stale or unreadable. A controller that was merely stalled, stopped or paged out for longer than the allowance produces this too | controller lost (reported) |
| 125 | worker (`worker/entry.hpp`) | The internal switch was used wrongly: a controller bug | startup failed |

Codes 0–2 and 126–127 are never reserved, and nothing above 255 is used: `exit(2)` keeps
eight bits.

D-state (uninterruptible sleep) is deliberately absent: it cannot be produced reliably from
userspace, and a test that cannot be made to fail is not a test. It is named here so its
absence is a recorded gap rather than an oversight.

**The rule that keeps the multi-process paths honest:**

> **No supervision test runs with a single worker.** Every T7 case runs with N ≥ 2, and the
> cases about lock recovery and orphan reaping run with N ≥ 3.

N=1 exercises none of the cross-process behaviour the design was chosen for, and a suite
that only ever runs N=1 passes right up until production. The default N in tests is 2; the
mixed workload at M5 raises it.

**Two further obligations specific to the split:**

- **The IPC boundary is fuzzed.** The controller must survive arbitrary bytes arriving on
  the record pipe, because a worker producing garbage is precisely the failure this tool
  exists to detect. Garbage in must never become a hardware verdict out.
- **Every process-shared lock has a robustness test.** Kill the holder mid-critical-section
  and assert a survivor recovers rather than blocking forever (`PLAN.md` §4.6).

**New gates:** two. A release-artifact check that no test-hook symbol is present in the
shipped binary, and the `EOWNERDEAD` obligation recorded per lock in the ledger.
`tools/check-test-obligations.py` and `tests/obligations.toml` already exist, so M1's
modules join a ledger rather than being retrofitted into one. Both are in:
`tools/check-no-test-hooks.sh` reads the shipped binary's symbols in the release and CI
workflows, and the ledger gate refuses an undeclared lock (§2.5; JOURNAL §2.21).

**Exit criteria** — all measurable, all in CI:

1. The controller runs a null workload for a configured duration and honours its limits.
2. Every transition in the state machine above has a passing T7 test, including the
   controller-killed case.
3. OOM-kill is classified **distinctly** from a fault — asserted, not merely logged.
4. Every P2 path in the milestone has an injected test; the obligation gate agrees.
5. Config round-trips through TOML with schema validation, and every rejection path is
   tested for the message it produces.
6. 100% line and branch, zero exclusions; obligation ledger current.

**Exit review (2026-10-20 04:44 UTC, `reviews/m1-exit-review-2026-10-20-0444-UTC.md`):**
criteria 1 to 6 are met, each with the tests that prove it named in the review. The review
closed four gaps on the way: the START to RUNNING transition had no topology snapshot (now
taken by `loadforge stress` before the run and refused on a `/sys` that will not describe the
machine); the OOM row had no end-to-end test (now a fake worker really OOM-killed under a
memory cgroup, reported `kOomKilled` beside a fault, run as root in CI); the "exit just
before the deadline stop" row had no T7 test; and a worker whose controller died before its
started record reported its own setup failure instead of the controller lost. **M1 is not
yet marked complete:** the obligation every milestone exits under includes the mutation
gate, and the pinned Mull has not yet swept `src/`. M1 is complete when its first run
(scheduled 2026-10-26 03:17 UTC, or an owner's dispatch before then) reports zero
unexplained survivors.

**Blocked on:** nothing. Q10 is closed — multiple worker processes, one executable
(`PLAN.md` §4.6) — which was the only decision M1 could not start without, because it
determines the IPC shape.

### M2a — First vertical slice, exact verification

**Builds:** `workloads/compression`, `verification/{oracle,checksum,isolation,golden}`,
input-integrity re-verification (F18), minimal `telemetry/capability`.

Compression is first because its contract is the simplest one that can be stated exactly:
`decompress(compress(x)) == x`, byte for byte. It proves the verification machinery before
any floating-point subtlety enters. What it *cannot* prove is that the input was right —
which is F18, and why input re-verification lands here rather than later.

**Paths introduced:**

| Class | Paths | Tests owed |
|---|---|---|
| P4 | pass; `ExactBitCorruption` with each bit-classification (single-bit, multi-bit in word, whole-cache-line); `VerificationNotPerformed` | T5 per classification; T5b for each way the check can fail to happen |
| P4 | Input corruption detected by regeneration, not by the round trip | A T5 test corrupting the *input* and asserting it is caught (F18) |
| P3 | Empty input, single byte, incompressible input, maximum block, block boundary ± 1 | Boundary tests |
| P5 | Verification concurrent with compute (duty cycle, F17) | Ordering test |

**Exit criteria:**

1. `loadforge stress --test compression` runs end to end.
2. Every P4 classification has a fault-injection test asserting **which** classification is
   produced, not merely that something was caught.
3. A corrupted *input* is caught and reported as such — distinct from a corrupted result.
4. `VerificationNotPerformed` is produced and reported for every way the check can be
   prevented, and is **not** routed into fault isolation.
5. The capability set is reported at run start.
6. **The mutation gate has run against real Mull** (carried from M0) and reports zero
   unexplained survivors over `verification/`.

### M2b — Second slice, bounded verification

**Builds:** `workloads/dense_linear`, `verification/{residual,invariant}`,
`platform/fpenv`, the first vectorized kernel with AVX2 and NEON behind one dispatch seam.

**Paths introduced:**

| Class | Paths | Tests owed |
|---|---|---|
| P4 | `NumericalVerificationFailure`; the bit fields reported **not-applicable**, never zero | T5 asserting the null fields are null |
| P3 | Subnormals, NaN, infinities, the tolerance boundary from both sides | Boundary tests at `ratio = 1 ± ε` |
| P1 | Each ISA dispatch arm: scalar, AVX2, NEON | The scalar path is always present and always tested |
| P2 | `FpEnvironment` assertion failing — FTZ/DAZ set by something else | Injected test forcing a dirty MXCSR/FPCR |

**Exit criteria:**

1. Both contracts demonstrated: exact from M2a, bounded here.
2. The declared determinism class is enforced on both architectures — `BitExactFixedDecomposition` within an ISA path, never across.
3. A derived tolerance, with the derivation written down; a test asserts the bound is
   breached at `1 + ε` and held at `1 − ε`.
4. `FpEnvironment` is asserted at startup and re-asserted after library load, with the
   failure path tested.
5. First meaningful thermal result recorded on real hardware (T11, manual).

### M3 — Full telemetry and Load Signature

**Builds:** all `telemetry/sources`, `sampler`, `timeline`, `statistics`,
`controller/results`, CSV and JSON reporters, per-provider remediation (F3), verification
duty cycle (F17), EDAC (F13).

This is the **P7-heavy** milestone and the one where the fixture library earns its keep.
The obligation is explicit and large:

> **Every telemetry source owes six fixtures**: present, absent, permission-denied,
> malformed, disappears mid-run, and counter-wraps. Not one representative hostile case
> per source — six per source.

**Exit criteria:**

1. A full Load Signature is produced from fixtures alone, with no real sensors.
2. Every source × state combination has a fixture and a test; the obligation gate agrees.
3. Every absent metric is reported as **absent with a remediation hint**, never as zero.
4. A wrapping counter is handled correctly, proved by a fixture that wraps mid-sample.
5. The result schema is versioned and the run fingerprint is reproducible across two runs
   on one machine.
6. The verification duty cycle is recorded, and expensive verification is demonstrably
   outside benchmark windows.
7. Mutation gating extended beyond the M0 module set to `verification/`, `config/`,
   `safety/` and `controller/supervision/`.

### M4 — Workload breadth

**Builds:** workloads 3–11, `primitives/*`.

Low-risk, broad, parallelizable — the only milestone where more than one person can work
without stepping on the architecture. Each workload owes the identical set: independent
oracle (T3), invariants (T4), declared determinism class enforced (T6), fault injection
per classification (T5), unrun-check (T5b).

**Exit criterion:** every workload's obligations discharged, with no workload carrying a
weaker determinism class than its algorithm actually supports.

### M5 — Dynamic mixed workload and cycling

**Builds:** `workloads/mixed`, thermal and power cycling, `controller/journal` (F16).

The crash journal lands here because it is required *before* serious multi-hour runs, not
after: a kernel panic during the most valuable run must not destroy the evidence
describing it.

**Paths introduced:** P6 at its most complex — workloads rotating across cores while
verification continues, and any worker able to die at any point in the rotation.

**Exit criteria:**

1. A five-hour schedule completes with continuous verification.
2. The journal survives a simulated hard kill and its content is replayable.
3. T10 soak: no memory growth, no counter overflow, over a multi-hour run.
4. Worker granularity checked against the mixed workload. Q10 closed at M1 in favour of
   multiple processes (PLAN §4.6); M5 confirms the choice holds when workloads mix, or
   records in an ADR why it no longer does.

### M6 — Explore

**Builds:** `explore/` — search strategy, objectives, drift correction, confidence
reporting (F5).

The research-flavoured milestone, correctly last. Its risk is not correctness but
**confident nonsense**: a search that reports a worst case without variance is worse than
no search.

**Exit criterion:** repeatable worst-case discovery on real hardware, with variance
reported and replicates recorded; an objective whose metric is unavailable fails loudly
rather than silently scoring zero.

### M7 — Hardening and 1.0

Docs, packaging, and the manual hardware validation checklist executed on both
architectures across multiple machines.

**Exit criterion:** multi-machine validation complete on x86-64 and ARM64; no known
correctness defects; both release artifacts validated on live media.

---

## 5. Sequencing

```text
M0 ✅ ──► M1 ──► M2a ──► M2b ──► M3 ──► M5 ──► M6 ──► M7
                   │        │       │
                   └────────┴───────┴──► M4 (parallelizable once M2b sets the pattern)

Critical path: M0 → M1 → M2a → M2b → M3.  M4 is breadth, not depth.
```

Two ordering constraints that are not obvious:

- **M4 cannot start before M2b**, even though it looks independent. M2b establishes the
  oracle-and-dispatch pattern every workload copies; starting breadth first produces eleven
  workloads that each invent their own.
- **M5 requires M3**, not just M2b. The mixed workload's value is the Load Signature it
  produces while rotating, and without full telemetry it is only a heater.

---

## 6. Gate evolution

What CI enforces at each milestone. A gate, once added, is never removed.

| Gate | M0 | M1 | M2a | M3 | M5 |
|---|---|---|---|---|---|
| 100% line and branch | ✅ | ✅ | ✅ | ✅ | ✅ |
| Coverage exclusions reviewed (`LF-COV-NNN`) | ✅ | ✅ | ✅ | ✅ | ✅ |
| Supply chain: manifest ↔ build reconciliation | ✅ | ✅ | ✅ | ✅ | ✅ |
| Vendored-tree digest (silent edits fail) | ✅ | ✅ | ✅ | ✅ | ✅ |
| External fetches centralized | ✅ | ✅ | ✅ | ✅ | ✅ |
| Cross-architecture determinism digest | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Test-obligation ledger** | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Mutation gate, demonstrated against real Mull** | | | ✅ | ✅ | ✅ |
| Fault-injection required on `verification/` | | | ✅ | ✅ | ✅ |
| Unrun-check (T5b) required on `verification/` | | | ✅ | ✅ | ✅ |
| Determinism class enforced per workload | | | ✅ | ✅ | ✅ |
| Supervision tier on every process transition | | ✅ | ✅ | ✅ | ✅ |
| No test-hook symbol in the shipped binary | | ✅ | ✅ | ✅ | ✅ |
| `EOWNERDEAD` robustness test per process-shared lock | | ✅ | ✅ | ✅ | ✅ |
| Hostile fixture per source × state | | | | ✅ | ✅ |
| Soak (T10), nightly | | | | | ✅ |

**Exclusion count target at every milestone: zero.** It is zero now, and every increase
should be treated as a design failure to be argued for, not a routine event.

**How the mutation gate runs.** Since 2026-10-20 11:45 UTC the sweep is one CI job per
module under `src/` (`tools/mutation-shard.py`), each mutating only its own sources and
judged on its own by `tools/mutation-gate.py --scope`: a mutant outside the shard is
refused, and only the shard's own allowlist entries are applied or can be stale. Every
mutant runs the in-process unit tests; the tests that create a process are named
`RealProcess*` and left out, and the workflow checks that name against what each test
really does. The first run as a single job reached the six-hour limit with no report
(JOURNAL §1.26, §2.28).

---

## 7. Cross-review against the design of record

This plan was cross-reviewed against `PLAN.md`, `verification.md`, `determinism.md` and
`platforms.md`, and against the repository as it actually stands. Findings, and what was
done about each:

| # | Finding | Resolution |
|---|---|---|
| **X1** | **`config/` at M1 needs a TOML parser, and none is vendored.** `PLAN.md` §5 chose a vendored header-only parser (`toml++`), but `third_party/` contained only `MANIFEST.toml` and nothing was declared. M1 would have begun with a supply-chain decision rather than with code. | **Closed.** toml++ v3.4.0 vendored at commit `30172438`, declared, recorded in `NOTICE`, exposed as a SYSTEM include so third-party code is not held to `-Werror` rules it never agreed to, and exercised by a smoke test on the surface `config/` will use. The static artifact still links and runs. |
| **X2** | **The vendored-and-linked path through `check-dependencies.py` had never been exercised.** Every component was `kind = "fetched"`, test-only, and the checker required `pinned_by` only for fetched — so a vendored component could be declared with no directory at all and pass. | **Closed, before X1 landed**, per the F20 rule that a gate untested on a path will fail open on it. A vendored component must now ship a directory and record `files_sha256`, a digest over its tree: a fetched dependency is pinned by the download it verifies, and a vendored one has no such moment, so the digest is its equivalent. Six tests cover the path, including an edit to vendored code after the fact. |
| **X3** | `PLAN.md` §6's directory tree lists `docs/architecture.md`, `telemetry-sources.md`, `result-schema.md`, `testing.md`, `safety.md` and `ISSUE_TEMPLATE/bug-report.yml`, none of which exist, without distinguishing planned from present. | `PLAN.md` §6 annotated: present entries versus planned, with the milestone that creates each. |
| **X4** | `PLAN.md` §6's workflow list is mis-nested and stale — `mutation.yml` is described as "nightly mull run, tracked mutation score", but it is `workflow_dispatch`-only and gates on *zero unexplained survivors*, not a score. | Corrected in `PLAN.md` §6. |
| **X5** | The tree omits `tests/tools/`, which exists and holds the trust-chain tests. | Added in `PLAN.md` §6. |
| **X6** | **The tier table has no tier for the gate tests themselves.** 56 tests run in CI against `tools/`, and the tier model does not mention them — so the document that defines "what is tested" omits the machinery enforcing it. | **T0b Gate tests** added to `PLAN.md` §7.1. |
| **X7** | M0's exit criterion in `PLAN.md` §8 predates the trust-chain work and does not mention the gates actually built. | `PLAN.md` §8 M0 row updated to what shipped. |
| **X8** | `PLAN.md` §8 gives no milestone a coverage exit criterion, though §7.3 makes 100% a standing gate. Implicit is not measurable. | The standing obligation is stated once in §4 of this document and referenced from `PLAN.md` §8. |
| **X9** | The Mull pin is listed in `PLAN.md` §11 as an open M0 item without a milestone deadline, so it could drift indefinitely while mutation testing is the declared primary metric. | Made an explicit **prerequisite for M2a** here and in `PLAN.md` §8. |
| **X10** | Open question 9 (milestone confirmation) is still formally open, which means this entire plan rests on an unconfirmed premise. | Raised in §9; the plan is written assuming yes, and says so. |

Nothing in the cross-review contradicted a normative contract in `determinism.md` or
`verification.md`. The findings are all of one kind — **documents that fell behind the
repository** — which is the failure mode to expect from here on, and the reason §6's gate
table and §3's ledger are written to be mechanically checkable rather than prose.

### 7.1 How X1 and X2 were closed

In this order, because the order was the point — the gate was extended and tested on the
vendored path *before* anything travelled down it:

1. `check-dependencies.py` gained vendored-component validation: the directory must exist,
   and `files_sha256` must match a recomputed digest over its tree. Six tooling tests,
   including one that edits the vendored file afterwards and asserts the gate notices.
2. toml++ v3.4.0 vendored — `toml.hpp` and `LICENSE` only, not upstream's tests, docs or
   build files — pinned to commit `30172438cee64926dc41fdd9c11fb3ba5b2ba9de`.
3. Declared in `third_party/MANIFEST.toml` as `vendored`, `linked = true`, MIT, with the
   digest; recorded in `NOTICE` with the GPL-compatibility reasoning.
4. Exposed through `cmake/Dependencies.cmake` as a **SYSTEM** include. The project compiles
   under `-Werror` with `-Wconversion`, `-Wold-style-cast` and more; third-party code has
   no obligation to satisfy a bar it never agreed to, and our own code stays held to it.
5. A smoke test compiles and exercises it on the surface `config/` will use — including
   that a malformed document *throws* rather than yielding an empty table, which is the F20
   property every parser here owes. A digest over a file that does not compile is a
   checksum of a brick.
6. The static artifact rebuilt, verified `statically linked`, and run.

---

## 8. Execution risks specific to this plan

These are risks to *building it*, distinct from the product risks in `PLAN.md` §9.

| Risk | Impact | Likelihood | Mitigation |
|---|---|---|---|
| The obligation ledger (§2.5) becomes a checkbox nobody reads | High — it is the mechanism the whole doctrine rests on | Medium | It is a gate, not a convention: unlisted module fails, missing fixture fails, stale entry fails |
| M1's platform seam is built without injection, making P2 untestable everywhere downstream | **Very high** — retrofits into ten modules | Medium | M1 exit criterion 4 requires an injected test per error mode *before* M1 closes |
| Mutation gate stays undemonstrated while becoming the primary metric | High | **Medium-high** — already carried once | Hard prerequisite for M2a (X9); M2a cannot close without it |
| P7 fixture work at M3 is larger than it looks — six states × every source | Medium | High | Budgeted explicitly in §4; `tools/capture-sysfs.sh` lands with M3 so users can contribute fixtures |
| M4 breadth invites weakened determinism classes to make tests pass | High | Medium | T6 enforces the declared class; the PR template requires a reviewed change to weaken one |
| Documents fall behind the repository again (every X-finding above) | Medium | **High** | §3 and §6 are tables a gate can check; the prose is deliberately thin |

---

## 9. Decisions this plan is waiting on

| # | Decision | Blocks | Recommendation |
|---|---|---|---|
| ~~Q10~~ | Worker process granularity | — | **Closed** (PLAN §4.6, revision 14): multiple worker processes, one executable |
| **Q9** | Milestone confirmation: framework-first with the M2a/M2b split | This whole plan | Proceed as written; the split exists so the first slice proves the machinery |
| **Q5** | Privilege policy — never escalate, ship a `setcap` helper, or opt-in privileged mode | M3 telemetry | Never escalate; report reduced capability with a remediation hint, document how to grant access |
| **Q11** | Is Ubuntu 24.04 the declared minimum for the dynamically-linked build | M7 packaging | Yes, with the static artifact covering everything older |
| — | Branch protection on `main` | Nothing technically; everything in terms of trust | Require the CI checks and a pull request |
| ~~—~~ | Mull release pinned by verified SHA-256 | — | **Done** 2026-10-20: 0.34.2, reported by the bootstrap job from GitHub's runners |

---

## 10. Status

**M0 complete. M1: every exit criterion met; complete once the first mutation sweep reports clean.
The contracts for M2a to M7 are written and the work after M1 is planned as streams in
waves (ADR 0004, `docs/ROADMAP.md`, `docs/streams/`).**

The working queue, item by item, is [`ROADMAP.md`](ROADMAP.md); this section is the
summary it is kept consistent with.

Revision 2 of this section said M1 was blocked only on Q10 and listed the order to build
in: decide Q10, then `platform/fs` and `platform/process` with injection from the first
commit, then `topology`, then the controller/worker split. That is the order it happened
in. Q10 closed (PLAN §4.6, revision 14), and `main` now holds, each at 100% line and
branch with zero exclusions and a ledger row the gate checks: `core`, `main`, the whole of
`platform` (seam and `fs`, exit status, launcher and pool, clock, affinity, memory),
`topology`, `config` with `loadforge --check-config FILE`, `worker/ipc` (heartbeat region,
record channel), `worker` (the `--worker` switch and the null workload loop),
`controller` with `reporters/console` and `loadforge stress` (#23), the misbehaviour
catalogue (#29: `tests/support/fake_worker`, a second implementation of the worker contract
that misbehaves on demand, every reachable catalogue row as a T7 test against it, and the
two gates M1 adds: no test-hook symbol in the shipped binary; the `EOWNERDEAD` obligation
per lock in the ledger), and `resources` (the memory budget and the OOM-kill record). The
ledger in §3 marks each ✅ with the corrections made against what was actually built.

**Against the M1 exit criteria in §4:** all six are met on the branch of the M1 exit review
(`reviews/m1-exit-review-2026-10-20-0444-UTC.md`), which names the tests behind each
criterion and each state-machine transition and audits every P2 path call site by call site.
The review closed four gaps: the topology snapshot before the first fork (the START to
RUNNING transition, which nothing implemented), the OOM row end to end (a fake worker really
OOM-killed under a memory cgroup the test makes, `kOomKilled` beside a `SIGSEGV` fault; CI
runs it as root, where a skip is a failure), the stop byte to a pipe nobody reads as a
deterministic T7 test, and a worker's `EPIPE` on its record pipe reported as the controller
lost (a flaky controller-killed test found it). Of the catalogue's rows, "die holding a
process-shared lock" has nothing to test, because there is no such lock (PLAN §4.6), and the
ledger gate checks that on every run.

**What remains of M1:** the mutation clause of the obligation every milestone exits under.
Mull is pinned and scheduled, but has not yet swept `src/`. Its first full run, as one job,
reached GitHub's six-hour limit and was cancelled with no report; the sweep now runs one job
per module against the tests that create no process (JOURNAL §1.26, §2.28), and five
survivors are expected in `src/platform/syscalls.cpp`, where only a forking test reaches the
decision on a returned pid. M1 is marked complete here and in §4 when a sweep reports zero
unexplained survivors; a survivor is a test to strengthen first. Recorded and not an exit criterion: a worker's grandchild outlives a controller that
is itself `SIGKILL`ed (JOURNAL §2.20); the real worker forks nothing.

Outside the repository, and still owner-only: **branch protection on `main`**. The Mull
pin, the other hard prerequisite for M2a, is in place (`mutation.yml`).
