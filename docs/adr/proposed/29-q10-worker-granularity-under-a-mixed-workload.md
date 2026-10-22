# ADR — Worker granularity under a mixed workload: Q10 confirmed

**Status:** proposed
**Date:** 2026-10-21 22:46 UTC
**Stream:** 29, the M5 integration (`feature/integration-m5`)

## Context

Q10 (PLAN §4.6) was closed at M1 in favour of multiple worker processes, one
executable, re-executed with an internal switch. M5 exit criterion 4 asks that
the choice be checked against the mixed workload, where one worker runs one
workload after another, and either confirmed or replaced by an ADR saying why.

The alternatives are the ones the PLAN weighed: one process with a thread per
workload, or one process per workload per core. What a mixed workload adds is
that a worker's *work* changes while its *process* does not, so the question is
whether a process that is told to change workload keeps the properties the split
was bought for.

## Decision

Keep multiple worker processes. A worker is a process; what it runs is a
property it is told, not something it is born with. A workload change (the
`Switch` message) replaces the runner, the executor, the oracle and the
workload inside the same process, in the order the start-up uses, and the
process, its heartbeat slot, its pipes and its cumulative tally stay.

## Evidence

All of it is in the repository's tests and runs on every push:

- **Fault containment holds across a rotation.** A worker killed with SIGKILL
  between two rotations is reported killed while its neighbour finishes its
  rotation and answers every message it was sent
  (`tests/supervision/control_test.cpp`, N = 2 real processes). One process
  holding every workload would have lost the neighbour with it.
- **A corruption is attributed to what was running.** With the faulty worker, a
  corruption found before a switch is reported about the workload before, one
  after about the workload after, and the neighbour that was never switched
  reports only its own; the units are numbered on across the switch with none
  repeated (`ACorruptionIsFoundAboutWhicheverWorkloadAWorkerRanWhenItHappened`,
  and the in-thread version in `tests/fault_injection/workloads/mixed`).
- **The controller's death still takes every worker with it** with a mixed run
  and a journal: the shipped binary is killed with SIGKILL mid-rotation, its
  process group empties within ten seconds, and the journal it left replays
  (`tests/supervision/journal_kill_test.cpp`).
- **Nothing accumulates across rotations.** The soak rotates one worker every
  250 ms for LOADFORGE_SOAK_SECONDS and finds no growth of the resident set,
  descriptors or threads, no tally running backwards, and no verification lost
  (`tests/soak/workloads/mixed`, nightly for hours).
- **Every control message is answered by every worker** in a run of two or
  more (`ControlOverRealWorkers`).

## What it costs, and what stays open

- A switch frees one working set and allocates another; the first units after
  it are cold. The schedule changes workloads on the order of seconds, not
  microseconds, and the cost is paid once per rotation.
- The run's result names one workload per worker (`WorkerWork`, frozen in
  `result.hpp`): the one it started as. A worker that rotated ran others, and
  the findings carry their own provenance but the summary does not. That is a
  contract question, written in the journal fragment, not a reason to change the
  granularity.
- A `Rebind` moves the executor's threads, not the worker's own control
  thread, which stays where the process was placed.

## Alternatives considered

- **One process, a thread per workload.** Rejected for the reason the PLAN gave
  and nothing in M5 weakens it: a workload that corrupts memory or hangs takes
  every other workload with it, and the one finding that matters most (which
  workload, on which core) is the one this loses.
- **A new process per rotation.** Would give the cleanest isolation, and the
  cost is a start-up per rotation and a gap in a unit numbering that is meant
  to be continuous. A switch inside the process keeps both; a process that
  misbehaves after a switch is still one process the controller can kill and
  report.
