# Architecture

**Written:** 2026-10-22 22:09 UTC, against `main` at 97cf70e plus the release stream.

How LoadForge is put together, as the tree at this commit has it. This document describes
what is built. The reasons behind the choices, and the alternatives rejected, are in
`docs/PLAN.md` (the design of record), `docs/JOURNAL.md` (decisions and facts found by
experiment) and `docs/adr/`; the pointers below say where. Each section names the
directory in `src/` it is about.

## 1. One binary, two kinds of process

`loadforge` is one executable. A run uses it as a **controller**, which supervises, and as
several **workers**, which compute. The controller starts each worker with `fork` and then
`exec` of `/proc/self/exe` and an internal switch (`--worker`); a second internal switch
(`--rerun`) runs one unit again for fault isolation. Neither switch is in `--help`.

```
 loadforge stress | explore | journal | --check-config      (src/main)
 ┌────────────────────────── CONTROLLER (one thread) ──────────────────────────┐
 │ configuration (src/config)     topology snapshot, once (src/topology)       │
 │ plan: workload, footprint, CPUs, ISA path per worker (src/controller)       │
 │ supervision: beats, deadline, stop → SIGTERM → SIGKILL, fate of each worker │
 │ sampler → timeline (src/telemetry)    crash journal (src/controller/journal)│
 │ isolation ladder (src/controller, src/verification/isolation)               │
 │ results: statistics → document → JSON / CSV writers; console reporter       │
 └────────┬───────────────────────────────────────────────────────┬────────────┘
          │ heartbeat region (shared memory, no lock)             │ fork + exec
          │ control pipe (stop, rotate, move …)                   ▼
          │ record pipe (checksummed frames) ◄──────────  WORKER × N (own process,
          ▼                                               own session, pinned CPUs)
   workers beat, send records,            executor threads → workload.compute()
   and obey the control pipe              input-integrity bracket + oracle
```

**Why processes.** The most valuable output of a reliability tool is the evidence of the
seconds before a fault. A fault that kills a worker must not kill the timeline. Separate
address spaces also give per-worker attribution, let an out-of-memory kill land on one
worker and be told from a hardware fault, and give every worker its own mapping of memory
to verify. The controller is single-threaded; threads exist only inside a worker, in the
executor (`PLAN.md` §4.6; `JOURNAL.md` §2.1).

**Why `fork` and `exec`.** A forked child inherits the parent's dirtied heap, which is the
wrong start for a tool that verifies memory; `exec` also gives every worker a fresh
address-space layout. The path comes from `/proc/self/exe`, never `argv[0]`. Without
`/proc` there is no fallback: the run is refused.

**Sessions.** Each worker leads a session of its own. When the controller reaps it, it
kills the worker's group, so nothing a worker forked outlives it (`JOURNAL.md` §2.20).

## 2. A run, in order

`loadforge stress --test sort --duration 2s`, as the code does it:

1. **Parse** the arguments (`src/main/cli.cpp`). A mistake is exit code 2 and names the
   argument. A configuration file (`stress --config`) is read by `src/config` (TOML, a
   vendored header-only parser; an unknown key is a refusal, not an ignored typo).
2. **Describe the machine** (`src/topology`): logical CPUs, cores, packages, caches, NUMA
   nodes, from `/sys`. The snapshot is taken **once, before the first fork**, is immutable
   and is shared: both processes need it, and two runs on one machine cannot disagree about
   what the machine is. Sources that contradict each other are a refusal.
3. **Read the memory budget** (`src/resources`): `MemAvailable` and the binding cgroup
   limit. The kernel's OOM-kill count is read before the first fork, to attribute a
   `SIGKILL` death afterwards.
4. **Probe the telemetry** (`src/telemetry/capability`, `docs/telemetry-sources.md`) and
   print the capability report. With a temperature ceiling configured, the baseline round
   (taken before the first fork) must give at least one temperature, and none may already
   be at the ceiling; otherwise the run is refused here, with the thermal source's reason.
5. **Plan each worker** (`src/controller/plan`): workload, its footprint, the CPUs and
   thread count, the ISA path (the best that this CPU, this build and this workload all
   have, or the one asked for, or a refusal; never a silent substitute), the duty cycle,
   and a seed (`seed + slot × a fixed odd stride`). Every choice is made once, here, and
   handed to the worker as arguments, so the workers, the re-runs and the report argue
   from the same numbers. The banner prints it.
6. **Start the workers**, one at a time, beating between forks so that a slow start is not
   mistaken for a hang. The duration counts from the moment the last one has started.
7. **Run**: each tick the controller reads every worker's beat, drains their record pipes,
   takes a telemetry round on its schedule and writes the journal. It judges each worker
   by its beat (alive, stale, torn) and its fate (below). When a temperature ceiling is
   configured it then looks at the point every temperature series received in that round
   (`src/controller/temperature`), reading back the timeline the sampler wrote and never
   the sensors themselves; a sensor at or above the ceiling ends the run as the deadline
   does (step 8), with its own exit code.
8. **Stop** at the deadline, when the temperature ceiling is reached, or when `SIGINT` or
   `SIGTERM` arrives: a stop request on
   the control pipe, then `SIGTERM`, then `SIGKILL`, each after its grace
   (`docs/safety.md` §2). It then reaps every worker and
   takes a last telemetry round. The two signals are caught for the run only
   (`src/controller/stop_signals`): the handler writes the signal's number to a pipe that the
   tick's poll waits on with the workers' record pipes, so the controller learns of a Ctrl-C
   in its own code, at once. A second signal while the workers are stopping skips the grace
   and sends `SIGKILL`. The workers lead sessions of their own, so the terminal's Ctrl-C
   reaches the controller only.
9. **Isolate** each wrong result found (§5), if any.
10. **Report**: the console, and `run.json` / `run.csv` if asked. The exit code is derived
    from the verdict and the fates (§7).

## 3. Inside a worker

A worker (`src/worker`) beats before anything else, then sets up in this order, and any
refusal is its own setup failure (exit 119):

1. the floating-point environment is established and asserted, before any thread exists
   (the executor's threads assert that they inherited it; `docs/determinism.md`);
2. the ISA path the controller chose is checked against what this CPU can run and this
   build holds, and used or refused;
3. the workload, then its oracle for the plan the workload resolved, then the executor,
   then the runner;
4. the environment is asserted again, after all of that: a library that changed a
   rounding mode is a configuration error, reported, not repaired.

Then it ticks until the duration or a stop. A tick: publish the beat (with how many units
it has computed, so "busy" is told from "stuck"), look at the duration, judge the
controller, **look at** the control pipe without waiting on it, then run a slice of units.
A slice is sized so that a tick lasts about half a heartbeat interval. A worker is only
ever asked to stop between units; a unit has no upper bound, which is why the stop grace
is 10 s for a verifying run.

**Units.** A workload (`src/workloads/<name>`) is a family of independent units. Unit
`i`'s input is a pure function of `(seed, i)` (a counter-based generator, Philox); its
output is a pure function of the input and the configuration. Units share nothing, so any
unit can be regenerated anywhere from two integers, which is what the integrity bracket
and the re-runs need. A workload allocates nothing, starts no thread, reads no clock and
makes no system call: the runner owns memory, placement, slicing and verification.

**Executor** (`src/worker/executor`): a thread pool placed by index, one thread per
placement, each pinned to its CPU. The thread that calls it runs no work.

## 4. Verification: nothing is checked by running it again

For each unit `k` in a slot of the working set (`src/verification`, contract in
`runner.hpp`):

1. **write** the input from `(seed, k)`;
2. **open the bracket**: regenerate the input in a separate buffer and compare it with
   what is in memory. A difference is corruption *before consumption*, and the unit stops
   there: its input is already known wrong;
3. **compute** the output;
4. **check** the output with the workload's **oracle**, an independent second
   implementation derived from the mathematical definition (an LZ4 decoder written
   separately from the compressor; a residual and invariants for dense linear algebra and
   the other bounded workloads). Agreement is tallied; a mismatch becomes a record;
5. **close the bracket**: regenerate and compare again. A difference is corruption *during
   consumption*.

A unit's input stays verifiable until every result derived from it has been checked.
Which units are bracketed or sampled is a rule (`k mod period`), not a random draw, so a
run's tally is predictable and a re-run checks the same units. A check that could not be
made is its own outcome, `VerificationNotPerformed`, never a pass and never a failure.

The workloads built are: `adaptive_mesh`, `compression`, `dense_linear`, `fft`, `graph`,
`monte_carlo`, `particles`, `raytrace`, `sort`, `sparse`, `stencil`. Their determinism
class (bit-exact, bit-exact for a fixed decomposition, or bounded residual) decides how
the oracle compares; the classes are in `docs/determinism.md` and the doctrine in
`docs/verification.md`. Two runs are made of a **schedule** rather than one workload
(`src/workloads/mixed`): `mixed`, in which workloads rotate across the workers and the
workers across the CPUs while every unit goes on being verified, and `cycling`, thermal
and power cycling by level of duty or by number of working cores. A schedule is data made
before the run from its options and a seed; it speaks only the commands of the control
pipe; it never stops a worker, the run's end is the controller's deadline.

**Vector paths.** The ISA probe (`src/platform/isa`) says what the CPU can run; the paths
are scalar, AVX2 and NEON (AVX-512 is probed and has no kernel). A scalar build is a
required build: it proves no intrinsic leaked into portable code, and the scalar path is
the one that is held bit-identical on x86-64 and ARM64.

## 5. When something is wrong

Two different things can go wrong, and the tool keeps them apart.

**A worker dies.** The controller classifies how (`src/controller/supervision`), from
evidence rather than a guess: completed; worker error (its own non-zero exit); startup
failed; controller lost (the worker's view); fault signal; killed; killed by the OOM
killer (the kernel's OOM-kill count accounts for it); stopped by the controller; timed out
(its
beat went stale); record channel corrupt (a frame that did not verify, reported over everything
else: garbage in must never become a verdict out); evidence truncated; unrecognised. A
fault signal says that the process died and never why. A `kill(2)` that succeeded proves
nothing about the cause, because the kernel accepts a signal to a zombie
(`JOURNAL.md` §1.27).

**A worker computes a wrong result.** It keeps running and sends a record: an exact bit
corruption (with the bytes, the word offset, the cache line and the core) or a numerical
verification failure (with the residual and the derived bound). Each carries provenance:
the workload and its version, the unit, the seed, the ISA path, the decomposition and the
footprint, enough to regenerate the unit anywhere.

**The isolation ladder** (`src/verification/isolation`, `src/controller/rerun`) then
re-runs the unit in a worker process of its own: on the same core (clean means a
non-reproducible transient); on a different physical core with the same threads; on the
same core with one thread. What it finds is *consistent with* a cause, with a confidence,
and is never a verdict on the hardware. A wrong result enters the ladder; a check that
could not be made does not.

## 6. Telemetry, results and the journal

**Telemetry** (`src/telemetry`; `docs/telemetry-sources.md`). Seven sources, a capability
probe, a round sampler and a bounded timeline on `CLOCK_MONOTONIC`, the clock the
heartbeats use. Absence is first-class and carries its reason and remedy.

**Statistics** (`src/statistics`) summarise each series over the load window with
compensated sums; a counter's wrap is advanced across and its reset is a gap.

**Results** (`src/controller/results`, `src/reporters`; `docs/result-schema.md`). One
document tree is built from the run and rendered by each reporter, so JSON, CSV and the
console cannot disagree about what was found. It holds the fingerprint of the machine and
the run, the capabilities, the signature (series summaries), each workload's tally, each
finding with its record and isolation report, and each worker's fate. Absence is `null`
with its reason beside it. The verdict is one word: `clean`, `corrected errors`,
`incomplete` or `findings`.

**Crash journal** (`src/controller/journal`). A small record, appended and synced every
few seconds when `--journal FILE` is given, with a length and a CRC-32 per record. A torn
tail is reported, not repaired; the end mark is written when the run returned a report and
means only that (a run the controller failed leaves none); a file that is not a journal,
or a journal another run holds, is refused; `loadforge journal FILE` prints it in words
and says how
the run ended. The next start with the same file keeps the previous one as `FILE.1`.

**Explore** (`src/explore`, `src/main/explore_command`, `src/main/trial_runner`). A search
over a curated list of candidates (workloads, thread counts, CPU lists) for the worst
case of one objective (a metric and a statistic). A *trial* is one controller run with a
telemetry rig of its own. Candidates are compared by successive halving; an interleaved
reference measures drift, which is subtracted and whose noise is added to the interval;
every claim carries its replicate count, standard deviation and 95% interval (the winner's
claim is from replicates run after it was chosen); the report also says which trials did
not settle, which had corrected hardware errors, and the rival the ranking is against. The
search
never uses a metric the machine cannot read: that is exit code 1 before anything starts.

## 7. Exit codes

`0` clean; `1` refused (the configuration, or the run could not start); `2` usage; `3` a
worker did not complete; `4` a result file could not be written; `5` a fault: a wrong
result, an uncorrected error, or corrected errors; `6` a check was not performed (a run
with a verified workload that checked nothing is not clean; a run of unverified workloads
can end `incomplete` and exit 0); `7` the temperature ceiling ended the run; `130` or `143`
`SIGINT` or `SIGTERM` stopped the run (the shell's 128 plus the signal; the run was
reported and written as any other). When several apply, the first of 5, 7, 4, 130 or 143,
3, 6 is the code (`README.md` says why 7 and the signals sit where they do). Explore uses
0, 1, 2, 3, 5, 6, 130 and 143: 3 a trial that could not be run (one the ceiling ended
included); 5 a finding in a candidate's or the reference's trial, or a corrected hardware
error in a scored trial; 6 a candidate's or the reference's trial that did not finish, or
one with no reading of the objective; 130 or 143 a search a signal stopped, which reports
what it measured (above 6, below 5).
`loadforge journal FILE` exits 0 whatever the ending and 1 for no journal.

## 8. The platform boundary and how it is tested

Everything that touches the kernel goes through one interface, `platform::Syscalls`
(`src/platform`), with a real implementation and a substitutable fake. Nothing above the
seam calls a system call directly; a link error is the answer if one does. That is what
makes every `errno` of every call site an injectable test and not a hope. The only
modules that read `/sys` and `/proc` do so through `platform::FileSystem`.

Linux only. x86-64 and ARM64 are both first-class, because ARM64 is weakly ordered where
x86-64 is total-store-ordered, and a memory-ordering bug that x86 hides shows on ARM.
Every atomic operation states its memory order; there is no process-shared lock anywhere
(heartbeat slots are single-writer seqlocks, records travel in pipes).

**Tree.** `src/` holds one directory per module; each of the newer ones owns a
`CMakeLists.txt` that is found by glob (`cmake/Components.cmake`, ADR 0004). `tests/`
holds the tiers (`unit`, `oracle`, `property`, `fault_injection`, `determinism`,
`supervision`, `integration`, `soak`, `hardware`) and `tests/obligations.d/`, the ledger
of what each module owes; `tools/check-test-obligations.py` refuses a module that has
not discharged it. Components share contracts (the headers marked `CONTRACT`), never
tests or mocks (ADR 0003).

## 9. Where to read more

| Question | Document |
|---|---|
| Why is it built this way | `docs/PLAN.md`, `docs/adr/` |
| What was found by experiment | `docs/JOURNAL.md`, `docs/journal/` |
| How a result is checked | `docs/verification.md`, `docs/determinism.md` |
| What a result file holds | `docs/result-schema.md` |
| What the machine must offer | `docs/platforms.md`, `docs/telemetry-sources.md` |
| What is and is not protected | `docs/safety.md` |
| Running it on real hardware | `docs/hardware-validation.md` |
| Building and installing a release | `packaging/README.md` |
