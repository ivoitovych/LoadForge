# LoadForge — Implementation Plan

**Status:** revision 1 — written at M0 complete, `main` at the trust-chain hardening pass
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

The owner's requirement is that every execution path carries comprehensive test coverage,
so that production holds no surprises. That requirement is only useful if it is
**decidable**: someone must be able to look at a module and say whether the obligation is
discharged. This section makes it decidable.

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
skipped under deadline. The mechanism, landing at M1:

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
from the ledger, when a declared tier has no corresponding test target, when a declared
fixture does not exist, or when a ledger entry names a module that no longer exists. It is
built to the F20 standard: it fails closed on a ledger it cannot parse, and an empty
ledger with a populated `src/` is a failure, not a pass.

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

**T0 (static), T0b (gate tests) and T9 (sanitizers) are omitted from every row because
they are universal** — T0 and T9 run over the whole tree on every push, and T0b covers
`tools/` rather than `src/`. The "tiers owed" column lists only what a module owes *beyond*
those, so a blank there means a real exemption and never an oversight.

| Module | M | Path classes | Tiers owed | Notes |
|---|---|---|---|---|
| `core/` | M0 ✅ | P1, P3 | T1 | Result, units, duration, byte size — done, 100% |
| `main/` | M0 ✅ | P1, P3 | T1, T8 | CLI dispatch, selftest digest |
| `config/` | M1 | P1, P2, P3 | T1, T8 | TOML parse, schema validation, resolution. **Needs the vendored parser — see §7 finding X1** |
| `platform/fs` | M1 | P1, P2, P7 | T1, T2 | The fixture-swappable reader every other platform module goes through |
| `platform/clock` | M1 | P1, P3 | T1 | Monotonic time; fake clock for the rest of the suite |
| `platform/process` | M1 | P1, P2, P6 | T1, T7 | fork/exec, signals, reaping, `PR_SET_PDEATHSIG` (F9) |
| `platform/affinity` | M1 | P1, P2, P7 | T1, T2 | |
| `platform/memory` | M1 | P1, P2, P3 | T1, T2 | mmap, huge pages, NUMA, mlock — each failure mode injected |
| `platform/fpenv` | M2b | P1, P2, P3 | T1, T6 | MXCSR/FPCR assert and re-assert (F15) |
| `platform/sysfs` | M3 | P1, P2, P7 | T1, T2 | |
| `platform/procfs` | M3 | P1, P2, P7 | T1, T2 | |
| `platform/perf_events` | M3 | P1, P2, P7 | T1, T2 | `perf_event_paranoid` is a first-class P7 state |
| `platform/edac` | M3 | P1, P2, P7 | T1, T2 | F13 |
| `topology/` | M1 | P1, P2, P3, P7 | T1, T2 | Shared immutable snapshot taken before fork |
| `worker/scheduler` | M1 | P1, P5, P6 | T1, T7 | Pool, affinity, barriers, heartbeat emission |
| `worker/ipc` | M1 | P1, P2, P5 | T1, T5b, T7 | Shared-memory status, record channel |
| `controller/orchestration` | M1 | P1, P3, P6 | T1, T8 | Phase schedule, run control |
| `controller/safety` | M1 | P1, P3, P6 | T1, T7, T8 | Limits, watchdog, e-stop, child kill |
| `controller/supervision` | M1 | P1, P2, P6 | T1, T7 | Heartbeats, crash classification, OOM versus fault |
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

**Two M0 items remain open and are carried, not closed:**

1. **The mutation gate is specified, not demonstrated** — Mull is unpinned and its
   end-to-end test has never run (F21). This is a **prerequisite for M2a**, not for M1:
   from M2a the mutation gate becomes the primary quality signal on verification code, and
   an unexercised gate cannot hold that role.
2. **`main` is unprotected** — no required status checks; every gate above is bypassable
   by a direct push. Requires owner action in GitHub settings.

### M1 — Core framework and supervision

**Builds:** `config`, `platform/{fs,clock,process,affinity,memory}`, `topology`,
`worker/{scheduler,ipc}`, `controller/{orchestration,safety,supervision}`,
`reporters/console`.

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

**New gates:** `tools/check-test-obligations.py` and `tests/obligations.toml` (§2.5).

**Exit criteria** — all measurable, all in CI:

1. The controller runs a null workload for a configured duration and honours its limits.
2. Every transition in the state machine above has a passing T7 test, including the
   controller-killed case.
3. OOM-kill is classified **distinctly** from a fault — asserted, not merely logged.
4. Every P2 path in the milestone has an injected test; the obligation gate agrees.
5. Config round-trips through TOML with schema validation, and every rejection path is
   tested for the message it produces.
6. 100% line and branch, zero exclusions; obligation ledger current.

**Blocked on:** open question 10 (worker process granularity). *Recommendation stands:
single worker process at M1, revisit at M5.* This is the only decision M1 cannot start
without, because it determines the IPC shape.

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
4. Q10 revisited — worker granularity re-decided with the mixed workload in hand.

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
| External fetches centralized | ✅ | ✅ | ✅ | ✅ | ✅ |
| Cross-architecture determinism digest | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Test-obligation ledger** | | ✅ | ✅ | ✅ | ✅ |
| **Mutation gate, demonstrated against real Mull** | | | ✅ | ✅ | ✅ |
| Fault-injection required on `verification/` | | | ✅ | ✅ | ✅ |
| Unrun-check (T5b) required on `verification/` | | | ✅ | ✅ | ✅ |
| Determinism class enforced per workload | | | ✅ | ✅ | ✅ |
| Supervision tier on every process transition | | ✅ | ✅ | ✅ | ✅ |
| Hostile fixture per source × state | | | | ✅ | ✅ |
| Soak (T10), nightly | | | | | ✅ |

**Exclusion count target at every milestone: zero.** It is zero now, and every increase
should be treated as a design failure to be argued for, not a routine event.

---

## 7. Cross-review against the design of record

This plan was cross-reviewed against `PLAN.md`, `verification.md`, `determinism.md` and
`platforms.md`, and against the repository as it actually stands. Findings, and what was
done about each:

| # | Finding | Resolution |
|---|---|---|
| **X1** | **`config/` at M1 needs a TOML parser, and none is vendored.** `PLAN.md` §5 chose a vendored header-only parser (`toml++`), but `third_party/` contains only `MANIFEST.toml` and nothing is declared. M1 therefore begins with a supply-chain decision, not with code. | Recorded as an M1 prerequisite here; §7.1 below states what landing it requires. |
| **X2** | **The vendored-and-linked path through `check-dependencies.py` has never been exercised.** Every current component is `kind = "fetched"`, test-only. `toml++` will be the first `vendored`, `linked = true` component, and the checker requires `pinned_by` only for fetched. | Flagged; the checker needs a vendored-path test before X1 lands, per the F20 rule that a gate untested on a path is a gate that will fail open on it. |
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

### 7.1 What X1 requires before M1 code starts

1. Choose the parser and pin it by commit SHA, not tag.
2. Declare it in `third_party/MANIFEST.toml` as `kind = "vendored"`, `linked = true`, with
   its licence checked against the GPL-compatible allowlist.
3. Add the vendored-path test to `tests/tools/test_tooling.sh` (X2) — a vendored component
   with a missing directory, a wrong licence, and an undeclared directory present.
4. Record it in `NOTICE`.
5. Confirm the static release artifact still links and runs, since this is the first
   component actually linked into the shipped binary.

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
| **Q10** | Worker process granularity — one process, or one per workload group | **M1 — cannot start** | Single worker process at M1; revisit at M5 with the mixed workload in hand |
| **Q9** | Milestone confirmation: framework-first with the M2a/M2b split | This whole plan | Proceed as written; the split exists so the first slice proves the machinery |
| **Q5** | Privilege policy — never escalate, ship a `setcap` helper, or opt-in privileged mode | M3 telemetry | Never escalate; report reduced capability with a remediation hint, document how to grant access |
| **Q11** | Is Ubuntu 24.04 the declared minimum for the dynamically-linked build | M7 packaging | Yes, with the static artifact covering everything older |
| — | Branch protection on `main` | Nothing technically; everything in terms of trust | Require the CI checks and a pull request |
| — | Mull release pinned by verified SHA-256 | **M2a** | Owner or a network-capable environment must supply it |

---

## 10. Status

**M0 complete. M1 blocked on Q10 and, before code, on the X1 parser decision.**

The next concrete actions, in order:

1. Decide Q10 (one line from the owner).
2. Land the TOML parser as a vendored, linked, declared component — with the
   vendored-path test in the dependency checker first (X1, X2).
3. Build `platform/fs` and `platform/process` with injection from the first commit, since
   every downstream P2 path depends on that seam.
4. Land `tests/obligations.toml` and its gate alongside the first M1 module, so the ledger
   is never retrofitted.
