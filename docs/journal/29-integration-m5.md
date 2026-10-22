# Journal fragment 29 — The M5 integration: the mixed workload, cycling, worker control, the crash journal

**Written:** 2026-10-21 22:47 UTC
**Stream:** 29, `feature/integration-m5` (`docs/streams/29-integration-m5.md`)
**Modules:** `src/workloads/mixed`, `src/worker/control`, and the M1 modules the brief gives
(`src/worker`, `src/controller`, `src/main`).

## Established by experiment

### Every control message is answered by every worker, and a refusal leaves the worker working

Measured by `tests/supervision/control_test.cpp` against the shipped binary, two real
workers, 2026-10-21 22:47 UTC and earlier. A worker told to pause for a fifth of a 1.9 s run computes fewer
units than its neighbour; one given a duty cycle of 150 per mille over a 100 ms period
computes fewer than its neighbour; a Rebind to the last CPU and back, and a Switch from
sort to compression, are Done and the run stays clean (no wrong result, nothing not
performed). A Rebind to CPU 1000 and a Switch to a name no registry holds are both
refused with the reason in the worker's story, and the worker goes on computing. The
controller's own Stop is counted in `sent` when it is sent: a worker that is paused at
the deadline answers it Done, one that has already left on its own duration never reads
it, and one already reaped is sent nothing (below).

### A killed controller leaves a journal that replays, and no worker outlives it

`tests/supervision/journal_kill_test.cpp`: the shipped binary runs `stress --test mixed
--rotation 1s --journal FILE`, is sent SIGKILL 4.5 s in, and the file replays in this
process as `Unterminated` (or `TornTail`) with at least three entries, in order, the
first with both workers alive and the last with verified units. `loadforge journal FILE`
says "abnormally". The process group the run was spawned in is empty within ten seconds.

### A rotated worker grows nothing

`tests/soak/workloads/mixed`: the real worker loop in a thread, rotated every 250 ms by
the commands of a mixed schedule for 20 s by default (LOADFORGE_SOAK_SECONDS; the nightly
job sets two hours). Every acknowledgement is Done, the tally never runs backwards, no
finding of any kind is made, and the resident set, the descriptors and the threads end
where they began. A five-hour schedule of 64 workers with four placements is made whole in
under two seconds: 1799 steps at a rotation of ten seconds, all phase labels different, and
the same digest twice.

### The schedule in C++ and the schedule in Python agree

`tests/fixtures/workloads/mixed/generate.py` is a second implementation of the algorithm
(written from the header's rules, not from the C++) that also computes the digest. Six
listings and their digests are committed; the C++ produced the same listing and digest for
all six on the first run of the oracle and determinism tests.

### A rotation faster than the timeline's clock cannot be marked

`stress --test mixed --rotation 1ms` for seven seconds runs, and the timeline refuses
some of the phase marks ("phase r2 memory-heavy refused: out of order"): several steps fall
in one millisecond and the timeline wants strictly later marks. The run is unaffected, as
the controller says; it is a limit of the timeline, not found by a test of its own. The
step count (`kMaxSteps`, 100000) is what bounds a rotation that is too short; shorter
rotations than a timeline tick are legal but their phases are not all marked.

### A worker usually leaves on its own duration before it reads the controller's Stop

The worker's clock starts after exec; the controller's deadline is measured from before the
fork, so the Stop is usually written first, but a worker busy in a slice reads it at the
next tick and a worker that has already counted its duration out leaves without reading it.
Both are clean. So a Stop is excluded from "unanswered" (the count of messages with no answer
at the end), and counted in "sent" when it is written. It is not always written: on a loaded
machine the controller's tick can come after the worker has left and been reaped, and
`request_stop` sends nothing to a reaped worker. With six CPU hogs beside it, the pause test
lost the Stop from both workers' `sent` in 2 runs of 12 (2026-10-22 11:40 UTC); the tests
now admit the Stop or its absence in `sent`, as they already did in `done`.

### A worker whose records nobody reads blocks in write() and never ends

Found on CI 2026-10-22 14:53 UTC, once a hung test was stopped after 900 s (#96): the
mixed soak and `RotationFaults.ACheckThatCannotBeMadeAfterASwitchIsNotPerformedNeverClean`
ran the worker loop in a thread and read its records pipe only after joining it. A pipe
holds 64 KiB; a machine that runs units fast enough writes more than that, the worker
blocks in `write()`, and the join never returns. Here, a soak of 60 s hung (its worker
thread in `anon_pipe_write`, the test thread in the join) where 20 s passed, and the
nightly soak runs for two hours. Both tests now read the records in a second thread
while the worker runs (the soak counts them as they come and keeps none); with the
fault test's pipe cut to 4 KiB both of its tests pass, and the 60 s soak passes in 60 s.

## Decisions

### The control pipe carries frames, the acks come back as frames, the stop byte is gone

Options: a new pipe for acks; the old stop byte next to the new messages; framed
messages both ways. Chosen: the existing control pipe carries `ipc` frames of type 32
(`kControlFrameType`) and acks return on the record pipe as frames of type 33, so no
descriptor is added and the record reader's checks (length, checksum) are the ones the
control channel gets too. A truncated frame at end of file is the controller hanging up;
any other malformed frame, a frame of another type, or an undecodable payload is the
worker's `kControlChannelBroken`, which is counted with the controller lost (exit 124),
because a worker that can no longer be told to stop leaves as one whose controller is gone.
The null workload's Stop is answered Done and ends the run; every other command is refused
there with a reason.

### The order of a worker's tick, and a tick that handled a message runs no slice

Beat, duration, controller, look at the control pipe (without waiting when there is work,
for up to a heartbeat interval when paused or in the idle part of a duty cycle), handle
every message in order, then a slice only if none was handled. A Stop is answered before
anything else the worker says, and messages after it are not handled.

### A replacement is a release and a prepare, with the old one prepared again when the new is refused

Rebind and Switch share one routine: close the runner (its records and tally are sent),
release it in the reverse of construction, then prepare the next from the next unit index. A
refusal of the new prepares the old again and answers refused with the reason; only when that
also fails is it a failure of the worker (stage "restore"). A Switch to a name the registry
does not hold is refused before the runner is touched. The ISA path is kept if the new
workload offers it, else the best by `select_path`. A Rebind clears the facts about the
physical package, core and NUMA node that were true of the old placement. The memory frame
is sent again after every replacement, and the controller keeps the weakest.

### The check that could not be made is said once, by the last runner

A runner's final report says what the whole run did not check. At a replacement the report of
the runner being replaced would be a statement about part of the run, so its two "over ..."
statements (oracle, input-integrity bracket) are dropped, and at the end they are kept only
if the cumulative tally says nothing was checked. The fault-injection test with an oracle
that declines to judge finds exactly one such statement for a rotated run, about the last
workload.

### The controller's limits on unanswered messages

At most 64 unanswered messages per worker. A schedule command past that is dropped and
counted (`dropped`); a Stop is always sent. Acknowledgements must arrive in order; an
acknowledgement of anything but the oldest unanswered is a corrupt channel. An effect (a
paused worker, the idle allowance of a duty cycle) is taken only from a Done.

### A duty cycle widens the stall allowance by its idle share

A worker told to be busy a share of a period is silent between slices by design. The
controller's stall allowance for it becomes `stale_after + idle` once the SetDuty is
acknowledged Done, and a paused worker's stall clock is refreshed, so neither is judged hung.

### The schedule is data, made before the run

`mixed::Schedule` is a value with no process or clock behind it: steps, each with the
commands for the workers it names. `problem()` refuses a schedule that does not fit the run
before the first fork; `digest()` is a CRC-32 over a canonical text, equal on every
architecture. Mixed: five phases in a fixed order (mixed, compute-heavy, memory-heavy,
balanced, all-join) indexed by the rotation modulo five, every third rotation a "move" when
placements are given, a footprint of the whole or half of the ceiling, seeds unique per
(rotation, slot). The intensity classes are a hypothesis (compute: dense_linear, fft,
monte_carlo, particles, raytrace; memory: adaptive_mesh, graph, sort, sparse, stencil;
compression and anything unknown: balanced) recorded as one function to change when a
measurement says otherwise. Cycling: load (idle, heavy, moderate, heavy at 0, 1000, 500,
1000 per mille) or cores (shares 125, 500, 1000, 250, 1000 of the workers, at least one),
with only the commands that change a worker's state.

### The journal

An entry at the start, every `journal_interval_ns` (5 s), at every phase (forced), "stop"
at the deadline and "done" after the last reap; appended then synced. A fault in either ends
journaling and is reported, never the run. The controller never closes the writer: the
caller does, in the parent only, so a forked child whose exec failed never closes a journal
it shares. `last_verified_unit` is the largest `units_computed` over the workers' tallies.
An existing journal is replayed first: a clean one is removed, an abnormal or torn or corrupt
one is kept as FILE.N (never overwritten, up to 1000) and its last entry is printed.

### The command line

`stress --test mixed [--rotation D]`, `stress --test cycling [--workload NAME] [--cycle
load|cores] [--dwell D]`, `--journal FILE` on `--test` and `--config`, and `loadforge
journal FILE`. Defaults: rotation = duration/6 clamped to 1 s to 5 min; dwell = duration/8
clamped to 1 s to 60 s; the cycling workload dense_linear; the mixed footprint must be an
absolute size (a share of memory cannot be halved by a rotation).

### The registries hold all eleven workloads

Seven workloads (adaptive_mesh, fft, graph, particles, raytrace, sparse, stencil) gained a
`workload_descriptor()` their own `create()` uses, so the registry can answer `descriptor(name)`
without making one. The registries' tests that used "fft" as the unknown name now use "teapot".

### Q10 is confirmed

See `docs/adr/proposed/29-q10-worker-granularity-under-a-mixed-workload.md`.

## Contract disagreements found

None found against `ControlCodecContract`, `JournalContract` or `RegistryContract`: the
codec passes its suite, and the suites for the journal and the registry pass against this
stream's models (`journal_model_test.cpp`; `roster_test.cpp`).

## Contract questions

1. **`result.hpp` names one workload per worker.** A worker rotated by a Switch ran several
   and the result says only what it started as (`WorkerWork`). Findings carry their own
   provenance, so the judgement is not lost, but the run's summary of what each worker did
   is. Reading chosen: the report states the initial workload and, in the worker's story, the
   count of Switch commands done; a field for the workloads run (in order) is wanted.
2. **How often a worker states its working set.** `control.hpp` says nothing about the
   memory frame (type 18); the M2 controller took a second one for corruption. A Switch
   changes the working set, so the worker sends the frame again and the controller now
   accepts a repeat and keeps the weakest outcome. If the frame is meant to be sent exactly
   once, the contract should say so and a Switch should not repeat it.
3. **The timeline refuses phase marks in the same millisecond** (see above): a limit of
   `TimelineSink::mark_phase`, not mine to change.
