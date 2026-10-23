# Safety

**Written:** 2026-10-22 22:09 UTC, against `main` at 97cf70e plus the release stream.  
**Revised:** 2026-10-22 22:40 UTC, for the temperature ceiling (roadmap 34a).

LoadForge is built to drive a machine to sustained full load on every core and a large
part of its memory, for as long as you ask. A machine with a failing fan, dried paste,
a blocked intake or an unstable overclock is the machine this is most likely to hurt,
and finding that out is the point of running it. This document says what the tool does
to protect the machine and the data on it, what it does not do, and what you should do
before and during a run. It states only what the code at this commit does; each claim
about behaviour was checked by running the binary, and the checks are shown.

Read the first section before your first run.

## 1. Heat: the ceiling is yours to set

**LoadForge stops a run because the machine is too hot only if you give it a ceiling.**
There is no default. A configuration names `limits.max_temperature_c` (1 to 150, degrees
Celsius), and `stress --test` and `explore` take `--max-temperature C`. With none, nothing
watches the temperature and a run goes on at full load until its duration, whatever the
sensors say.

- Choose the ceiling from your CPU's datasheet: a few degrees under the sustained junction
  limit, and under what you are willing to see on a machine you want to keep. The tool
  does not know your cooling.
- The ceiling is a software backstop behind the CPU's own throttling and thermal shutdown,
  never a substitute for them. A machine that throttles or powers off before the sampler
  sees the ceiling ends the run without a report, and the crash journal (section 3) is what
  survives.
- Between the reading that crosses the ceiling and the last worker stopping there is time:
  up to one sample interval (1 s by default, `--sample-interval`) before the reading is
  taken, then the stop (section 2). The measured case is in section 2; plan for seconds,
  not milliseconds.
- On a machine with no temperature sensor to read (a virtual machine, a container, a board
  without a thermal driver) a run with a ceiling is **refused**, not run unprotected
  (section 2). Remove the ceiling to run there; nothing inside such a machine can see its
  temperature.

Watch the temperature yourself on the first run on any machine, keep the first run short
(`--duration 2m`), and do not leave a run unattended on a machine whose cooling you have not
seen work.

## 2. What is enforced

### The duration

`run.duration` (or `--duration`) is the time the workers run, counted from the moment
the last of them has started. At the deadline the controller asks every worker to stop,
and a worker answers between two units of work. Stopping then escalates, bounded:

| Step | When | For a verifying worker |
|---|---|---|
| stop request on the worker's control pipe | at the deadline | waits up to 10 s for the worker to finish its unit |
| `SIGTERM` | after the stop grace | waits 1 s |
| `SIGKILL` | after that | cannot be ignored |

A run can therefore end about 11 s after its duration on a machine where a unit takes
very long, and ordinarily ends within a fraction of a second of it (the two-second run
in section 6 ended 0.24 s late). A worker running the null workload (a configured name
that is not a workload of this build) is given 1 s for each step instead of 10 s.

### The temperature ceiling

`limits.max_temperature_c` is enforced from the telemetry the controller already takes. After
every sampling round it looks at the point each temperature series received: every gauge in
degrees Celsius that the machine exposes, which today means `thermal.package`,
`thermal.core`, `thermal.sensor` and `thermal.zone` (`docs/telemetry-sources.md`). It does not
choose "the" temperature, because nothing can know which sensor is the package on every
board: **any one sensor at or above the ceiling stops the run**, a chipset or VRM sensor as
well as the die. A reading equal to the ceiling counts. The sensors it watches are in the
banner:

```
  limit temperature: enforced (stops the run at 90 C; watching thermal.package 0)
```

(`thermal.core x8` when a family has several.) What happens when one reads at or above the
ceiling, measured on this build with a fake coretemp chip that went from 45 C to 93 C three
seconds into a 60 s run of two `sort` workers:

```
[3.172 s] temperature ceiling: thermal.package 0 (hwmon:coretemp) read 93.0 C, at or above the ceiling of 90 C, 3.172 s into the run: stop requested
[3.420 s] worker 1 (pid 17724): exited with code 0
[3.452 s] worker 0 (pid 17723): exited with code 0
run ended after 3.552 s: 2 of 2 worker(s) completed
  ended by the temperature ceiling: thermal.package 0 (hwmon:coretemp) read 93.0 C, at or above the ceiling of 90 C, 3.172 s into the run
...
verdict: clean
ended by the temperature ceiling: thermal.package 0 (hwmon:coretemp) read 93.0 C, at or above the ceiling of 90 C, 3.172 s into the run
...
$ echo $?
7
```

- Every worker is stopped exactly as at the deadline: a stop request, then `SIGTERM` after
  the stop grace, then `SIGKILL` (the table above). A worker answers between two units, so a
  verifying worker can take up to 10 s to answer and the run up to about 11 s to end; the
  ordinary case is a fraction of a second, as above. The ceiling does not shorten the
  escalation: a worker killed in the middle of a unit is a worker that did not finish, and
  the exit code says why (below).
- The run ends with **exit code 7**, which outranks a failed write (4), a worker that did not
  complete (3) and a check not made (6), because the stop is why they happened. Only a fault
  (5) outranks it: a wrong result found before the stop is still the thing to read first.
  The `verdict:` line is the checks' and says nothing of the stop (a stopped run can be
  `clean` and exit 7); the line under it does, and so does the exit code.
- The console says which sensor, what it read, the ceiling and when (above, twice: when it
  happened and in the summary). The result files say it in every worker's `termination`
  (the result schema has no field for a reason to end, so each worker's account of how it
  ended carries it). The crash journal, when one is open, has an entry at the stop whose
  phase is `ceiling 90 C reached: thermal.package 0 (hwmon:coretemp)` and whose temperature
  is the reading:

```
$ loadforge journal j
journal j: ended cleanly; 3 entries
  t=0.0s phase "run" workload sort 45.0C workers 2 alive 0 stale
  t=3.2s phase "ceiling 90 C reached: thermal.package 0 (hwmon:coretemp)" workload sort units 8 93.0C workers 2 alive 0 stale
  t=3.6s phase "done" workload sort units 9 93.0C workers 0 alive 0 stale
```

**A ceiling nothing can hold is a refusal.** If no temperature can be read before the first
worker starts (no sensor on the machine, every sensor unreadable, no telemetry), the run is
refused with exit code 1 and no worker is started. The message carries the thermal source's
own reason and remedy:

```
loadforge: temperature: the temperature ceiling is 85 C and no temperature can be read, so it could not be enforced: thermal.package: no hwmon chip with a temperature input and no thermal zone; this machine exposes no temperature sensor to read; in a virtual machine or a container that is the platform's choice and nothing inside it can change that, while on bare metal the platform's hwmon driver (coretemp on Intel, k10temp on AMD) may not be loaded; remove the ceiling to run without one
$ echo $?
1
```

This differs from the memory limit, which runs and says NOT ENFORCED when the budget cannot
be read. The reason is what the failure costs: an unread memory budget leaves the kernel's
OOM killer in charge, which ends one worker and says so; an unread temperature leaves the
machine at full load with nothing to stop it, and a banner line is easy to miss in a
capability report of two dozen lines. There is no option to run on with a ceiling that
cannot be held: removing the ceiling from the configuration (or leaving out
`--max-temperature`) is that option, and it is explicit. A machine already at or above the
ceiling before the first worker starts is refused the same way (exit 1): the run would end
at its first tick.

**Sensors that go during a run.** A sensor that stops answering is told once on the console
(`temperature: thermal.core 3 (hwmon:coretemp) can no longer be read: ...`, and again when it
answers) and the others are still watched. If **no** sensor has answered for 10 s, or if
sampling itself ends (the timeline refused a round), the ceiling can no longer be enforced
and the run is stopped like a crossing, exit code 7, with the reason:

```
[13.210 s] temperature ceiling: no temperature could be read for 10.3 s (open(/sys/class/hwmon/hwmon0/temp1_input): No such file or directory (errno 2)), so the ceiling of 90 C can no longer be enforced, 13.210 s into the run: stop requested
```

(A run that stops at its crossing is 7; so is one stopped because nothing could hold the
ceiling; the text says which.) Only the rounds taken while the workers run are watched; the
round after the last worker is gone is not.

`explore` honours `--max-temperature` in every trial. With no sensor it is refused before the
first trial (exit 1). A trial in which a sensor reaches the ceiling is stopped as above and
**fails the search** (exit 3, naming the sensor): a load cut short is not the load the
candidate was asked to carry, so it has no score, and the machine is hot, so the search does
not go on to the next candidate.

### Hangs

Each worker beats on a shared-memory slot every 100 ms and the controller reads it. A
worker whose beat is older than the staleness allowance (10 s for a verifying worker,
2 s for the null workload) is called hung, sent `SIGTERM`, and escalated to `SIGKILL` the
same way. Its fate is `timed out`; the run is not clean. A worker that has been told to
switch workload or to rebind is given 60 s more, counted from the write of the command until
the worker answers it, because it cannot beat while it frees and makes its working set; past
those 60 s the usual allowance judges it again, so a replacement that never ends is still
stopped.

### The controller dying

A worker asks the kernel to deliver a signal to it if its parent dies
(`PR_SET_PDEATHSIG`; the signal is `SIGKILL`), and checks after that call that its parent is still the process
that started it, because the parent may have died in between. A worker that finds its
control pipe closed, or its record pipe without a reader, leaves with its own exit code
for a lost controller. Measured: a controller sent `SIGKILL` seven seconds into
a run left no worker behind.

```
$ loadforge stress --test sort --duration 60s --journal j --output json --output-dir out &
$ kill -KILL $!        # after 7 s
$ pgrep -x loadforge | wc -l
0
```

### Ctrl-C and `SIGTERM`

The controller installs no handler for `SIGINT` or `SIGTERM`. They end it at once with
the shell's usual status (130 and 143 here), its workers go with it by the same
parent-death signal, and **no report and no result file is written** for that run. If
you want a report, let the run reach its duration. Measured:

```
$ loadforge stress --test sort --duration 60s & kill -INT $!   # after 3 s
INT rc=130
$ loadforge stress --test sort --duration 60s & kill -TERM $!
TERM rc=143
$ pgrep -x loadforge | wc -l
0
```

A graceful early stop is not built; it is a candidate for a later release.

### Memory

`limits.memory` and each workload's `memory` are checked **once, before any worker
starts**, for the largest total any step of a schedule can hold (for `mixed` and `cycling`
the footprints change during the run, and the refusal names the step that is too large). The budget is what the kernel says this process may use: `MemAvailable`, and
the memory limit that binds its cgroup (the lowest over the cgroup and its ancestors on
v2, `hierarchical_memory_limit` on v1). A configuration whose workloads add up to more
than the budget, or than `limits.memory`, is refused with exit code 1 and no worker is
started. If the budget cannot be read the run goes ahead and says
`limit memory: NOT ENFORCED (no memory budget could be read: ...)` with the reason.

The check is against a snapshot; it is not a watch. If something else on the machine
takes the memory during the run, the kernel's OOM killer decides, not LoadForge. When it
kills a worker, the controller reads the kernel's own OOM-kill count and reports
`killed by the OOM killer` (fate `oom killed`), not a hardware fault, and the
run is not clean. Where there is no readable count the death is reported as a kill of
ambiguous origin.

Each worker's working set is 32 MiB by default for a single workload (`--footprint`); in
`mixed` a dataset may be half of it. The default footprint and `--workers` are not
checked against memory: the check runs against what the configuration or `--footprint`
states, so `--workers 8` is your own limit. The runner locks the working set
in memory when the process is allowed to (`--lock try`, the default). A run that cannot
lock runs unlocked and says so on the worker's `working set` line; `--lock require`
refuses instead.

### What a run writes

Only what you ask for: `run.json` / `run.csv` (`--output`, into `--output-dir`; written
as `run.json.partial`, fsynced, renamed, and the directory fsynced, after a test file `.loadforge-write-test` has been
created and removed in that directory), and the crash journal (`--journal FILE`).
Everything else is read: `/proc`, `/sys`, and `perf_event_open` for counters. The tool does not write to sysfs, does not touch model
specific registers, does not load modules and does not change governors, fan curves or
power limits. The one register it writes is the calling thread's own floating-point
control register, which it sets and asserts so that results are reproducible.
A search of `src/` for files opened for writing finds exactly those two writers.

If the result directory is not there the run is refused before it starts (exit 1,
`cannot write the result: "/nonexistent/x" is not a directory`). A result that cannot
be written after the run is exit 4.

## 3. The crash journal

`--journal FILE` writes a small record every few seconds, each synced to disk. If the
machine loses power, or the controller is killed, the journal says how far the run got.
At the next start with the same file, the tool reports how the previous run ended and
keeps it as `FILE.1`, `FILE.2` and so on. A file at that path that is not a journal is
refused (exit 1) and left where it is, not renamed; so is a journal that another run holds
open (a run takes an exclusive lock on the file). The end mark ("ended cleanly") is written
when the run returned a report and means only that; a run the controller failed (a refused
start, an error in the loop) leaves no end mark and its journal replays as ended abnormally. Measured, after a `SIGKILL` seven seconds in:

```
$ loadforge journal j
journal j: ended abnormally between records; 2 entries
  t=0.0s phase "run" workload sort workers 4 alive 0 stale
  t=5.0s phase "run" workload sort units 12 workers 4 alive 0 stale
$ loadforge stress --test sort --duration 1s --journal j
previous journal j: ended abnormally between records; 2 entries; the last: t=5.0s phase "run" workload sort units 12 workers 4 alive 0 stale; kept as j.1
```

Use `--journal` on the first run of every unattended or long job: for a run that ends in
a power-off, it is the only evidence.

## 4. A wrong result is a finding, not a crash

A worker that dies by `SIGSEGV`, `SIGBUS` and the other fault signals is reported as a
fault of the process; the signal does not say why. A worker that computes a wrong answer
keeps running, and the answer is reported: the unit, the workload, the input, the
expected and observed bytes, and the core. LoadForge then repeats that unit on the same
core, on another core, and on one thread, and says what the three results are consistent
with. It never says "the CPU is bad": the report is evidence, and a hardware verdict is
yours to make. Exit code 5 means a fault was found. See `docs/verification.md` §7.

Corrected memory errors (EDAC) reported by the hardware during a run also make the
run's verdict `corrected errors` and the exit code 5: a run that had them is not clean.
A hardware error counter that was blind for part of the run makes the verdict
`incomplete` (exit 6).

## 5. Before you run

1. Look at the machine: fans turning, vents clear, the case closed, laptop on a hard
   surface and not on a bed.
2. Know your limit: the sustained package temperature your CPU's datasheet gives, and set
   a ceiling a few degrees under it (`limits.max_temperature_c`, or `--max-temperature`).
   The capability report at the top of every run says whether the machine exposes a
   sensor to hold it (`thermal: N of M present`); if it says 0, a run with a ceiling is
   refused (section 2) and a run without one is on your watch alone.
3. Close what you need to keep. A power-off loses unsaved work.
4. On a laptop, plug it in. The fingerprint records `power_source` and `battery_state`
   when the kernel offers them, because a run on battery is a different experiment.
5. Start short, with a ceiling:
   `loadforge stress --test dense_linear --duration 2m --max-temperature 85 --journal run.journal`.
6. On shared machines, remember that one worker is started per CPU you may use (at most
   64), pinned to its own CPU: the machine will be unusable for other work while it runs.
   `--workers N` starts fewer.
7. Do not run it on a machine you cannot afford to lose to a failing component. Finding
   such a component is a success of the tool, and a harder one for the machine.

## 6. What a clean end looks like

```
$ loadforge stress --test sort --duration 2s
...
[2.017 s] deadline reached: stop requested
[2.032 s] worker 2 (pid 7454): exited with code 0
...
run ended after 2.238 s: 4 of 4 worker(s) completed
...
verdict: clean
$ echo $?
0
```

The verdict is one of `clean`, `corrected errors`, `incomplete`, `findings`. A run with a
verified workload exits 0 only when the verdict is `clean`; a run of unverified (`null`)
workloads can end `incomplete` and still exit 0. The exit codes are in the README and in `loadforge --help`.

## 7. What is not covered

- No default ceiling: a run with none is not stopped for heat (section 1).
- No thermal, power or fan control; no limit on package power. The ceiling stops a run; it
  does not throttle one.
- No watch of memory after the start (section 2).
- No graceful stop on a signal (section 2).
- SSDs, discrete GPUs and PCIe bandwidth are out of scope; the tool minimises writes to
  persistent storage, and writes only the files named in section 2.
- A virtual machine or container shows only what its host lets it see: sensors,
  counters and error counters are usually absent there, and the report says which.
  A run on such a machine proves the software, not the hardware under it.
