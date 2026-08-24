# LoadForge — Development Plan

**Status:** draft for review
**Covers:** review of the project description, architecture, directory structure,
testing strategy, milestones, risks, open questions.

---

## 1. How this plan was produced

The input was the project description in
[`LoadForge-Integrated-Hardware-Reliability-Load-Test-Suite.md`](../LoadForge-Integrated-Hardware-Reliability-Load-Test-Suite.md)
(51 sections). It was read in full and treated as a **working draft** — a strong
statement of intent and scope, not a specification to be implemented literally.

Section 2 below records where the draft is solid, and where it makes assumptions
that will not survive contact with real Linux systems. Those findings are what shape
the architecture in section 3 onward. Nothing here is settled; section 10 lists the
questions that need the owner's decision before implementation starts.

---

## 2. Review of the project description

### 2.1 What is solid and should be preserved unchanged

These are the load-bearing ideas of the project and the plan adopts them as-is:

- **Whole workloads, not instruction loops.** The insistence that a reliability test
  be a complete computation (§2) is correct and is the project's main differentiator.
- **Two-layer separation** (§46): primitive kernels as an internal engine, integrated
  scenarios as the user-facing suite. This is a genuinely good architectural boundary
  and the source tree is organized around it.
- **Deliberate workload diversity** (§47): the argument that no single program
  maximizes every subsystem, and that ~12 workloads with different dominant
  characteristics cover the envelope better, is right.
- **Load Signature over a single percentage** (§29) — and keeping the multidimensional
  signature authoritative over any composite score (§30).
- **Self-verifying computation** (§19) as a defining feature rather than an add-on.
- **Common monotonic timeline for all telemetry** (§28). Correlating workload
  transitions against thermal and power response is what makes the data useful.
- **Minimizing persistent writes** (§41) — SSD wear is out of scope, so it should not
  be incurred accidentally.
- **Refusing to rely on undefined behaviour** (§40). Stressing valid hardware
  operation is the right line to draw.

### 2.2 Findings that require a decision or change

Ordered by how much they affect the architecture. Each names the draft section it
concerns.

---

#### F1 — Parallel floating-point results are not deterministic by default *(blocking)*

*Concerns §6, §10, §15, §19, §43.*

The draft asks for "deterministic final checksum" (§10), "deterministic checksums"
(§15) and verification by comparing computed results. But floating-point addition is
not associative. A parallel reduction whose order varies with thread count, work
stealing, or scheduling timing produces **different bits on every run on perfectly
healthy hardware**.

If this is not addressed first, the suite will emit spurious verification errors and
be worthless — a stress tester that cries wolf is worse than none.

**Implication:** determinism must be an explicit, enforced architectural property:

- Fixed-shape reduction trees; reduction order a function of problem decomposition
  only, never of thread count, timing, or completion order.
- Compensated (Kahan/Neumaier) summation where the accumulation depth warrants it.
- `-ffast-math` and friends **banned project-wide**; FP contraction pinned explicitly
  (`-ffp-contract=off` or a deliberate, documented `fast` with golden vectors
  regenerated per compiler).
- Verification split into two distinct kinds, never conflated:
  - **exact** — bit-identical checksums (integer, compression round-trip, sort);
  - **bounded** — residual or invariant within a tolerance *derived from the problem's
    condition number and operation count*, not a hand-picked epsilon.
- A determinism test in CI: same workload, N threads vs M threads, assert identical
  output. This is cheap and it is the test that keeps the property alive.

---

#### F2 — Distinguishing a hardware fault from a LoadForge bug *(blocking)*

*Concerns §19 — arguably the most important missing piece in the draft.*

The draft describes detecting incorrect results but never addresses the question a
user will ask the moment a failure appears: **is my CPU bad, or is your program bad?**
Without a credible answer, every reported error will be dismissed.

**Implication:** the following are required features, not nice-to-haves:

- **Golden vectors.** Every workload has known-good reference outputs for fixed small
  inputs, committed to the repository and checked at startup (`loadforge selftest`).
  A machine that fails self-test is either faulty or unsupported; either way the
  suite should say so before running for five hours.
- **Fault isolation on failure.** When a verification error occurs, automatically
  re-run the failing unit of work: (a) on the same core, (b) on a different core,
  (c) single-threaded. The pattern of reproduction is the diagnostic signal —
  core-local, thread-count-dependent, or fully reproducible are three very
  different diagnoses.
- **Error records carry full provenance:** logical CPU, physical core, NUMA node,
  buffer address and offset, expected vs actual bytes, workload seed, iteration index.
- **Bit-level classification.** Single-bit flip, multi-bit within one word, and whole
  cache-line corruption imply different failing hardware. Record the XOR delta and
  its population count.

---

#### F3 — Telemetry is privileged on modern kernels, and the draft assumes it is not *(blocking)*

*Concerns §24, §26, §27.*

The draft says LoadForge should read directly from Linux interfaces and degrade
gracefully. Degradation is mentioned for performance counters only, but the access
problem is much broader:

- **RAPL energy is root-only.** Since CVE-2020-8694 (PLATYPUS), `energy_uj` under
  `/sys/class/powercap/intel-rapl/` is `0400 root` on essentially every current
  distribution. Package power — one of the headline metrics — is unavailable to an
  unprivileged user.
- **`perf_event_paranoid` is typically 2 or higher**, blocking most PMU access
  without `CAP_PERFMON` or root. IPC, cache-miss and branch-miss rates all depend on it.
- **`hwmon` naming is not standardized.** `temp1_input` may be package temperature,
  a VRM, or a chipset sensor depending on board and driver. Sensor identification is
  a discovery problem, not a path lookup.
- **Memory bandwidth generally requires uncore IMC counters** (Intel
  `uncore_imc_*`), which are root-only and absent on many platforms; AMD is harder still.

*Verified in this development container:* `/sys/class/hwmon`, `/sys/devices/system/cpu/cpu0/cpufreq`
and `/sys/class/powercap/intel-rapl` are **all absent**, and `perf_event_paranoid = 2`.
So the environment where the code is written and where CI runs exposes *no* real
telemetry whatsoever.

**Implication — this is the single biggest driver of the architecture:**

- A explicit **capability model**. At startup LoadForge probes each telemetry source,
  records `available / permission-denied / unsupported`, and reports the resulting
  capability set to the user *before* a long run begins. "Package power unavailable —
  run as root or grant CAP_PERFMON to measure it" is far better than a silently empty column.
- Every metric in the Load Signature is **nullable**, and every consumer (scoring,
  ranking, explore-mode objectives) must handle absence. A ranking objective whose
  metric is unavailable must fail loudly at configuration time, not produce zeros.
- **All OS access goes behind narrow interfaces** so it can be driven from synthetic
  `/sys` and `/proc` fixture trees in tests. Without this the telemetry layer is
  simply untestable in CI. See §4.1 and §6.
- Default to running **unprivileged**, with documented, minimal privilege escalation
  for the metrics that need it.

---

#### F4 — Scope of "the initial version" is far too large *(high)*

*Concerns §5, §49.*

The draft places twelve complete workloads, full telemetry, load signatures, ranking,
worst-case search, and thermal cycling in the first version. Each workload is
individually a substantial piece of numerical engineering; the flagship dynamic mixed
test (§17) depends on all the others existing.

Building breadth first produces twelve half-working workloads and no working framework.

**Implication:** invert it. Build a **vertical slice** — one workload driven end to end
through config, topology, scheduling, verification, telemetry, safety, and reporting —
and only then add workloads against a proven framework. Milestones in §8 follow this.
Workloads are the *easy*, parallelizable part once the framework is real.

---

#### F5 — `explore` mode is a research problem, not a feature *(high)*

*Concerns §32, §33.*

Automatic worst-case discovery optimizes a black-box objective that is:

- **expensive** — thermal equilibrium takes 30–120 s per evaluation, so a run affords
  hundreds of samples, not millions;
- **noisy** — ambient temperature, other system activity, fan hysteresis;
- **non-stationary** — the machine heats up over the search, so an identical
  configuration evaluated at hour 3 scores differently than at minute 5;
- **high-dimensional and mixed** — continuous sizes, discrete algorithms, per-core assignments.

Naive random or grid search over this will produce confident nonsense.

**Implication:** treat as its own milestone (M5), after everything else, and design it honestly:

- Separate **fast objectives** (package power, IPC, bandwidth — seconds to measure)
  from **slow objectives** (equilibrium temperature — minutes). Use different budgets.
- **Interleave a reference configuration** periodically to measure and correct for drift.
- Mandatory **cool-down/settle phase** between evaluations, with an explicit convergence criterion.
- Start with successive halving over a curated candidate set; only consider model-based
  optimization once the measurement harness is proven repeatable.
- Report **confidence, replicate count and observed variance** alongside any "worst case" claim.

---

#### F6 — `dT/dt` is meaningless without a controlled starting state *(medium)*

*Concerns §22, §39.*

Temperature rise rate is a genuinely good metric, but comparing "Test A: +5.3 °C/s"
against "Test B: +1.8 °C/s" is only valid if both started from the same thermal state.
The five-hour schedule in §39 runs phases back to back with no cool-down, so every
phase after the first begins from the previous phase's equilibrium — and the measured
rise rates are not comparable.

**Implication:** rise rate is only recorded when preceded by a settle phase that has
reached a defined idle band. Otherwise the field is null. The default schedule must
include cool-downs, which lengthens it — a real trade-off the owner should make
knowingly.

---

#### F7 — Memory sizing needs the real Linux memory model *(medium)*

*Concerns §25, §37, §40.*

`memory = "70%"` is underspecified. Seventy percent of what? `MemTotal` invites the
OOM killer; the correct basis is `MemAvailable`. And in a container the true ceiling
is the cgroup v2 `memory.max`, not the host's memory at all.

**Implication:** allocation policy resolves against `MemAvailable` **and** cgroup
limits, whichever is lower, with a reserve headroom; pre-faults and locks pages where
permitted; verifies no swap is in use and aborts if swap-in begins mid-run (a swapping
"memory test" measures the disk, not the RAM). Huge-page strategy (THP vs explicit
`hugetlbfs`) must be an explicit, recorded configuration choice — it materially changes
TLB behaviour and therefore the result.

---

#### F8 — Reproducibility requires counter-based RNG *(medium)*

*Concerns §15, §43.*

The draft says to record RNG algorithm and seed. Necessary but not sufficient: with a
conventional sequential PRNG, the data any given thread generates depends on how work
was split, so changing thread count changes the data and the run is not reproducible
across machines with different core counts.

**Implication:** use a **counter-based RNG** (Philox/Threefry family). Value at index
*i* is a pure function of (seed, i), so any thread can generate any block
independently and the dataset is identical regardless of decomposition. This also
makes fault isolation (F2) possible — a failing block can be regenerated in isolation.

---

#### F9 — Safety controls need a watchdog, not polling *(medium)*

*Concerns §40.*

"Maximum allowed CPU temperature" and "emergency stop" are listed, but if the check
lives in the worker loop, a stalled or spinning worker is exactly the case where it
never runs — the case that matters most.

**Implication:** a dedicated **supervisor thread**, running at elevated priority,
independent of worker progress, owning temperature ceilings, duration limits, stall
detection (per-worker heartbeat counters) and emergency stop. Workers observe a single
atomic stop flag at bounded intervals. Shutdown must be prompt and must still flush
telemetry and results. Note honestly in the docs that software thermal protection is a
backstop *behind* the CPU's own throttling and thermal shutdown, never a substitute.

---

#### F10 — Smaller corrections *(low)*

- **§50 repository name.** The draft prefers `loadforge`; the actual repository is
  `LoadForge`. Harmless (GitHub URLs are case-insensitive) but the docs should be
  consistent. Question 8 in §10.
- **§44 "no large dependency graph"** vs. the need for a real test framework. Resolved
  by making test dependencies *test-only*, fetched at configure time, never linked into
  the shipped binary. The runtime dependency claim stays true.
- **§45 source structure** is a reasonable sketch but has no place for the platform
  abstraction (F3), safety (F9), or explore (F5), and no test tree at all. Superseded by §6.
- **§8 FFT** — "optionally three-dimensional" adds significant transpose complexity for
  little additional coverage over 2-D. Recommend deferring 3-D.
- **§13 compression** — "a simple internally implemented compression algorithm is
  sufficient" is right, and worth stating more strongly: an internal LZ77-class codec
  plus a strong hash gives an exact round-trip identity, which is the most valuable
  verification property in the whole suite. Keep it dependency-free.

---

## 3. Principles adopted

1. **Framework first, breadth second.** One workload end to end beats twelve stubs.
2. **Determinism is a hard requirement**, not an aspiration (F1).
3. **Every OS interaction is behind an interface** that a test can substitute (F3).
4. **Every metric is nullable**; absence is first-class and reported (F3).
5. **The suite must be able to accuse itself** — self-test and golden vectors run before
   any long run (F2).
6. **Safety is supervisory**, never dependent on worker liveness (F9).
7. **Test code is product code.** Held to the same review and style standards.

---

## 4. Architecture

### 4.1 Layering

```text
┌──────────────────────────────────────────────────────────────┐
│  CLI  ·  modes: selftest │ benchmark │ stress │ explore      │
├──────────────────────────────────────────────────────────────┤
│  Orchestration:  config · phase schedule · run control       │
├───────────────┬───────────────┬───────────────┬──────────────┤
│  Workloads    │  Verification │  Telemetry    │  Safety      │
│  (Layer 2)    │  residuals,   │  sampler,     │  supervisor, │
│  ┌──────────┐ │  checksums,   │  timeline,    │  watchdogs,  │
│  │Primitives│ │  golden,      │  capability   │  limits,     │
│  │(Layer 1) │ │  invariants   │  model        │  e-stop      │
│  └──────────┘ │               │               │              │
├───────────────┴───────────────┴───────────────┴──────────────┤
│  Scheduler:  worker pool · affinity · barriers · heartbeats  │
├──────────────────────────────────────────────────────────────┤
│  Topology:  logical CPUs · cores · SMT · caches · NUMA · P/E  │
├──────────────────────────────────────────────────────────────┤
│  ►► PLATFORM ◄◄   the ONLY code that touches the OS          │
│  sysfs · procfs · perf_event · affinity · mmap · clocks      │
└──────────────────────────────────────────────────────────────┘
```

The **platform layer is the critical boundary**. Every `open()`, every path under
`/sys` or `/proc`, every syscall lives there and nowhere else, behind interfaces.
Above that line the entire program is testable with no hardware, no privileges and
no real sensors — which, per F3, is the only environment CI will ever have.

### 4.2 Consequences for testability

| Layer | How it is tested | Real hardware needed? |
|---|---|---|
| Workloads, primitives | Tiny problem sizes, golden vectors, invariants, metamorphic relations | No |
| Verification | Fault injection — deliberate corruption must be caught | No |
| Telemetry, topology | Synthetic `/sys` + `/proc` fixture trees captured from real machines | No |
| Scheduler, safety | Fake clocks, controllable fake workers, injected stalls | No |
| Platform | Thin, kept as close to zero logic as possible; covered by a small hardware-tier suite | Yes (manual tier) |

---

## 5. Technology choices (proposed)

| Decision | Proposal | Why |
|---|---|---|
| Language | C++20 | As per draft §44. GCC 13 / Clang 18 both present and current. |
| Build | CMake ≥ 3.24 + Ninja, `CMakePresets.json` | Ubiquitous; presets keep local and CI builds identical. |
| Test framework | **GoogleTest + GMock** | GMock matters: the platform layer (F3) needs substitutable fakes. Test-only, `FetchContent`. |
| Coverage | **gcovr** (GCC `--coverage`), `llvm-cov` for Clang builds | Simple, good CI/XML/HTML output. *Not installed in this container — must be added.* |
| Sanitizers | ASan+UBSan, **TSan**, in separate CI jobs | TSan is essential; this is a heavily threaded project by construction. |
| Static analysis | `clang-tidy`, warnings-as-errors, `clang-format` | All present in the container. |
| Config format | TOML | As per draft §37. Needs a small parser — vendored single-header or hand-rolled; runtime deps stay at zero. |
| Runtime dependencies | **None** beyond libc/libstdc++ | Per draft §44. Enforced by a CI check on the linked binary. |

---

## 6. Directory structure (proposed)

```text
LoadForge/
├── README.md
├── LICENSE                         # to be chosen — see Q2
├── CMakeLists.txt
├── CMakePresets.json               # debug, release, asan, tsan, coverage
├── .clang-format
├── .clang-tidy
├── .gitignore
│
├── .github/workflows/
│   ├── ci.yml                      # build + unit + integration, gcc & clang
│   ├── sanitizers.yml              # asan+ubsan, tsan
│   └── coverage.yml                # coverage gate, ratchet-only
│
├── cmake/
│   ├── CompilerWarnings.cmake      # warnings-as-errors; bans -ffast-math (F1)
│   ├── Sanitizers.cmake
│   ├── Coverage.cmake
│   └── Dependencies.cmake          # FetchContent, test-only
│
├── docs/
│   ├── PLAN.md                     # this document
│   ├── architecture.md
│   ├── determinism.md              # the F1 contract — required reading for contributors
│   ├── telemetry-sources.md        # every /sys path, its privilege, its fallback
│   ├── testing.md
│   ├── safety.md
│   └── adr/                        # architecture decision records
│       └── 0001-record-architecture-decisions.md
│
├── include/loadforge/              # public headers (only if a library is exposed)
│
├── src/
│   ├── main/                       # CLI parsing, mode dispatch
│   ├── core/                       # Result/Error, units, duration, span, logging
│   │
│   ├── platform/                   # ►► ONLY code that touches the OS ◄◄
│   │   ├── fs/                     #    abstract file/dir reader (fixture-swappable)
│   │   ├── sysfs/
│   │   ├── procfs/
│   │   ├── perf_events/
│   │   ├── affinity/
│   │   ├── memory/                 #    mmap, huge pages, NUMA, locking
│   │   └── clock/                  #    monotonic time, abstract for tests
│   │
│   ├── topology/                   # logical CPUs, cores, SMT, cache domains, NUMA, P/E
│   ├── scheduler/                  # worker pool, phases, barriers, heartbeats
│   ├── config/                     # TOML parse, schema validation, defaults, resolution
│   ├── safety/                     # supervisor thread, limits, watchdogs, e-stop (F9)
│   │
│   ├── telemetry/
│   │   ├── capability/             # probe + report what is actually available (F3)
│   │   ├── sources/                # cpu, thermal, fan, power, memory, counters
│   │   ├── sampler/
│   │   └── timeline/               # common monotonic timeline (§28)
│   │
│   ├── verification/
│   │   ├── checksum/               # CRC, exact integer checksums
│   │   ├── residual/               # bounded numerical checks, tolerance derivation
│   │   ├── invariant/              # momentum, energy, permutation, ordering
│   │   ├── golden/                 # known-good reference vectors (F2)
│   │   └── isolation/              # failure re-run and classification (F2)
│   │
│   ├── statistics/                 # load signature, aggregation, scoring, ranking
│   ├── explore/                    # search strategy, objectives, drift correction (F5)
│   │
│   ├── primitives/                 # ── Layer 1 ──
│   │   ├── integer/  floating/  simd/
│   │   └── cache/  memory/  branch/  atomic/
│   │
│   ├── workloads/                  # ── Layer 2 ──
│   │   ├── dense_linear/  sparse/  fft/  stencil/
│   │   ├── particles/  raytrace/  sort/  compression/
│   │   ├── graph/  monte_carlo/  adaptive_mesh/
│   │   └── mixed/                  # flagship dynamic mixed test (§17)
│   │
│   └── reporters/
│       ├── console/  csv/  json/
│
├── tests/
│   ├── unit/                       # mirrors src/ one-to-one
│   ├── property/                   # metamorphic + invariant relations
│   ├── fault_injection/            # corruption MUST be detected (F2)
│   ├── determinism/                # thread-count and repeat-run invariance (F1)
│   ├── integration/                # full CLI runs, tiny sizes, temp dirs
│   ├── soak/                       # hours-long, opt-in, nightly
│   ├── hardware/                   # needs real sensors; skipped in CI
│   ├── support/                    # fakes, matchers, fixture loaders
│   └── fixtures/
│       ├── sysfs/                  # captured /sys trees: intel, amd, arm64, degraded, denied
│       ├── procfs/
│       └── golden/                 # per-workload reference outputs
│
├── benchmarks/                     # micro-benchmarks for the primitives
│
├── tools/
│   ├── coverage.sh
│   ├── capture-sysfs.sh            # snapshot a real machine into a test fixture
│   └── gen-golden/                 # regenerate golden vectors deliberately
│
└── config/                         # example profiles
    ├── quick.toml
    ├── stress-5h.toml
    └── explore-temperature.toml
```

Two things to note. First, `tests/` is not an afterthought directory — it has as much
internal structure as `src/`, with whole categories (`fault_injection/`,
`determinism/`) that exist because of specific findings above. Second,
`tests/fixtures/sysfs/` is what makes the telemetry layer developable at all in an
environment with no sensors, which is the environment we actually have.

---

## 7. Testing strategy

The requirement is high coverage and comprehensiveness. Coverage percentage alone is a
weak signal — it is possible to execute every line and assert nothing. The strategy
therefore pairs a coverage gate with categories of test that specifically attack the
ways *this particular* program can be wrong.

### 7.1 Test tiers

| Tier | Scope | Runs | Budget |
|---|---|---|---|
| **T0** Static | format, clang-tidy, warnings-as-errors, no-runtime-deps check | every push | < 1 min |
| **T1** Unit | pure logic, parsers, math at tiny sizes; hermetic | every push | < 60 s total |
| **T2** Fixture | platform + telemetry against synthetic `/sys`, `/proc` | every push | < 30 s |
| **T3** Property | metamorphic and invariant relations | every push | < 2 min |
| **T4** Fault injection | corruption is detected, and error is classified correctly | every push | < 1 min |
| **T5** Determinism | thread-count invariance, repeat-run invariance | every push | < 2 min |
| **T6** Integration | real CLI, real files, tiny durations | every push | < 3 min |
| **T7** Sanitizers | ASan+UBSan; TSan | every push (parallel jobs) | < 15 min |
| **T8** Soak | multi-hour runs, memory growth, counter overflow | nightly | hours |
| **T9** Hardware | real telemetry, real thermal response | manual / self-hosted | manual |

### 7.2 The categories that matter most

**Fault injection (T4).** For every verification mechanism, a test that deliberately
corrupts a result and asserts the corruption is caught: flip one bit in a matrix buffer
and assert the residual check fires; corrupt one byte of a compressed block and assert
the round-trip fails; drop one atomic increment and assert the graph consistency check
notices. This is the only evidence that the tool would catch real silent corruption.
**A verification path without a negative test is considered untested regardless of
line coverage.**

**Determinism (T5).** For each workload: run at 1, 2, 3, 4, 7 threads and assert
identical checksums; run twice at the same thread count and assert identical results;
assert that changing block/tile size does not change results beyond the declared
tolerance. This is what protects F1 from eroding as the code grows.

**Metamorphic and invariant testing (T3).** These give strong assertions without needing
a known-correct answer:

| Workload | Relation asserted |
|---|---|
| FFT | `IFFT(FFT(x)) ≈ x`; Parseval's theorem; linearity |
| Dense linear | `‖Ax − b‖` bounded by condition number × ε × n |
| Sort | output is a permutation of input **and** is ordered |
| Compression | `decompress(compress(x)) == x` exactly, for all block classes |
| N-body | total momentum conserved; energy within tolerance; time-reversal |
| Graph | BFS distances satisfy the triangle inequality; components partition vertices |
| Stencil | symmetric initial field stays symmetric; conserved quantity holds |
| Monte-Carlo | fixed seed reproduces exactly; statistical invariants within bounds |

**Fixture-driven platform tests (T2).** Captured `/sys` trees from real machines,
committed as fixtures, including deliberately hostile cases: an AMD machine, an ARM64
machine, a machine with no `hwmon` at all, one where `energy_uj` returns `EACCES`, one
with a counter that wraps mid-sample, one with a sensor that disappears during the run.
The capability model (F3) is tested by asserting the correct degraded behaviour for each.

### 7.3 Coverage

Measured with `gcovr` on a dedicated CMake preset; reported per-directory, not just as
one number.

| Area | Line | Branch | Notes |
|---|---:|---:|---|
| `core/`, `config/`, `verification/`, `statistics/` | ≥ 95% | ≥ 90% | Pure logic; no excuse for gaps |
| `telemetry/`, `topology/`, `scheduler/`, `safety/` | ≥ 90% | ≥ 85% | Fixtures and fakes make this reachable |
| `workloads/`, `primitives/` | ≥ 90% | ≥ 80% | Tiny sizes; SIMD paths may need per-ISA runners |
| `platform/` | ≥ 80% | — | Deliberately thin; remainder covered by T9 |
| `main/`, `reporters/` | ≥ 90% | ≥ 85% | Integration tier reaches most of it |
| **Overall gate** | **≥ 90%** | **≥ 85%** | CI fails below; ratchet-only — never allowed to drop |

Supporting rules:

- Uncovered lines require an explicit, reviewed exclusion comment with a reason.
- **Mutation testing** (`mull`) as a stretch goal on `verification/` and `config/` —
  the two places where a passing-but-assertion-free test would be most dangerous.
- Coverage is reported per pull request as a delta, so regressions are visible at review time.

### 7.4 What CI cannot test — stated plainly

CI runners are virtual machines. Verified in this container: no `hwmon`, no `cpufreq`,
no RAPL, `perf_event_paranoid=2`. Therefore CI can **never** validate: real sensor
readings, actual thermal response, genuine throttling behaviour, real power measurement,
or that a workload in fact heats a physical CPU.

Those belong to tier T9 and require either a self-hosted runner with real hardware or a
documented manual validation checklist run before each release. This limitation is
recorded here so it is never mistaken for coverage.

---

## 8. Milestones

Each milestone has an exit criterion. Nothing proceeds past a milestone whose criterion
is unmet.

| # | Milestone | Contents | Exit criterion |
|---|---|---|---|
| **M0** | Foundation | Repo scaffolding, CMake + presets, CI, coverage gate, clang-tidy/format, ADR process, one trivial module proving the whole pipeline | Green CI on GCC and Clang; coverage gate active and enforced |
| **M1** | Core framework | `core`, `platform`, `topology`, `config`, `scheduler`, `safety`, console reporter | Framework runs a null workload for a configured duration, honours limits, stops cleanly on e-stop |
| **M2** | First vertical slice | Dense linear algebra + verification (`residual`, `golden`, `isolation`) end to end | `loadforge stress --test dense-linear` runs, verifies, reports; determinism and fault-injection suites pass |
| **M3** | Telemetry + Load Signature | Capability model, sysfs/procfs sources, sampler, timeline, CSV/JSON reporters, statistics | Full Load Signature emitted from fixtures; correct graceful degradation on all hostile fixtures |
| **M4** | Workload breadth | Remaining single workloads (2–11) + primitives layer | Each has golden vectors, invariants, determinism and fault-injection tests; coverage gate held |
| **M5** | Dynamic mixed + cycling | Flagship mixed test (§17), thermal/power cycling (§35) | A five-hour configured schedule completes with continuous verification |
| **M6** | Explore | Search strategy, objectives, drift correction, confidence reporting (F5) | Repeatable worst-case discovery with reported variance on real hardware |
| **M7** | Hardening / 1.0 | Docs, packaging, manual hardware validation, license | Multi-machine validation; no known correctness defects |

M0–M3 are the critical path — that is where the project is either sound or not.
M4 is broad but low-risk and parallelizable. M6 is the research-flavoured one and is
correctly last.

---

## 9. Risk register

| Risk | Impact | Likelihood | Mitigation |
|---|---|---|---|
| Spurious verification errors from FP non-determinism | **Fatal to credibility** | High if unaddressed | F1: determinism contract + T5 tier from M0 |
| Users cannot distinguish tool bugs from hardware faults | **Fatal to credibility** | High | F2: self-test, golden vectors, fault isolation |
| Telemetry unavailable or wrong on the user's machine | High | High | F3: capability model, nullable metrics, hostile fixtures |
| Scope collapse under 12-workload breadth | High | High | F4: vertical slice first; M-gates |
| Explore mode produces confident nonsense | Medium | High | F5: deferred to M6, variance reporting, drift correction |
| Silent regression via untested SIMD paths | Medium | Medium | Per-ISA test runners; golden vectors per ISA |
| Damage to poorly-cooled user hardware | Medium | Low | Conservative defaults, supervisor, clear warnings |
| Coverage gate gamed by assertion-free tests | Medium | Medium | Mutation testing on critical modules; review standards |

---

## 10. Open questions — owner decision needed

1. **Document placement.** Move `LoadForge-Integrated-Hardware-Reliability-Load-Test-Suite.md`
   into `docs/` (e.g. `docs/DESIGN-DRAFT.md`)? It is currently in the repository root.
   *Recommendation: yes — root should hold README and build files.*
2. **License.** Must be settled before public release. *Recommendation: Apache-2.0
   (patent grant) or MIT (maximum simplicity).*
3. **Minimum toolchain.** GCC 13 / Clang 18 as verified here, or support older
   distribution compilers (GCC 11 on Ubuntu 22.04)? This constrains which C++20 library
   features are usable.
4. **Architectures.** x86-64 only for v1, or ARM64 from the start? Affects SIMD strategy
   and roughly doubles telemetry work — different sensor and counter interfaces entirely.
5. **Privilege policy.** Should LoadForge ever request root, ship a setcap helper, or
   simply document reduced capability when unprivileged?
   *Recommendation: never escalate; report clearly and document how the user can grant access.*
6. **Test framework.** Confirm GoogleTest + GMock, or prefer Catch2/doctest for a
   lighter footprint (at the cost of built-in mocking).
7. **Default schedule.** Adding cool-downs (F6) makes `dT/dt` meaningful but lengthens
   the five-hour schedule. Accept a longer run, or drop rise-rate comparability?
8. **Repository name casing.** Draft §50 prefers `loadforge`; the repository is
   `LoadForge`. Align the docs to which?
9. **Milestone confirmation.** Is the framework-first ordering (F4) agreed, or is there a
   reason to prioritise workload breadth earlier?

---

## 11. Next step

Nothing is implemented yet, by design — this plan is for review first. Once the
questions in §10 are answered, work begins at **M0**: repository scaffolding, build
system, CI, and the coverage gate, so that every subsequent line of code lands into a
harness that is already enforcing the standards described above.
