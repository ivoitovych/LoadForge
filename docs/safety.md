# Safety

**Written:** 2026-10-22 22:09 UTC, against `main` at 97cf70e plus the release stream.

LoadForge is built to drive a machine to sustained full load on every core and a large
part of its memory, for as long as you ask. A machine with a failing fan, dried paste,
a blocked intake or an unstable overclock is the machine this is most likely to hurt,
and finding that out is the point of running it. This document says what the tool does
to protect the machine and the data on it, what it does not do, and what you should do
before and during a run. It states only what the code at this commit does; each claim
about behaviour was checked by running the binary, and the checks are shown.

Read the first section before your first run.

## 1. What is not protected: there is no temperature ceiling

**LoadForge 1.0 does not stop a run because the machine is too hot.**

A configuration may name `limits.max_temperature_c`. The value is read, checked
(1 to 150), and then reported as not enforced. Nothing watches it. Running the
configuration below on this commit:

```toml
schema_version = 1

[run]
mode     = "stress"
duration = "3s"

[limits]
max_temperature_c = 85
memory = "512MiB"

[[workload]]
name    = "sort"
threads = 2
memory  = "64MiB"
```

```
$ loadforge stress --config limits.toml
...
  limit duration: enforced (3s)
  limit memory: enforced (checked at start against a ceiling of 14005748KiB)
  limit temperature: NOT ENFORCED (not enforced until telemetry lands (M3); configured 85 C)
```

The message is out of date (telemetry landed with M3) but the fact it states is current:
the temperature sources are read and recorded on the timeline, and no part of the
controller compares them with a limit. `loadforge stress --test NAME` has no temperature
option at all. This is recorded as an open defect of the 1.0 candidate in
`docs/journal/34-release-1-0.md`. Until it is built:

- Watch the temperature yourself, with a tool of your own, on the first run on any
  machine, and keep the first run short (`--duration 2m`).
- Do not leave a run unattended on a machine whose cooling you have not seen work.
- A machine's own protection still applies: every CPU this tool targets throttles and
  then powers off at its junction limit. That protection is the platform's, not
  LoadForge's, and a run that reaches it ends with the machine off and no report.
  The crash journal (below) is what survives.

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

### Hangs

Each worker beats on a shared-memory slot every 100 ms and the controller reads it. A
worker whose beat is older than the staleness allowance (10 s for a verifying worker,
2 s for the null workload) is called hung, sent `SIGTERM`, and escalated to `SIGKILL` the
same way. Its fate is `timed out`; the run is not clean.

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
starts**. The budget is what the kernel says this process may use: `MemAvailable`, and
the memory limit that binds its cgroup (the lowest over the cgroup and its ancestors on
v2, `hierarchical_memory_limit` on v1). A configuration whose workloads add up to more
than the budget, or than `limits.memory`, is refused with exit code 1 and no worker is
started. If the budget cannot be read the run goes ahead and says
`limit memory: NOT ENFORCED (no memory budget could be read: ...)` with the reason.

The check is against a snapshot; it is not a watch. If something else on the machine
takes the memory during the run, the kernel's OOM killer decides, not LoadForge. When it
kills a worker, the controller reads the kernel's own OOM-kill count and reports
`killed by the kernel for memory` (fate `oom killed`), not a hardware fault, and the
run is not clean. Where there is no readable count the death is reported as a kill of
ambiguous origin.

Each worker's working set is 32 MiB by default (`--footprint`), and the runner locks it
in memory when the process is allowed to (`--lock try`, the default). A run that cannot
lock runs unlocked and says so on the worker's `working set` line; `--lock require`
refuses instead.

### What a run writes

Only what you ask for: `run.json` / `run.csv` (`--output`, into `--output-dir`; written
as `run.json.partial` and renamed, after a test file `.loadforge-write-test` has been
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
keeps it as `FILE.1`, `FILE.2` and so on. Measured, after a `SIGKILL` seven seconds in:

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
2. Know your limit: the sustained package temperature your CPU's datasheet gives, and a
   tool that shows it. LoadForge will show the temperature in its report when the
   machine exposes a sensor; the capability report at the top of every run says whether
   it does (`thermal: N of M present`).
3. Close what you need to keep. A power-off loses unsaved work.
4. On a laptop, plug it in. The fingerprint records `power_source` and `battery_state`
   when the kernel offers them, because a run on battery is a different experiment.
5. Start short: `loadforge stress --test dense_linear --duration 2m --journal run.journal`.
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

The verdict is one of `clean`, `corrected errors`, `incomplete`, `findings`. Only `clean`
exits 0. The exit codes are in the README and in `loadforge --help`.

## 7. What is not covered

- No thermal, power or fan control; no limit on package power.
- No watch of memory after the start (section 2).
- No graceful stop on a signal (section 2).
- SSDs, discrete GPUs and PCIe bandwidth are out of scope; the tool minimises writes to
  persistent storage, and writes only the files named in section 2.
- A virtual machine or container shows only what its host lets it see: sensors,
  counters and error counters are usually absent there, and the report says which.
  A run on such a machine proves the software, not the hardware under it.
