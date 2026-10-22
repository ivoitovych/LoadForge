# Journal fragment 33 — explore integration

**Written:** 2026-10-22 13:10 UTC
**Change:** `src/main` — the provider of `explore::TrialRunner` over the controller (`trial_runner.{hpp,cpp}`) and `loadforge explore` (`explore_command.{hpp,cpp}`); `tests/unit/main`, `tests/integration/m6`.

## Established by experiment

### A run that must reach its deadline cannot be built over the fake of the system calls

The first version of the deadline test started two workers on the fake and scripted no reap. The controller asked them to stop, escalated, and went on waiting for children the fake never reaped: the test did not end (killed at three minutes, 98% of a core). The orchestration tests get a deadline by releasing the workers' pipe ends and scripting the reaps from inside the reporter's `event` callback; the trial runner owns its reporter, so the path "a run with a `stop` mark" is reached by real processes only (`tests/integration/m6`), and the unit tests drive the other ending, workers that exit at once, which has no `stop` mark and is windowed by the last round.

### `poll(2)` with no descriptors is the settle phase's sleep, and it sleeps

`Syscalls::poll_readable({}, ms)` is a pure wait on the real kernel. The integration test settles on `memory.available` with a one-second window and reads `settle_ns` back between 1.0 s and the 20 s maximum, so the wait is real, and the phase cannot be satisfied before its window is full. The fake's poll does not advance the clock; the unit tests move time by reading it (10 ms per read) and check the wait asked for (one millisecond at least, a quarter of the window, never longer than the sample interval or the time that is left).

### Three coverage traps from §1.9, §1.13 and §1.22, met again

- `explore_command(..., TrialOptions{}, ...)` as an argument left eight branches no test can take in `cli.cpp`: the temporary's `std::string` members are destroyed in a cleanup block that only runs if a later argument throws. A named `const TrialOptions kDefaultTrialOptions{}` beside `kDefaultOptions` removes them, as it did for stress.
- `inputs.timeline = &rig->timeline();` carries a null test: `BoundedTimeline` derives from both `TimelineSink` and `TimelineView`, so the conversion of a pointer to the second base must keep null as null, and the arm for null is never taken. Binding a reference first (`const TimelineView& view = rig->timeline(); inputs.timeline = &view;`) leaves nothing to test.
- A call spread over several lines (`make_rig(..., timeline_capacity(RunShape{...}), ...)`) left its middle line "executable and never executed": the inlined `noexcept` call is attributed to the next line. Splitting it into statements removed the phantom line.
- `return refused(key, "expected ... '" + std::string{text} + "'")` left one branch (the temporaries' cleanup): the message is built in a named string first, as §1.13 says.

### This machine's free memory is too noisy to rank one worker against three

The first search tests asked which of `one:sort:1` and `three:sort:3` (64 MiB each) leaves less memory available, and expected `three`. The workers' footprints differ by 128 MiB. Over 30 repetitions of the whole file, two failed, naming `one`. The searches that failed reported replicate standard deviations of 250 MB for `memory.available` (the first search of the day, on a quiet machine, gave 12 MB for the same counter): the free memory of this VM moves by hundreds of megabytes between trials for reasons outside the search, and its cause is not established here. The tests now assert what holds on any machine under any load: the report is complete (worst case with `+/-` and `n=3`, every replicate, the reference's readings, 8 trials in one round) for a real objective, and a search on a quantity no candidate changes (`memory.total`) ranks nothing, with a standard deviation of 0. Which candidate wins a noisy objective is measured, not asserted: see T11 below.

## Decisions

- **A trial is one `Controller::run`, with a rig of its own.** Each trial makes its own telemetry rig (sources, sampler, bounded timeline), runs the controller on it, summarises the load window, and drops it. Options: (a) one long-lived rig and timeline for the whole search; (b) a rig per trial. (a) means the timeline grows by every trial of a search of hours and can only refuse at capacity; and a window of the timeline would have to be found among all the trials'. (b) re-probes the sources each trial (a few file reads), which costs nothing against a load of seconds or more. `capabilities()` is the first probe, made once by `create()` and returned unchanged for the runner's life, which is what the contract asks ("probed once"); a later trial's rig may in principle see a different machine, and then its signature simply lacks a series, which explore reports as `kNoReading`.
- **The settle phase is the cool-down, and it is measured, not slept.** `Settle` is the only way a candidate asks for a controlled starting state, so a candidate without one gets none (PLAN §4.4 requires cool-down between explore trials; explore sets `Settle` on every candidate of a search, and the command does so for all of them when `--settle` is given). The phase takes rounds on the trial's own sampler every `clamp(window/4, 1 ms, sample interval)`, and stops when the least-squares slope over the last `window_ns`, computed by the statistics module's own summarizer, is within the bound **and the window is full** (elapsed ≥ `window_ns`), or when `max_ns` has passed. The slope is never judged from a window that has not been filled: two readings a millisecond apart give a slope of anything. An unsettled phase reports `settled = false` and `settle_ns = max_ns`; it is not an error, because explore records `settled` per trial and the report can say how many trials started warm. A sensor that drops out during the phase reads as an Unavailable summary, which cannot be settled on, so the phase runs out its time rather than guess.
- **Fixed affinity is the candidate's CPU list.** `Candidate::cpus` becomes `VerificationOptions::cpus` (sorted, de-duplicated), and the plan places worker slot `i` on `cpus[i mod n]`. Empty means the CPUs the process may use. A CPU the process may not use refuses the candidate (`kCandidate`) before anything starts, so a trial is never silently run somewhere else.
- **Threads are worker processes, and workloads are dealt out in turn.** `workloads = {a, b}`, `threads = 5` is workers `a b a b a`. A candidate with fewer threads than workloads would not run one of them, which is a candidate that is not what it says, so it is refused. More than 1024 worker processes is refused as a mistyped count. A duration is refused above 30 days (so the sums of lengths below cannot overflow).
- **The seed is the same on every trial.** Two replicates then run the same units; what differs between them is the machine, which is the variance explore is measuring. A varying seed would add the workload's own variance to a number meant to describe the configuration.
- **The load window is exactly `duration_ns` wide and ends one nanosecond past the controller's `stop` mark.** The controller takes a round at the deadline and marks `stop` at the same instant; a statistics window that closes on a reading closes one nanosecond past it (`statistics::kClosingMargin`), so the window is `[stop + 1 - duration_ns, stop + 1)`. The configured duration is whole milliseconds, so a duration of part of a millisecond makes the controller run up to a millisecond longer than the window. A run that ended before its deadline has no `stop` mark and is windowed by its last round; its verdict says it did not finish.
- **No isolation ladder per trial.** `stress` repeats a wrong result in processes of its own to say whether it is the CPU, the memory or the code. A trial's verdict is what `derive_verdict` says of the trial (a wrong result is `kFindings` whether or not it is isolated), and a search that finds one reports the candidate and stops scoring it; isolating it is `loadforge stress`'s job on that candidate. Running the ladder inside a search would add minutes to a trial the search schedules by the second.
- **A trial that cannot keep its telemetry is a failed trial, not an empty window.** If the sampler stopped mid-trial (a round the timeline refused) the trial answers `kRun` with the controller's own words, rather than returning a signature whose objective reads as "no sample", which would end the search with a less useful error.
- **A worker whose exec failed returns in the forked child.** `Controller::run` returns in the child (orchestration.hpp). The trial answers `kRun` and keeps the worker's exit code, `worker_exit_code()`; the command checks it before it prints anything and ends the process with that code, exactly as `stress` does. The search is not told anything else, so a child never prints a report.
- **The command's grammar is flags, not a configuration file.** `config::Mode::kExplore` parses but "lands with a later milestone" and `src/config` is not this stream's; the search space of explore is a curated list that reads best on one command line: `--objective METRIC[@INDEX]:STATISTIC:higher|lower`, `--candidate ID:WORKLOAD[+WORKLOAD...]:THREADS[:CPUS]` (repeatable), an optional `--reference` of the same form (default: the first candidate's workloads, one worker each, anywhere, id `reference`), the settle phase and the search parameters. Every candidate and the reference share the duration, the settle phase and the default duty cycle, so F17 holds by construction.
- **Exit codes keep `stress`'s vocabulary.** 0 every candidate scored; 1 refused before the first trial (objective, search or machine: the objective on an unavailable metric lives here, with the source's own reason and remedy); 2 usage; 3 a trial could not be run or the reference was unusable; 5 a candidate's trial found a fault (a finding, never a score); 6 a candidate's trial did not finish, or a trial held no reading of the objective. When 5 and 6 both apply, 5.
- **The report lists everything it computed from.** The worst case with its replicate count, standard deviation and 95% interval (explore's `format`), the ranking against the best rival, every candidate's replicates in order (raw and drift-corrected), and every reference reading with the time it was taken. A bare number is not printable.

## Contract questions

- **`trial.hpp`: where does the load window start?** The brief says "[load start, load start + duration_ns)" but the controller does not record the instant its last worker launched, and counts the duration from it. Reading chosen: the window is anchored on the deadline (the `stop` mark) and extends back `duration_ns`, plus the one-nanosecond closing margin so that the deadline round is inside. The start is therefore the last launch to within a controller tick. Worth saying in the header.
- **`trial.hpp`: is a candidate without `settle` entitled to a cool-down?** The contract has `Settle` as the only expression of one. Read: no. PLAN §4.4 requires cool-down between explore trials, so the explore stream (or its command) should put a `Settle` on every candidate; the command does.
- **`TrialOutcome::settle_ns` of an unsettled phase.** Unstated. Read as `max_ns`: the time the phase took. `settled = false` is the signal.
- **`ASettlePhaseIsMeasuredAndBounded` uses a 1 ms window.** A runner that fills the window before it judges the slope (which this one does) can never settle on a 1 ms window sampled no faster than 1 ms, so the contract's example ends unsettled at `max_ns`. The contract's assertions (0 ≤ settle_ns ≤ max_ns) hold; a stronger assertion that the example settles would be unsound for any runner that does not take a slope from fewer than two readings.
- **Stream 28's question about window order is answered:** windows are on the monotonic clock and each trial's is later than the previous one's, because trials run one after another and a rig's first round is later than the last of the trial before. Worth stating in `trial.hpp`.
- **Stream 28's question about corrected errors:** a trial with verdict `kCorrectedErrors` reaches explore as a score, as it read it. This runner does not change that; the report does not yet flag it. A reporter may want to.
- **`TrialFault::kCandidate` for "fewer threads than workloads", "more than 1024 threads", "duration above 30 days" and "a settle phase that is not well formed"** are this runner's additions to the contract's list; each is a candidate that is not runnable as asked.

## What the tiers hold

- T1/T2 (`tests/unit/main/trial_runner_test.cpp`, `explore_command_test.cpp`): over the fake of the system calls and scripted sources, the contract suite, every refusal, every settle ending, every injected failure of the clock, the wait, the timeline, the fork and the summarizer.
- T8 (`tests/integration/m6`): the contract suite against real worker processes; the shipped binary refusing an objective or a settle metric this machine cannot read before any worker starts; a whole search with real workers that names its worst case with replicates, spread and interval, twice.
- T11: see below.

## T11 — repeatable worst-case discovery on real hardware (M6 exit criterion 1)

**Criterion:** "repeatable worst-case discovery on real hardware, with variance reported and replicates recorded; an objective whose metric is unavailable fails loudly rather than silently scoring zero."

### The half that is met in full: an unavailable objective fails loudly, before anything runs

```
$ loadforge explore --objective thermal.package:max:higher --candidate a:sort:1
loadforge: explore: objective metric unavailable: objective metric thermal.package is unavailable: no hwmon chip with a temperature input and no thermal zone (this machine exposes no temperature sensor to read; in a virtual machine or a container that is the platform's choice and nothing inside it can change that, while on bare metal the platform's hwmon driver (coretemp on Intel, k10temp on AMD) may not be loaded)
exit code 1
```

The reason and the remedy are the source's own, the exit code is 1 (refused before the first trial), and no worker was started: the integration test checks the console holds no run banner and that the command took under five seconds. A settle phase on the same metric is refused the same way (`settle metric thermal.package ...`), by explore at configuration and again by the runner at the trial. An objective on a metric no source knows is `objective metric unknown`, also 1. All three are tests (`tests/integration/m6/binary_test.cpp`, `tests/unit/main/explore_command_test.cpp`).

### The half that needs real hardware: what was run, on what, and what it shows

**The machine** (2026-10-22 15:32 UTC, commit `283de7c` plus the journal): a container on a virtual machine (Linux 6.18.44, x86-64), four vCPUs of an "Intel(R) Xeon(R) Processor @ 2.80GHz", 16 GiB, no hwmon chip, no thermal zone, no cpufreq, no RAPL, no PMU: 10 of 24 metrics present (memory, CPU tick counters, context switches). A debug build. Other tenants share the host; nothing is pinned or isolated.

**What was run.** Two searches, each five times in a row, with the same arguments, over one worker, two and four (`one:sort:1`, `two:sort:2`, `four:sort:4`), 4 s of load per trial, 250 ms sampling, base replicates 1, final replicates 4, reference replicates 3, reference interleaved every 2 trials, the default reference (`sort`, one worker). Each search is 14 trials in 2 rounds (about 50 s). Every candidate replicate and every reference reading is in the report; the files are the machine's and are not committed.

| objective | search | worst case | mean | 95% half-width | n | sd | ranking |
|---|---|---|---|---|---|---|---|
| memory.available, min, lower is worse (bytes) | 1 | four | 1.58743e10 | 3.90e7 | 4 | 1.50e7 | not distinguishable |
| | 2 | four | 1.58740e10 | 4.27e7 | 4 | 1.54e7 | not distinguishable |
| | 3 | four | 1.58917e10 | 5.31e7 | 4 | 1.07e7 | not distinguishable |
| | 4 | four | 1.58860e10 | 5.62e7 | 4 | 1.11e7 | not distinguishable |
| | 5 | four | 1.58983e10 | 5.65e7 | 4 | 5.95e6 | not distinguishable |
| frequency.busy_ticks, rate, higher is worse (ticks per second) | 1 | four | 402.46 | 7.31 | 4 | 1.61 | separated |
| | 2 | four | 401.89 | 2.88 | 4 | 0.88 | separated |
| | 3 | four | 404.12 | 16.97 | 4 | 0.89 | separated |
| | 4 | four | 402.91 | 13.31 | 4 | 0.63 | separated |
| | 5 | four | 399.59 | 2.22 | 4 | 0.15 | separated |

All ten searches name the same worst case, four workers, and the five intervals of each objective overlap one another (memory: 1.587e10 to 1.590e10 bytes, all within each other's half-widths of 4e7 to 6e7; busy ticks: 399.6 to 404.1, all within 4.6 of one another). With four workers on four vCPUs the busy rate is the machine's 400 ticks per second (100 Hz times four CPUs), and one and two workers measured 108 and 207 in search 1 of that objective: the instrument reads what the load is. The interval is wide where it should be: the "best rival" in a successive-halving round has one or two replicates, whose interval is wide, so memory's ranking says it cannot tell four from two, which is the honest statement (four's mean is 1.587e10, two's 1.598e10, but two has two replicates and a half-width of 2.9e8). Search 1 of the busy-tick objective, as printed:

```
candidate one: eliminated, ranked in 1 round(s), 1 replicate(s)       replicates: 108.021
candidate two: eliminated, ranked in 2 round(s), 2 replicate(s)       replicates: 208.048 206.91
candidate four: survivor, ranked in 2 round(s), 4 replicate(s)        replicates: 403.942 403.576 401.856 400.466
reference: anchor 107.629, drift 0.885894, mean 105.992 +/- 4.80847 (95% CI, n=7, sd 5.19902)
```

**What this shows:** the mechanism does what the criterion asks: replicates are run and every one is recorded, the spread and the 95% interval are on every claim, drift is measured against an interleaved reference and its noise is added to the interval, and a repeated search names the same worst case with intervals that agree. **What it does not show:** a thermal result. This machine cannot read a temperature, so none of this says anything about the quantity the criterion is about (the highest temperature a configuration can reach), about drift as a warming machine produces it (the reference here moved by less than 1% over a search), or about a settle phase on a real sensor (the settle phase is tested on this machine against `memory.available`, which is not a sensor that settles, and against scripted sources). A VM with neighbours is also not a machine whose variance means anything for a datasheet.

**What still needs real hardware**, for the criterion to be marked met: a machine with a readable package temperature (coretemp, k10temp or a thermal zone) that is otherwise idle, the same command with `--objective thermal.package:max:higher --settle thermal.package` over a curated set (for instance the dense-linear and the sort workload at one thread and at all cores, with and without pinning to one core), at least five repetitions of the whole search, the report of each kept whole (the replicates are in it), the machine described (CPU model, cooling, ambient), and the claim made only if the five searches name the same worst case and their intervals overlap. That is a `tests/hardware` occurrence of the manager's naming, one file per occurrence.
