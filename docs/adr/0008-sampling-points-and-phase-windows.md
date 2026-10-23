# ADR 0008 — The run is sampled at four fixed points, and a phase's window includes the round on its boundary

**Status:** accepted
**Date:** 2026-10-20 21:40 UTC
**Accepted:** 2026-10-21 10:17 UTC, with the merge of pull request #87 (stream 21, the M3 integration)

> **Outcome, 2026-10-21 14:02 UTC:** decision 3 (the window of a phase ends one nanosecond past the
> next mark) is stated in `statistics/signature.hpp` and built by
> `statistics::phase_window()` and `run_window()`. The recorded follow-up is done:
> `src/main/run_result.cpp` builds each window with them, and `phase_windows()` remains only
> as the labelling wrapper (`docs/journal/m3-contract-corrections-2026-10-21-1145-UTC.md`).
> The rest of this ADR is unchanged.

## Context

The sampler starts no thread and reads no clock (`telemetry/sampler.hpp`): the caller says
when a round happens. The controller has one loop with one blocking call. Three questions
follow that no contract answers, and a contributor who did not see them argued would find
each answer arbitrary:

1. **When are rounds taken?** A counter's delta is the difference of two readings. A run's
   delta from a counter that was first read at the first tick, or last read before the
   workers' last act, misses what happened before the first tick and after the last read.
2. **To which phase does a counter's advance belong?** `PhaseMark` says a phase lasts "from
   `start_ns` until the next mark", and a `Window` is `[start_ns, end_ns)`. Summarising each
   phase over exactly that window leaves the advance between the last reading before a mark
   and the reading on it in neither phase (`CounterSummary::delta` sums consecutive pairs
   that are both inside the window). EDAC deltas are reported per phase (PLAN F13); per-phase
   deltas that do not sum to the run's would under-report corrected errors at every boundary.
3. **What does a run's window end at?** A window excludes its end, and the closing reading is
   the point of taking it.

## Decision

1. The controller takes a round **before the first fork**, **every sample interval**, **at the
   deadline, at the instant the next phase is marked**, and **after the last reap**. A round
   whose time is not later than the last one's is skipped (the timeline would refuse it); one
   that is not yet due is skipped unless it is forced (the deadline's and the last).
2. Phases are marked at those instants: "run" at the baseline round, "stop" at the deadline's.
3. A phase's window runs from its mark to **one nanosecond past the next mark** (the last, to
   one past the run's last round). The round on a boundary is therefore in both neighbours'
   windows, and every consecutive pair of readings is in exactly one phase: the advance that
   ended on the boundary belongs to the phase that was running while it happened.
4. The run's signature window is the first round to one nanosecond past the last.

## Consequences

- Per-phase counter deltas sum to the run's (tested: `PhaseTest` in `tests/integration/m3`;
  the naive reading is the failing test `Statistics_SignatureHpp_PhasesTakenAtTheirMarksSumToTheRun`).
- A gauge's per-phase summary counts the boundary reading in both phases. That is a
  statement about the instant, not a double count of time, and the sample counts say it.
- Four rounds beyond the interval's own are paid for per run; the timeline's capacity
  (`timeline_capacity`) reserves a margin for them.
- The alternatives were to change `Window` to a closed interval (a contract change, and every
  consumer's reading of "end excluded" changes) or to have the summarizer attribute boundary
  pairs itself (a contract change to `CounterSummary`). Neither was needed to be correct, and
  both are open to the manager.
