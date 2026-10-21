# The M3 contract corrections: seven disagreements, decided

**Written:** 2026-10-21 11:45 UTC
**Branch:** `fix/m3-contract-disagreements`
**Authority:** ADR 0004, "Changing a contract": the header, its suite and its reference
change together, every in-tree instantiation of the suite still passes, the reason is in
the header and here. The evidence for each disagreement is in
`docs/journal/21-integration-m3.md`; each was a test registered `WILL_FAIL` in
`tests/integration/m3/disagreements_test.cpp`. Those seven tests now pass, in
`tests/integration/m3/corrections_test.cpp`, and the `WILL_FAIL` executable is gone.

The shape of the whole change, so that a reader sees what it costs: one new unit
(`kMicrojoules`), one new function (`counter_advance`), one new field (`CounterSummary::resets`),
one new metric (`counters.pmu_share`), one removed function (`probe_capabilities`), three
window helpers in the statistics contract, one new verdict rule, and the result schema
going from 1 to 2.

## 1. PowerSource and sample.hpp: the unit of an energy counter

**Question.** `Metric::unit` was `kJoules`, the raw counter is `energy_uj`, and nothing
scales it: +20 J was rendered "delta 20000000 joules".

**Options.**
(a) Convert in the source. A `Counter` is an integer, so only to a finer unit, which is
the unit it already is in. Dead end.
(b) Add a scale to `Metric` (`unit_scale`), and have the summarizer and writers apply it.
Every consumer must apply it; one that forgets is wrong by 10^6, silently.
(c) Name the unit the raw value is in: add `Unit::kMicrojoules`, and state in `sample.hpp`
that a counter's raw value is in its metric's unit, exactly.

**Choice: (c).** The disagreement was a metric that named a unit its value was not in; the
cheapest correction that cannot be misapplied is for the name to be true. No consumer
changes behaviour, and a power in watts from a delta in microjoules is the consumer's
division, not a hidden multiplier. `kJoules` stays for a future gauge. The enum value is
appended last, so no existing value moves. Both providers (powercap, and the power PMU
whose scale is already folded into microjoules) report `kMicrojoules`.

**Files.** `src/telemetry/sample.hpp`, `src/telemetry/sources/power/power.cpp`; suite:
`tests/contracts/telemetry/reference_telemetry_test.cpp` (the unit's name); source tests:
`tests/unit/telemetry/sources/power/power_test.cpp`; stack:
`corrections_test.cpp::Correction.PowerSource_SampleHpp_AnEnergyCounterIsInTheUnitItsMetricNames`
(delta 20,000,000, "microjoules" in the JSON, no metric says "joules").

## 2. CountersSource and sample.hpp: is a scaled estimate a counter?

**Question.** A perf count scaled for multiplexing (`value * enabled / running`) goes down
between rounds when the next reading is exact (2000 then 1500), and `counter_delta` read
the step down as a wrap at 2^64: a delta of 18,446,744,073,709,551,116.

**Options.**
(a) Keep the estimate and make the metric a gauge. A gauge has no delta, and the run's
number of instructions is the one thing a counter is for.
(b) Report the raw count, and enabled and running times as two more counters per event
(15 metrics). Complete, and noisy: the consumer must divide for every use.
(c) Report the raw count as the counter, and the share of time the events were on the PMU
as one gauge, `counters.pmu_share` (ratio).
(d) Report the raw count and nothing about multiplexing. Honest about the number and
silent about its quality.

**Choice: (c).** A scaled estimate is not a counter: its differences mean nothing, because
the scale factor changes between readings. The kernel's raw `value` only increases, so it
is a counter, and a counter of events that were counted. The loss from multiplexing is not
hidden: `counters.pmu_share` is the smallest, over the hardware events read in the round,
of `time_running / time_enabled` since the probe (1 means no event was ever time-shared;
below 1 the counts are lower bounds and the true count is at most the raw count divided
by the share). One gauge answers "can I trust these counts" at the cost of one metric,
where (b) costs ten. The share is present when any hardware event is held; when none is
it is absent with the first hardware event's own words (so its remedy is the right one,
and the contract's "never silent, every absence has a remedy" holds); when held but no
hardware event is readable in a round it is `kUnreadable` for that round. A share above 1
(a reading with running above enabled) is clamped to 1, and one with nothing enabled yet
is 1. `sample.hpp` now states the rule for every source: **an estimate is not a counter.**

**Files.** `sample.hpp` (the `Kind` text), `counters.hpp/.cpp`; tests in
`counters_test.cpp` (raw not scaled, the count does not go down, share bounds, share
absent/unreadable, errno matrix extended); stack: `Correction.CountersSource_SampleHpp_ACounterOnlyEverIncreases`.
The metric count of a full machine goes from 26 to 27.

## 3. EdacSource and sample.hpp: a step down in a counter that cannot wrap is a reset

**Question.** The EDAC counters declared modulus 2^32, so a reset from 5 to 0 was read as
4,294,967,291 corrected errors, which the verdict turned into "corrected errors" with that
number. The contract could not say "this counter does not wrap within a run".

**Options.**
(a) A `wraps` flag on `Metric`. A second way to say what absence of `modulus` can already
say, and the two can disagree.
(b) Keep the modulus, and read a step down as a wrap only when the previous reading was
within some distance of the modulus. A heuristic in a contract; a counter reset from
4,294,967,290 to 3 is indistinguishable from a wrap, which is correct only by luck.
(c) Give `modulus` the meaning "wraps within a run" (absent: does not), and define the step
down in a counter with no modulus as a **reset**: the advance across it is unknown, not
guessed, exactly as across an Unavailable reading.

**Choice: (c).** At one corrected error a second a 32-bit counter wraps in 136 years; at a
thousand a second, in 50 days, which is still longer than a run. What makes it go down is
`reset_counters` or a driver reload. The same is true of every counter without a modulus
(64-bit perf counts, `/proc/stat` ticks, which wrap in centuries), so the rule applies to
all of them, and nothing is special-cased for EDAC. The pieces:
- `telemetry::counter_advance(previous, current, modulus)` returns the advance or nothing.
  `counter_delta` stays the modular arithmetic, for the sources that use it internally.
- `statistics::CounterSummary::resets` counts the steps down that were resets. `delta`
  and the `rate_per_second` span exclude the pair; the reading after a reset is the new
  baseline (so the errors counted after the reset are counted).
- The EDAC source no longer declares a modulus; 2^32 (`EdacSource::kRange`) remains the
  bound on a reading, so a value of 2^32 or more is still refused as malformed.
- The result gains `resets` per counter (JSON; CSV follows from the flattening), so
  schema version 2.

A genuine wrap of an EDAC counter within a run would now be a reset, and is not counted.
That is the right side to err on: an unknown advance, flagged and fed into the verdict
(item 6), instead of 4 billion invented errors.

**Files.** `sample.hpp`, `signature.hpp`, `summarizer.cpp`, `edac.hpp/.cpp`,
`results/document.cpp`, `results/result.hpp` (schema 2); suite:
`signature_contract.hpp` (`AStepDownInACounterWithNoModulusIsAResetAndNotAWrap`), the
reference summarizer, the definition tests; stack:
`Correction.EdacSource_SampleHpp_AStepDownInACounterThatCannotWrapInARunIsAReset`.
The integration tier's wrap rows now check a wrap only for RAPL (which has a modulus);
the EDAC, `/proc/stat` ticks and perf rows check a reset.

## 4. CountersSource opens software events with exclude_kernel

**Question.** A software event opened with `exclude_kernel` reads 0 for ever for context
switches, a zero standing in for a value. Which privilege does the correct opening need?

**Evidence (experiment, 2026-10-21, Linux 6.18.44, `perf_event_paranoid` 2; a standalone
program, `perf_event_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, pid=getpid(),
inherit=1)` read after each of five 20 ms sleeps):**

```text
uid 0      exclude_kernel=1   0 0 0 0 0
uid 0      exclude_kernel=0   1 2 3 4 5
uid 65534  exclude_kernel=1   0 0 0 0 0
uid 65534  exclude_kernel=0   open failed: errno 13 (Permission denied)      [paranoid 2]
uid 65534  exclude_kernel=0   1 2 3 4 5                                       [paranoid 1]
```

`perf_event_open(2)` agrees: EACCES is returned for "not setting exclude_kernel when the
paranoid setting requires it", and `perf_event_paranoid` 2 "disallows kernel profiling".
The scheduler counts a context switch in the kernel, so with the kernel excluded the
event is never counted.

**Options.** (a) Keep `user_only` and report the count as unavailable: honest but the
metric is then never present. (b) Open software events with the kernel included, and
accept the privilege it needs. (c) Drop the metric.

**Choice: (b), for the software event only.** It is the only way to count a context
switch. The privilege it needs is **CAP_PERFMON or `kernel.perf_event_paranoid` <= 1**;
the hardware events stay user-space-only and keep needing nothing at paranoid 2. So at
paranoid 2 without CAP_PERFMON the source now reports `counters.context_switches` as
**denied, with a remedy that says so** ("grant LoadForge CAP_PERFMON, or lower
kernel.perf_event_paranoid to 1 (it is 2); this event happens in the kernel, so it can
only be counted with the kernel included"), rather than present and 0. That changes the
privilege a run needs for that one metric, which is why it is recorded here. The remedy
function took a `kernel_side` flag because the old text ("2 or below already allows
counting your own processes in user space; look for seccomp") would be wrong for it.
Running as root, as the development machine does, nothing is denied.

**Files.** `counters.hpp/.cpp`; tests: user_only only for hardware, the denial and its
words, a denied read, the extended errno matrix; stack:
`Correction.CountersSource_SampleHpp_ASoftwareEventIsNotAskedToExcludeTheKernelItCountsIn`.

## 5. statistics/signature.hpp and PhaseMark: the rule for a phase's window

**Question.** Windows of `[mark, next mark)` leave the advance from the last reading before
a mark to the reading on it in neither phase, so phases sum to less than the run (F13).

**Options.**
(a) A window owns the pair whose *earlier* reading is inside it, so a pair that crosses the
window's end is counted. Changes `delta` for every window that ends between readings; the
src/main workaround would then double count and must change in the same commit (not
allowed here).
(b) A window owns the pair whose *later* reading is inside it. Same problem, and it
assigns the advance to the wrong phase: the round on a mark closes the phase before it
(sampling points: the controller marks the next phase on the deadline's round).
(c) State the rule for the windows themselves: a window that ends at a mark closes one
nanosecond past it, so the reading on a mark is in both neighbours and every pair of
consecutive readings is in exactly one phase.

**Choice: (c).** It is the rule `src/main` already implements and the proposed ADR records,
so nothing moves, and it puts the knowledge where it belongs: in the statistics contract,
beside `Window`. `signature.hpp` now carries `kClosingMargin`, `run_window(first, last)`
and `phase_window(marks, index, last)`, constexpr and tested as definitions, and a
`SummarizerContract` case (`TheWindowsOfARunAndItsPhasesSumToTheRun`) that every
summarizer, the consumers' mocks included, must pass. A gauge's summary counts the shared
reading in both phases: a statement about the instant.

**Files.** `signature.hpp`; `signature_contract.hpp`, `reference_signature_test.cpp`;
stack: `Correction.Statistics_SignatureHpp_PhasesTakenAtTheirMarksSumToTheRun`.

## 6. results/result.hpp: a blind counter is not a claim of cleanliness

**Question.** `derive_verdict` never read `unavailable_samples`, so a run in which the EDAC
counter could not be read for part of the run, and 40 errors were counted in the gap, was
`clean`.

**Options.** (a) Any metric with unavailable samples makes the run incomplete: a fan
sensor that dropped one reading would then void the verdict of a run that verified
everything. (b) Only the counters the verdict reads: the corrected- and uncorrected-error
counters. (c) Also require the capability report to list them as present.

**Choice: (b).** `kIncomplete` means "something was not checked". The things the verdict
claims about are those counters; a gauge's dropout is in the signature and is not a claim.
A series is in the signature only if it was present at probe, so an *absent* error counter
(no ECC on this board) is not in it, is not blind, and still does not stop a run being
clean, as the contract chose: the capability report says it was never read. A counter that
was there and went blind (`unavailable_samples` above zero, including one unreadable for
the whole window) or was reset (`resets` above zero, item 3) is incomplete. The order is
unchanged: findings first, then incomplete (which now outranks corrected errors for a
counter that counted some and was blind for the rest), then corrected, then clean.
`results::counter_blind` is in `aggregate`.

**Files.** `result.hpp` (rule 2, `kIncomplete`), `aggregate.hpp/.cpp`, `verdict.cpp`;
suite: two `VerdictContract` cases, the reference verdict, and the main stream's mock
verdict (a consumer's reading, run against the suite); stack:
`Correction.Results_ResultHpp_ACounterBlindForPartOfTheRunIsNotAClaimOfCleanliness` and
`...ACounterThatWasResetIsNotAClaimOfCleanlinessEither`.

## 7. Who probes: the capability report or the sampler

**Question.** `source.hpp`: `probe()` "is called once". `capability.hpp` claimed the probe
("probed here and nowhere else"); `sampler.hpp` claimed it too and its `probe()` also
registers the present metrics as series. A controller using both probed every source twice.

**Options.** (a) The capability module probes and the sampler reads a report it is
given. The sampler then needs a second input, and the probing and the registering are
split across two components with an ordering between them. (b) The sampler probes; the
capability module renders. (c) Keep both and make the second probe harmless: every source
would have to survive a second probe, which is a requirement on seven sources to hide a
disagreement.

**Choice: (b).** The sampler must call `probe()` anyway to register the series, and its
probe is already idempotent ("a second call answers the same report and probes
nothing"), so it is the one place that can say "once" truthfully. `probe_capabilities()`
is removed from `capability.hpp`; `render_capabilities()` stays and is the module. `source.hpp`
now says the sampler alone calls `probe()`, `sampler.hpp` says it is the only prober and
that asking again is free, and the `CapabilityReport` is documented as its answer. The
sampler's contract suite gained `TheSamplerIsTheOnlyProberAndProbesEverySourceOnceHoweverOftenAsked`
(both sources, three probes, a round, a fourth probe: one `probe()` each). The capability
module's tests build their reports with a local stand-in and still run the real sources
over the captured machine and this one; the module no longer `consumes` `SourceContract`
in the ledger.

**Files.** `capability.hpp/.cpp`, `source.hpp`, `sampler.hpp`; `sampler_contract.hpp`,
`capability_test.cpp`, `obligations.d/telemetry/capability.toml`; stack:
`Correction.Capability_SourceHpp_ASourceIsProbedOnceWhoeverReportsItsCapabilities`.
`src/main` already used the sampler's probe alone, so no code there changes.

## Consumers' mocks, run against the changed suites

Two mocks of other components had to follow the contract, which is ADR 0003 working as
intended: the summarizer mock in `tests/unit/controller/telemetry_mocks.hpp` (used by the
controller and main tests) now uses `counter_advance` and fills `resets`; the verdict mock
in `tests/unit/main/contract_mocks_test.cpp` has the blind rule. Both were failing the new
suite cases before the edit, as they should. No `src/main` file was touched.

## Follow-ups (not done here)

For `src/main` (reconciled by another worker; not edited):
1. `cli::phase_windows()` and `cli::run_window()` (`src/main/run_result.cpp`, with
   `kWindowEndMargin`) are to be replaced by `statistics::phase_window()` and
   `statistics::run_window()`. Behaviour is identical; the proposed ADR 21 sampling-points
   decision 3 is now the contract's.
2. The console's per-phase EDAC report (`counter_by_phase`) and the summary lines read
   `delta` only; they should also say when a counter has `resets` above zero or
   `unavailable_samples` above zero ("the advance is not known"), since the verdict now
   says `incomplete` for it and the console would otherwise print "no error counted in any
   phase" beside it.
3. `src/main/telemetry_rig.cpp` calls `sampler->probe()` and nothing else probes: nothing to
   change; keep it so. If the capability report is wanted before the sampler exists, build
   the sampler first.
4. The fingerprint and capability text in the console count metrics (the machine's
   `counters.pmu_share` is one more, absent where there is no PMU). Any hard-coded count of
   metrics (26 in the M3 integration tests, 23 in the journal) is 27 (24) now.

For the controller (`src/controller/orchestration.*`, `plan.*`): none required. The
controller feeds the sampler and reads no metric names.

For `docs/` and the manager: the ADR proposed `21-...-expected-failures.md` and
`21-...-phase-windows.md` carry an outcome note; numbering them is the manager's.
`docs/journal/21-integration-m3.md` section "Contract disagreements found" is history and
unchanged.

## Gates

Each exit code is the command's own, written to a file and read from it. Run 2026-10-21 on
the development machine (x86-64, Linux 6.18.44, GCC 13.3; Clang 18 for tidy).

| Gate | Result |
|---|---|
| `cmake --preset debug`, `cmake --build --preset debug -j3`, `ctest --preset debug -j3` | 0, 0, 0 (4329 tests passed; 3 disabled or skipped as before) |
| `cmake --preset coverage`, build, `ctest --preset coverage -j3` | 0, 0, 0 (4329 tests) |
| `tools/coverage.sh build/coverage` | 0: lines 100.0 % (15,980), functions 100.0 % (2,803), branches 100.0 % (11,265); every source file reported |
| `clang-tidy-18 -p build/tidy` over the 27 changed `.cpp` (compile database from `CC=clang-18 CXX=clang++-18 cmake --preset debug -B build/tidy`) | 0 |
| `clang-format-18 --dry-run -Werror` over the 40 changed `.cpp` and `.hpp` | 0 |
| `python3 tools/check-test-obligations.py` | 0 (69 modules) |
| `asan` preset: configure, build, `ctest` | 0, 0, 0 (4328 tests) |
| `simd-scalar` preset: configure, build, `ctest` | 0, 0, 0 (4247 tests) |

Two things about the coverage run, so they are not mistaken for faults. The first
`tools/coverage.sh` exited 64: I had run the coverage tests twice over the same `.gcda`
files, the counters accumulated past 2^32 in one loop of `dense_linear/scalar_kernel.cpp`,
and gcov's own guard (suspicious hits, gcc bug 68080) refused to read them. The gate has
no `--gcov-ignore-parse-errors` on purpose; deleting the `.gcda` and running once gave the
result above. Before that, the first complete run found one branch missing in
`counters.cpp` (an aggregate built of a string temporary, journal §1.13); the channel is
now built in place, a field at a time.

The ThreadSanitizer preset was not run (it is not in the list of gates asked for). The
ARM64 build is CI's.
