# Journal fragment 21 — the M3 integration

**Written:** 2026-10-20 23:41 UTC
**Change:** `src/main/` (the telemetry rig, the result's assembly, the files written),
`src/controller/` top level (sampling in the loop), `tests/integration/m3/`,
`tests/supervision/telemetry_death_test.cpp`, the unit tests of both modules, their ledger
fragments, `.github/workflows/mutation.yml`.

The point of this stream was a test of the component model: do telemetry sources, the
sampler and timeline, statistics, results and the reporters, each built against a contract
and none reading another's tests, fit together? **Seven places were found where two
components read a contract differently** (section "Contract disagreements found"), and
four more where the result cannot say what a reader needs to be told. None was found by a
component's own suite or by cross-execution of the mocks (all six suites pass against this
stream's mocks); all were found by running the products together.

## What was built

- **Sampling in the controller's loop** (`RunOptions::sampler`, `timeline`,
  `sample_interval_ns`): a round before the first fork, every interval, at the deadline on
  the boundary of the phase it ends, and after the last reap. The cost of the rounds is
  measured, every reap is timed on the timeline's clock, and a round the timeline refuses ends
  sampling and is reported, never the run. `RunReport` carries a `SamplingReport`.
- **The telemetry rig** (`main/telemetry_rig`): the seven sources in one order over one
  machine's roots, a bounded timeline, the sampler, the capability report.
- **The result** (`main/run_result`): the controller's report, the fingerprint, the capability
  report and the statistics over the run's window, sealed with its verdict; per-phase counter
  deltas (F13); the console summary (verdict, observers, what each dead worker left behind,
  EDAC per phase, the fingerprint fields the machine would not give).
- **`loadforge stress --output json,csv [--output-dir DIR] [--sample-interval D]`**
  (`main/result_files`): `DIR/run.json` and `DIR/run.csv`, written whole or not at all, the
  directory checked before the run starts. The capability report, with each provider's
  remedy, is printed before the first worker exists.
- **The integration tier** (`tests/integration/m3`): the stack as built, over fixtures (all 26
  metrics; the six states of every source followed through sampler, statistics, result and
  both writers), over this machine's real `/proc` and `/sys`, and through the shipped binary
  run twice; and `tests/supervision/telemetry_death_test.cpp`, a real worker killed mid-run.
- **Mutation** (`.github/workflows/mutation.yml`): one unit per test program (below).

## Established by experiment

### The real kernel, and what it costs to watch it

`loadforge stress --config tests/fixtures/config/short-stress.toml --output json,csv
--sample-interval 100ms` on the development machine (Firecracker virtual machine, Linux
6.18.44-fc-v139, x86-64, 4 CPUs): 10 of 23 metrics present (memory 7, the two CPU tick
counters, `counters.context_switches`); no sensor, no fan, no cpufreq, no RAPL, no EDAC, no
PMU (`perf_event_open` ENOENT for the four hardware events, `perf_event_paranoid` 2). Every
absent metric prints with its provider's remedy. 11 to 12 rounds in a 1.0 s run, 110 to 120
points over 10 series, sampling took 3 ms: **0.28 % of one CPU at 10 Hz**. Eight
fingerprint fields are absent on this machine and are named on the console with the
kernel's own reason (the result carries the names but not the reasons; see Contract
questions).

### A software perf event opened with `exclude_kernel` reads zero for ever

Found by the real-kernel run: `counters.context_switches` was present, valid, and 0 over a
run in which the process slept. A standalone program, `perf_event_open(PERF_TYPE_SOFTWARE,
PERF_COUNT_SW_CONTEXT_SWITCHES, pid = getpid(), inherit = 1)` read after each of five 20 ms
`usleep`s, on this kernel:

```text
exclude_kernel = 1   value 0 0 0 0 0
exclude_kernel = 0   value 1 2 3 4 5
```

A context switch is counted in the kernel. The counters source opens every event, software
included, with `user_only = true`. Disagreement 4 below.

### What a point costs

`sizeof(Point)` is 120 bytes on x86-64 (`sizeof(Reading)` 112, `sizeof(Unavailable)` 104),
because a gauge's point carries room for an `Unavailable`'s three strings. The timeline's
resident bound is 2 × capacity × 120. At the default interval of 1 s, five hours over the
rig's bound for a 4-CPU machine (96 series) is 1.7 million points, 207 MB resident at most
414 MB. At 10 Hz it is ten times that. A machine with hundreds of CPUs has hundreds of
`frequency.cpu` series on its own. This is the frozen header's cost, recorded for M5's soak.

### UBSan found a signed overflow in this stream's own sizing code

`timeline_capacity` added a margin to `run_ns / interval` in signed 64-bit arithmetic; a run
of the longest representable length overflowed (`9223372036854775807 + 8`). The test of
saturation I had written to *show* the function saturates is what the sanitizer ran. Fixed in
unsigned arithmetic; the same care went into `--sample-interval`, which `parse_duration`
accepts up to the largest millisecond count and which would overflow once turned into
nanoseconds (refused above about 73 years). The controller's own
`duration * 1'000'000` in `orchestration.cpp` (M1 code) has the same shape for a configured
duration of centuries; not changed here, recorded.

### TSan is installable here

Stream 13 reported the Clang 18 ThreadSanitizer runtime could not be run in this container.
`clang++-18 -fsanitize=thread` links and runs here, and the unit, supervision and
integration binaries of this stream were run under it, clean.

### The sources do not agree on what their root is

Thermal, fan, frequency and memory take a prefix before `/sys/...` and `/proc/...`; power
and EDAC take the sysfs directory itself (default `/sys`); counters take the procfs
directory (default `/proc`) and append `/sys/kernel/perf_event_paranoid` to it, which is
`/proc/sys/kernel/perf_event_paranoid`. Nothing in `source.hpp` says how a source is
constructed, so this is not a disagreement with a contract; it is why `MachineRoots` has
three fields and why one function (`make_sources`) is the only place that spells a path.

### Every shard of the weekly mutation sweep ran the same test program

`mutation.yml` ran `tools/mutation-gate.py` for each of the 15 module shards against
`loadforge_unit_tests`. Modules with a test program of their own (`verification/*`,
`telemetry/*`, `statistics`, `reporters/*`, `controller/journal`, `controller/results`,
`workloads/*`, `explore`, `platform/fpenv`, `platform/isa`, `primitives/simd`) have mutants
that program never runs; the gate refuses a NOT-COVERED mutant, so those shards could only
fail. They have not yet been run (the sweep is weekly and these modules are new). See
Decisions.

## Decisions

### Sampling is in the controller's loop, at four fixed points

Options: (a) a sampler thread; (b) rounds on the controller's tick; (c) rounds in the
workers. (a) adds the only thread the controller would have and a P5 argument for no
benefit at 1 to 10 Hz, and forks before threads is the rule (PLAN §4.6); (c) puts the
observer inside the observed. Took (b), as `sampler.hpp` anticipates, with rounds at: before
the first fork (a counter's baseline before any worker can move it); every interval; at the
deadline on the instant the next phase is marked; after the last reap (what the workers did,
and left behind when they died). Proposed as an ADR:
`docs/adr/proposed/21-m3-integration-sampling-points-and-phase-windows.md`.

A round that is not later than the last is skipped, not offered and refused, because two
ticks can read the same clock tick and the fake clock does. A round the timeline refuses
(capacity) ends sampling and is reported on the console and in `SamplingReport::fault`; the
run goes on. The result has nowhere to say this (Contract questions).

### Phase windows share their boundary round

See the ADR. Per-phase counter deltas then sum to the run's; the naive reading does not
(disagreement 5).

### The result is assembled in `src/main`, not `src/controller`

`controller/results` links `loadforge_controller` (it uses `WorkerFate`), so a controller
that used results would be a cycle. The controller reports what happened (`RunReport`); the
command line, which already links everything, puts the result together. A consequence: the
result is assembled after the run, from a timeline the controller no longer touches.

### The null workload is a workload that verified nothing, and says so

There is no workload code in the worker yet (stream 11 has not landed), so `WorkloadResult`
is one entry per enabled workload with a zero tally and a duty cycle of `{0, 0}`: "no check is
scheduled". A duty cycle of `{1, 1}` would claim a check that never happens. The verdict is
therefore **incomplete** for every run today, which `derive_verdict` says correctly ("no
workload ... oracle_blocks at zero"), and exit code 0 still means every worker completed.

### Exit criterion 6 is met by the fallback `runner.hpp` states, not by deferral

M3 exit criterion 6 reads "expensive verification is demonstrably outside benchmark windows".
`verification/runner.hpp` says the opposite of how: checks happen in the slice that computed
the unit, "Benchmark mode keeps expensive checks out of its measurement by a lower duty
cycle, reported with the signature -- F17's stated fallback. Deferring checks to between
windows would need results held across slices; that is a contract change". The repository
(the runner header, which is frozen) wins over the brief. What this stream shows is the
recorded half: the duty cycle and the observers (sample interval, controller overhead) are in
both renderings (`ResultTest.TheDutyCycleAndTheObserversAreRecordedBesideTheSignature`).
Benchmark mode is not built (`stress` refuses it), so there is no benchmark window to be
outside of.

### A run that finds a fault does not exit 0

`loadforge stress` exited 0 whenever every worker completed. With a verdict in hand that
would be 0 for a run that counted corrected ECC errors, or one that found a wrong result.
Exit code **5** now means the verdict is a fault (`findings` or `corrected errors`,
`derive_verdict`'s own words), taken before 3 ("a worker did not complete"); 3 and 0 are as
before. Today's runs are `incomplete` (above) and exit as they did. Options considered: 0
for corrected errors with the verdict in the result only (a script reads files to learn a
machine is failing), or one code for everything not clean (loses 3's meaning). Took the
code a script can act on.

### Where the output goes is checked before the run

A run of five hours that finds its output directory missing when it ends has lost its
result. `ResultSink::prepare` creates and removes a file in the directory before the
telemetry is probed or a worker forked, and the real sink writes `run.json.partial` and
renames it, so a full disk or an unwritable directory leaves no short file that looks like
a result. Exit code 4 is "the run ended and a result could not be written"; 1 remains "the
run could not start".

### The disagreements are tests that are expected to fail

ADR proposed: `docs/adr/proposed/21-m3-integration-contract-disagreements-as-expected-failures.md`.
`loadforge_test_suite()` cannot say `WILL_FAIL`, so that executable repeats its body with the
property added to the discovery. The caveat is in the ADR: a failure that is a sanitizer's
report is also "expected", which the passing suite's identical code under the same
sanitizers guards against.

### Mutation: one unit per test program

`mutation.yml` now plans units: a fragment (a `src/` directory with a `CMakeLists.txt`) whose
tests fragment defines exactly one `loadforge_test_suite(loadforge_unit_*_tests)` is swept
by that program, and the shard of the module around it leaves it out through Mull's
`excludePaths`; every other module is swept by the shared program as before. The plan is
derived from the tree on every run. Units for the other architecture's SIMD are dropped.
`config`, `controller` (safety, supervision, orchestration, and now the loop's sampling) and
`main` remain units of the shared program; `verification/*` become eight units. This could
not be run here (Mull is not installable: its package host is blocked, as the workflow's own
bootstrap job says), so the planner and the generated Mull configuration were run locally
against the tree and read; the first weekly sweep is the real test, and the process check
(`check-processes`) will say if any of the new programs creates a process.

Header-only contract files (`telemetry/sample.hpp`, ...) belong to no unit, as before.

### Fixtures

The real captures (`/proc/stat`, `/proc/meminfo`, and the absent trees) are reused from
`tests/fixtures/`; hwmon, thermal-zone, fan and cpufreq trees are built in
`tests/integration/m3/machine.hpp` in the layout the kernel documents and are not claimed as
captures. powercap and EDAC come from the source streams' modelled trees, which their
README says are modelled. A hardware run (T11) should replace them.

### The ledger

`src/main` moved from `tests/obligations.toml` to its own fragment (the file's header says a
row moves the first time its owner edits it). It declares P7 for the stack: not the
sources' own states, but that each is followed through sampler, statistics, result and both
writers.

## Contract disagreements found

Each is a test in `tests/integration/m3/disagreements_test.cpp`, named
`<Component>_<ContractFile>_<Clause>`, that asserts what the contract says, fails today, and
is registered `WILL_FAIL`. No component outside this stream's directories was edited.

1. **`PowerSource` / `sample.hpp` — a counter is in the unit its metric names.**
   `Metric::unit` is `kJoules`; the source reports the raw `energy_uj` (microjoules); the
   statistics sum raw values; both writers print them under the metric's unit. +20 J in the
   fixture (`energy_uj` 1,000,000 to 21,000,000) is "delta 20000000, joules, rate 5e+06"
   for a 5 W load. `sample.hpp` calls the counter "energy in microjoules" and the unit
   joules in one place each. A unit or a scale is missing from `Metric` (contract change),
   or the source must convert (a `Counter` is an integer, so only to a finer unit).
2. **`CountersSource` / `sample.hpp` — "a counter only ever increases".** On a shared PMU
   the source reports the count scaled to the time its event was enabled, an estimate that
   goes down when the next round's count is exact: 1000 counted over half the time (2000),
   then 1500 counted all the time (1500). `counter_delta` reads every step down as a wrap
   at 2^64, so the run's delta is **18,446,744,073,709,551,116**. Machines with fewer PMU
   counters than events (every virtual machine that has a PMU at all) multiplex. Fix: the
   source reports unscaled counts, or the metric is a gauge ("estimated"), or unavailable
   while multiplexed.
3. **`EdacSource` / `sample.hpp` — a step down in a counter is a reset, not a wrap.** The
   source declares a modulus of 2^32 (the kernel's counters are 32 bits wide). At one error a
   second that counter wraps in 136 years; what makes it go down is `reset_counters` or a
   driver reloaded, and `counter_delta` reports 5 → 0 as 4,294,967,291 corrected errors,
   which `derive_verdict` turns into "corrected errors" with that number in the report. With
   no modulus it is worse (2^64 - 5). Nothing in the contract can say "this counter's wrap
   is out of reach": a `wraps` flag, or a rule that a step down is a gap (the summarizer
   already refuses to guess across one), is a contract change.
4. **`CountersSource` / `sample.hpp` — "never a zero standing in for [a value]".** The
   software-event finding above. Asserted on what the source asks the kernel for (the fake
   records `PerfEventSpec`), so the failure is the same on every machine; the real-kernel
   evidence is above.
5. **`Statistics` / `signature.hpp` with `timeline.hpp`'s `PhaseMark` — a phase's deltas
   sum to the run's.** Taking each phase's window as `[mark, next mark)`, which is how both
   headers read, leaves the advance across the boundary in neither. In the fixture the
   phases' deltas are 2 where the run's is 5. Worked around, not fixed: the glue's phase
   windows overlap by the boundary round (ADR proposed above). An F13 report built on the
   obvious reading would lose corrected errors at every boundary.
6. **`Results` / `result.hpp` with `signature.hpp` — a counter blind for part of the run is
   not a claim of cleanliness.** An EDAC counter unreadable for two rounds, 40 corrected
   errors counted in the gap, workloads fully verified, workers finished: `derive_verdict`
   says `clean`, because none of its four rules reads `unavailable_samples` and
   `CounterSummary::delta` (rightly) does not count across the gap. `kIncomplete` is
   defined as "something was not checked". The verdict contract chose that an *absent*
   counter is fine ("the capability report says EDAC was not read"); a counter that was
   there and went blind is a different thing, and the contract is silent.
7. **`Capability` / `source.hpp` and `sampler.hpp` — probe once.** `source.hpp`: `probe()` "is
   called once, before any `sample()`". `capability.hpp`: the sources "are probed here and
   nowhere else ... and the sampler that follows uses the same objects". `sampler.hpp`:
   `probe()` "calls each source's `probe()` once" and is the capability probe "without the
   rounds". A controller that reports capabilities with one and samples with the other
   probes every source twice (counted by a source in the test: 2, not 1). `PowerSource` and
   `CountersSource` happen to tolerate a second probe (they close and reopen); a source that
   does not would leak or double-register. The glue uses the sampler's probe alone.

## Contract questions

1. **Per-phase results have no place in `RunResult`.** F13 and M3 say EDAC deltas are
   reported per phase; `RunResult` has one `signature`. The glue computes them
   (`counter_by_phase`) and prints them on the console; the JSON and CSV do not carry them.
2. **A worker's death has no telemetry in `WorkerResult`.** The brief's "leaves the
   telemetry before its death in the result" is met by the run's signature spanning the
   death and by the console line naming how many points the timeline held at each reap.
   `SamplingReport::reaps` has the instant; the result has no field for "the readings at
   the time of death", though `Finding` has `telemetry`.
3. **`Observers` cannot say that sampling stopped, or that rounds were skipped.** A run
   whose timeline filled at 40 % has a signature that ends at 40 % and an `elapsed_ns` that
   does not; nothing marks it. (Console and `SamplingReport::fault` do.)
4. **`Fingerprint` gaps (stream 12's question 2) are real:** eight fields are absent on this
   machine with specific kernel reasons, and the JSON lists only the names. `topology::
   Unavailable` has no `remediation`, so even carried into the result the "what to do" would
   be missing for the fingerprint, unlike for every metric.
5. **`scope` is printed with `describe(Scope)`** ("memory controller", "node"), prose with
   a space, in a schema that gives availability and verdict stable codes
   (`availability_code`, `verdict_code`). A reader keyed on it is keyed on prose.
6. **No component turns the two CPU tick counters into a utilisation.**
   `frequency.hpp` exports `utilisation()`, which is a provider detail, not a contract; the
   Load Signature has `frequency.busy_ticks` and `frequency.total_ticks` deltas and nothing
   that says "62 % busy".
7. **`EdacSource` stops enumerating at a hole in the numbering** (stream 13's question 5).
   A controller whose `ce_count` is unreadable ends the list and the report says "2 of 2
   present" for the controllers before it; the failing controller's other counter is not
   reported at all. By the kernel's contiguous numbering this cannot happen, so it is
   recorded only.
8. **The capability probe and the sampler's probe** (disagreement 7) want one of them removed
   from the contract; the glue works either way.

## Exit criteria

M3 (`docs/IMPLEMENTATION.md` §4):

| # | Criterion | Evidence |
|---|---|---|
| 1 | A full Load Signature from fixtures alone | `FullSignatureTest.EverySourceContributesToOneSignatureFromFixturesAlone`: 26 metrics, 7 sources, no real sensor, values checked, in both writers |
| 2 | Every source × state has a fixture and a test | `SourceStates` (8 metrics across 7 sources × present, absent, denied, malformed, vanishing; `CounterWraps` for the three file-based counters that wrap and `PerfCounterWraps` for the performance counters) through the whole stack; the obligation gate agrees (`tools/check-test-obligations.py`) |
| 3 | Every absent metric is absent with a remedy, never zero | `SourceStates.AbsentIsAbsentWithARemedyAndNeverAZero`, `ResultTest.TheEveryAbsentMetric...`; the real run prints each remedy |
| 4 | A wrapping counter, proved by a fixture that wraps mid-sample | `CounterWraps` for RAPL energy, EDAC and `/proc/stat` ticks, `PerfCounterWraps` for a 64-bit count (disagreements 2 and 3 are the steps down that are *not* wraps) |
| 5 | Versioned schema; fingerprint reproducible across two runs on one machine | `RealBinaryTest.ARunWritesItsResultInBothFormatsAndTwoRunsShareTheirFingerprint`; `RealKernelTest.TheFingerprintIsTheSameTwice...` |
| 6 | Duty cycle recorded; expensive verification outside benchmark windows | recorded: `ResultTest.TheDutyCycleAndTheObservers...`; "outside": see Decisions (the runner header's fallback) |
| 7 | Mutation gating extended | `.github/workflows/mutation.yml` units; not run (Mull cannot be installed here) |

Stream: a worker killed mid-run leaves the telemetry before its death in the result
(`TelemetrySupervisionTest.AWorkerKilledMidRunLeavesTheTelemetryBeforeItsDeathInTheResult`, real
workers, real sources: the worker is killed 700 ms into a 1.8 s run, and the result holds at least ten rounds from before the death and some from after it).

## Verified locally (x86-64, GCC 13.3 and Clang 18)

- Debug build, the unit, supervision and M3 integration binaries, `ctest -R '^(cli_|release_gate)'`
  and the disagreement suite (every disagreement test fails as intended: ctest passes, the
  binary exits 1).
- The coverage gate over the whole tree at the last full run: **100.0 % lines, functions and
  branches** (11,427 lines, 2,062 functions, 8,968 branches), 2,776 ctest tests passed; after
  the last edits (sizing, the interval guard, the flag reader) `src/main` and the
  controller's top-level files again at 100.0 % (1,026 lines, 85 functions, 1,164 branches)
  from the binaries that exercise them.
- AddressSanitizer + UBSan (found the overflow above) and ThreadSanitizer: unit, supervision and
  M3 binaries clean. The scalar-only build (`simd-scalar`) and the static build: unit,
  supervision and M3 binaries pass in the first; the static `loadforge` is statically linked,
  carries no test hook and runs a real `stress --output json`. `tools/check-runtime-deps.sh`
  and `tools/check-no-test-hooks.sh` pass on the dynamic build.
- clang-format-18 over `src` and `tests`, clang-tidy-18 over every file this stream added or
  changed (it found: a function over the complexity threshold, `strerror` not being thread
  safe, public member variables, easily swapped parameters; all fixed), `python3
  tools/check-test-obligations.py` and `tests/tools/test_tooling.sh` (240 passed).
- Thirty repeated runs, 30 of 30, of the tests that create processes or threads: the
  worker-killed-mid-run supervision test and the real-binary and real-kernel tests (under GCC
  with coverage and under ASan + UBSan).

Not run here: ARM64 and the sanitizer, scalar and static builds over the *whole* test tree
(CI runs them); Mull.

## Left open

- Mull could not be run, so criterion 7's evidence is the workflow and not a sweep.
- The seven disagreements are for the manager and the owners of the contracts to settle; none
  was fixed here (rule: other components' code is not edited).
- ARM64 and the whole-tree sanitizer builds are CI's.
- The project still needs an outside reviewer.
