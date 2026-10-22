# Journal fragment 34a — the temperature ceiling

**Written:** 2026-10-22 23:05 UTC
**Change:** `src/controller/temperature.{hpp,cpp}` (new), `src/controller/{orchestration,report,safety}.*`, `src/main/{cli,outcome,run_result,telemetry_rig,trial_runner,explore_command}.*`, `src/reporters/console.cpp`; `README.md`, `docs/safety.md`, `docs/architecture.md`, `docs/telemetry-sources.md`, `docs/hardware-validation.md`; tests. No contract header and nothing under `tests/contracts/` was touched. `docs/ROADMAP.md` is the manager's.

## What is done

`limits.max_temperature_c` is enforced. After every sampling round the controller reads back, from the timeline, the point each temperature series received (every gauge in degrees Celsius: `thermal.package`, `thermal.core`, `thermal.sensor`, `thermal.zone`). A reading at or above the ceiling asks every worker to stop and ends the run through the ladder the deadline already uses (request, `SIGTERM` after the stop grace, `SIGKILL`), with exit code 7. The console, the result files and the journal say which sensor, what it read, the ceiling and when. A ceiling that nothing can read is a refusal before the first worker. `stress --test` and `explore` take `--max-temperature C`.

Shown on the real binary (a private mount namespace with a tmpfs over `/sys/class` holding a fake coretemp chip, because this container has no sensor; see "Established by experiment"): a run of two `sort` workers whose chip went from 45 C to 93 C three seconds in, with `--max-temperature 90`, printed the stop at 3.172 s, had both workers exited by 3.452 s, wrote `run.json` and a journal that replays as `phase "ceiling 90 C reached: thermal.package 0 (hwmon:coretemp)" ... 93.0C`, and exited 7. `docs/safety.md` §2 has the transcripts.

## Established by experiment

### The real binary can be given a fake hwmon without root's help on the machine, only in a mount namespace

`unshare -m` then `mount -t tmpfs none /sys/class` hides this container's `/sys/class` and lets a script build `hwmon/hwmon0/{name,temp1_label,temp1_input}` under it. The thermal source reads `/sys/class/hwmon` through `platform::FileSystem`, so the shipped binary sees the fake chip and nothing else changes (topology reads `/sys/devices`). A background `echo 93000 > temp1_input` changes the reading mid-run. This is how the transcripts in `docs/safety.md` were made. It needs root and a kernel that lets the namespace mount a tmpfs; the tests do not use it (they use the roots seam below).

### The thermal source can be pointed at a directory, and a file rewritten by `rename` is never seen half-written

`cli::roots_under(dir)` gives every source `<dir>/sys/...`; `tests/supervision/temperature_test.cpp` builds `dir/sys/class/hwmon/hwmon0/{name,temp1_label,temp1_input}` and changes the reading from a thread (controller level) or a helper process (command level). The write is `temp1_input.next` then `rename(2)`, so a reader sees the old number or the new one. A first design wrote the file in place; a read between the truncate and the write would be a malformed reading and a note about a sensor that "can no longer be read", which is a different test from the one intended.

### A reporter event is told before the thing it names happens only if the controller says so first

The first version of the ceiling told "temperature ceiling: ... stop requested" and then wrote the Stop. The unit tests play the workers from inside the reporter's callbacks (the only place a test can act while the loop owns the thread); a worker that leaves at the event closes its control pipe, and the Stop write that followed got `EPIPE`, so `commands.sent` was 0. The deadline path tells its event after `request_stop`. The ceiling now does the same, which is also the honest order ("stop requested" after it was).

## Decisions

### D1. The ceiling reads the timeline, not the sensors and not a changed sampler

**Options.** (a) Change `Sampler::round` to return the readings it appended: a contract change (`telemetry/sampler.hpp` is frozen, ADR 0004). (b) Have the controller read the sensors itself: a second reader of a source, which the sampler's contract forbids ("the sampler is the only prober") and which would make the reading that stopped the run differ from the reading in the report. (c) Read back the timeline through `TimelineView`, which the contract names the controller as a reader of ("the controller when a worker dies").

**Chosen: (c).** `RunOptions::view` is the same timeline the sampler writes, as the reading side. After a round, a series' last point has the round's timestamp, or the series missed the round (a round the timeline refused part of the way), and both are told apart. The reading in the report, the reading in the journal and the reading that stopped the run are one reading. `RunOptions` is not a contract header.

### D2. Which readings count: every Celsius gauge, any one of them

**Options.** (a) `thermal.package` only. (b) package and core. (c) every series that is a gauge in degrees Celsius.

**Chosen: (c).** `docs/telemetry-sources.md` already says that on a machine with neither coretemp nor k10temp there is no `thermal.package` and the temperature is a `thermal.zone` or `thermal.sensor`. With (a) a ceiling would be refused or ignored on every ARM64 board and most laptops. The source's own comment says nothing is guessed to be "the" package temperature. The price is stated in `docs/safety.md`: a chipset or VRM sensor, or a sensor with a bogus fixed value that passes the physical range check (the thermal source's comment records `-128000` for "not connected", and a *high* sentinel would stop every run), counts as much as the die. The ceiling errs towards stopping; the owner's hardware validation will say whether any real machine has such a sensor. If one does, the answer is a per-sensor exclusion in the configuration, which is a schema change and not made here.

### D3. A ceiling that cannot be read: refuse by default (the owner's lean, confirmed by the documents)

**What the documents say.** PLAN F3: "Every metric is nullable, and absence is reported", the silently empty column is "the worst outcome"; F9: orphaned workers "with no ceiling enforced" are "strictly worse than the problem the split was introduced to solve" and "the one genuinely dangerous state the process split can create"; "software thermal protection is a backstop behind the CPU's own throttling"; `safety.md` §1 (as #101 left it): "a run that reaches [the CPU's protection] ends with the machine off and no report". The memory limit's precedent runs the other way (it runs and says NOT ENFORCED), but its failure is an OOM kill of one worker with a record; an unread temperature leaves the whole machine at full load with nothing to stop it.

**Options.** (a) Run, banner "NOT ENFORCED" plus the source's reason. (b) Refuse (exit 1, nothing started), with the source's reason and what to do. (c) Refuse unless an option says to go on.

**Chosen: (b).** The banner line is one of a dozen and sits under a capability report of two dozen lines; a user who wrote a ceiling in a configuration and started a six-hour run on a VM would have believed it. (c) was considered: an `--allow-unenforced-ceiling` flag. It is the unsafe act under a name; the explicit way to run without a ceiling is to remove the ceiling (delete the key, leave out the option), which is a one-line change the user can see in their own file. `stress --config` parses flag/value pairs only, so a boolean flag would also have been a change to that parser for a flag nobody should use. Not built; if the owner wants it, it is small.

`limit_statuses` still has the "NOT ENFORCED (no temperature can be read; configured N C)" arm, because it is a total function of its inputs and tells the truth about them; no run reaches it.

### D4. A machine already at the ceiling is refused too

The baseline round is taken before the first fork. A sensor at or above the ceiling in it means the run would end at its first tick with workers that never worked, and the user would read exit 7 as a finding about the load. Exit 1 and "the machine is already at its ceiling, before any worker started" says what is true. This was not asked for; it falls out of having the baseline and costs one test.

### D5. Sensors that go during a run

**Options.** (a) Ignore a sensor that stops answering (the timeline records the `Unavailable`; the others are watched). (b) Stop the run at the first unreadable round. (c) Tell the first loss once, keep watching the others, and stop only when *no* sensor has answered for an allowance.

**Chosen: (c), with 10 s (`RunOptions::temperature_blind_after_ns`).** (a) leaves a ceiling that is silently dead when the last sensor goes: the case F3 names as the worst. (b) kills a five-hour run for one failed read of one of 64 core sensors. The allowance is the staleness allowance's order of magnitude (2 s for the null workload, 10 s for a verifying one) and a sampled sensor that has not answered for ten sampling intervals is not flaky, it is gone. The stop is the same as a crossing (exit 7) and its text says "the ceiling can no longer be enforced". The same stop follows when sampling itself ends (a round the timeline refused, `sampling_live_` false): with no sampler behind it the view never updates. The comparison is strict (`>`): exactly the allowance is not past it.

### D6. How the run stops: exactly as the deadline does

The task said "exactly as the controller already ends a run", and that is what is built: `request_stop`, then the per-worker escalation in `watch()`, with the grace of the run (1 s/1 s null workload, 10 s/1 s verifying, 5 s/1 s under a schedule). **Open question for the owner:** a temperature emergency might deserve a shorter first grace (the verifying worker's 10 s is for a unit that cannot be interrupted, and a machine at its ceiling gets hotter for those seconds). I did not shorten it: a worker killed mid-unit is a worker whose unit is lost and whose fate says so, and the owner's decision on the trade (a hot machine for up to 11 s against a killed unit) is not mine. Measured in the demonstration above: 0.28 s from the crossing to the last exit, because a `sort` unit takes a fraction of a second at 6 MiB.

A crossing observed in the same tick as, or after, the deadline's stop is still recorded and still gives exit 7 (the machine did reach the ceiling while loaded), but asks nothing twice: `request_stop` skips a worker that has been asked. The round after the last worker is gone is not watched (nothing can be stopped).

### D7. Exit code 7, between 5 and 4

**Options.** Where "stopped by the temperature ceiling" goes in 5 > 4 > 3 > 6 > 0, and which number.

- *Below 6.* Wrong: a stopped run has cut its checks short and killed workers, so 3 and 6 would both apply and hide the cause.
- *Below 3, above 6.* Wrong for the same reason about 3 (the killed workers).
- *Above 3, below 4.* Hides the stop behind a failed write of the report about it.
- *Above 4, below 5 (chosen: 5 > 7 > 4 > 3 > 6 > 0).* The stop explains 3 and 6; it is a fact about the machine that a failed result write must not hide (4 outranks 3 today because a lost result is worse than a killed worker; here the stop is worse than the lost result, because the user acts on the stop: cooling, load, ceiling). A fault stays first because "it is the finding" (`outcome.hpp`): a wrong result found before the stop is the thing to read.
- *Above 5.* Considered. A run that found a wrong result and then got hot is exactly the run where the hardware finding matters most, and 5 already says "do not trust this machine"; 7 would say "it got hot", which the console says anyway.
- *Number.* 7 is the next free code; 1 to 6 are taken, and 120 to 125 and above are the workers' (`worker/entry.hpp`). The shell's 128+N are signals. `explore` keeps its own table: a trial the ceiling ends is 3 ("a trial could not be run"), because it stops the search.

`with_output_failure` keeps 7 as it keeps 5. A ceiling that cannot be read when the run starts is not 7: nothing started, it is 1 and the message says why. README, `--help`, `docs/architecture.md` §7 and `docs/safety.md` say it.

### D8. Where a stopped run says so in the result files: each worker's `termination`

**The question for the owner.** `results::RunResult` (`controller/results/result.hpp`, a contract) has no run-level field for why a run ended, and the writers' contracts fix what a document holds. A stopped run needs the sensor, the reading, the ceiling and the time in the result files, and the verdict is derived from the checks alone (`derive_verdict`; a stopped run with every check passed is `clean`).

**Options.** (a) A new field and schema version 4: a contract change, not mine. (b) A phase mark on the timeline: the writers do not print phases. (c) Put it in `WorkerResult::termination`, which every writer prints ("the raw fact behind the fate"): each worker's account of how it ended gains "; the run was ended by the temperature ceiling: <sensor> read <x> C, at or above the ceiling of <n> C, <t> s into the run". (d) Change the fate of the workers: they exited 0 on a stop request and are `completed`; calling them anything else would be false.

**Chosen: (c).** It is true of every worker (each was stopped because of it), it travels through both writers and the JSON/CSV tests without touching a contract, and it is one line to remove if the schema grows the field. It stretches "raw fact" to "the controller's account of how it ended". **Questions for the owner:** (1) add a run-level `ended_by` (stop reason, sensor, reading, ceiling, time) to the result schema at the next schema version; (2) whether the verdict of a stopped run should be `incomplete` and not `clean` — today the console prints `verdict: clean` and, directly under it, "ended by the temperature ceiling: ..."; the README and `docs/safety.md` state that the exit code and that line outrank the verdict. I did not change `derive_verdict` (VerdictContract).

### D9. The journal: the phase says which sensor and the ceiling, the temperature is the reading

`journal::Entry` has a phase label (64 bytes), a workload label and `temperature_c`, filled by `TimelineNotes` from `thermal.package` only. At the stop the controller appends an entry (forced) with phase `ceiling 90 C reached: thermal.package 0 (hwmon:coretemp)` (cut to the label limit by `mixed::bounded`) and `temperature_c` set to the reading that crossed, whichever sensor it was. Its time is the entry's `elapsed_ns`. The unwatched stop's phase is `ceiling unwatched 90 C` and has no reading. The last entry is still `done`, written after the reap. The contract is unchanged.

### D10. `explore`: honoured; a trial the ceiling ends fails the search

`explore --max-temperature C` gives every trial the ceiling. With no temperature in the capability report the command is refused before the first trial (exit 1, same words as `stress`). A trial a sensor ends comes back as `TrialFault::kRun` ("the trial was ended by the temperature ceiling: ..."), so the search stops with exit 3: a load cut short is not the load the candidate was asked to carry (its window would be short and its score wrong), and a machine at its ceiling is the wrong place to start the next candidate. The alternatives (score the cut-short trial, mark the candidate `incomplete`, skip to the next candidate) all produce a number or a fate from a run nobody wanted. Not honouring it and saying why was the other option; `explore` is the command whose job is to push temperature up, so it needs the ceiling more than `stress` does.

### D11. `--max-temperature` is an option of `stress --test` and `explore`; `stress --config` has the file's key

A second place to say it for `--config` would let the command line override the file in a direction (higher) that the file's author may not want. Range as in the configuration: 1 to 150, a whole number (`config::kMaxTemperatureLimitC`).

### D12. The banner names the sensors, grouped

`limit temperature: enforced (stops the run at 90 C; watching thermal.package 0, thermal.core x8)`: a family of more than one is `name xN`, a single sensor is named with its index. A machine with 64 cores and 64 zones would otherwise print 130 names. Sensors present but unreadable at the baseline are still watched (they may come back) and counted in `readable()`.

## Contract questions

1. **`WorkerResult::termination` carries the stop** (D8). The result schema needs a run-level reason to end.
2. **`derive_verdict` knows nothing of a stop** (D8): `clean` and exit 7.
3. **`Sampler::round` returns a count only** (D1). Everything works from the timeline; a future sampler that returned the readings would remove the read-back, and nothing requires it.

## Open

- The faster-first-grace question (D6).
- Whether a per-sensor exclusion is needed (D2); only real hardware can say.
- 34b (SIGINT/SIGTERM stop gracefully) is the other half of "emergency stop" and is the next roadmap item: a Ctrl-C still ends the run with no report.
- ARM64 was not run here (no machine); the CI legs run the new tests there. The code reads files and compares numbers; nothing in it is architecture-specific.
- The project still needs an outside reviewer.

## Gates

(Filled in at the end of the session; see the report.)
