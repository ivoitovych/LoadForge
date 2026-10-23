# ADR 0006 — The slice runner's working set is populated, locked, and of ordinary pages

**Status:** accepted; amended 2026-10-21 18:02 UTC (see the note below)
**Date:** 2026-10-20 11:25 UTC
**Accepted:** 2026-10-20 18:03 UTC, with the merge of pull request #40 (stream 02, input integrity)

> **Amended, 2026-10-21 18:02 UTC (the M2 contract corrections):** locking is now a
> controller-chosen policy. `LockPolicy { kRequire, kTry, kNone }` in
> `src/verification/runner.hpp` is carried by `RunnerSetup::lock` (default `kRequire`); the
> command line defaults to `try`, which falls back to unlocked memory and says so, and the
> result records the choice (`WorkloadResult::lock_policy`, `memory_lock`). So "there is no
> fallback" below holds for `kRequire` only, and the 6.6 MB consequence applies to `kRequire`
> only: an unprivileged run under the 8 MiB allowance uses `kTry` or `kNone`. The "superseded
> when" condition is met for locking. What still holds: ordinary pages, populated with
> `MADV_POPULATE_WRITE`, bound to the worker's NUMA node, and no huge-page policy.

## Context

`verification/runner.hpp` gives the slice runner a system-call seam to allocate its
working set through and nothing else: `RunnerSetup` says where the worker runs
(`base.numa_node`) but carries no memory policy — no huge-page strategy, nothing about
locking or populating. The runner must still decide each of them, because
`platform::Memory::map()` asks, and each answer changes what a finding on that memory
means:

- A page that is not resident when a unit is computed is faulted in during the unit, and
  the run measures the page-fault handler as well as the workload.
- A page swapped out and back is a different physical page that made a round trip
  through storage. A bit flip found on it may have happened in the storage path, and the
  record would attribute it to memory it never stayed in.
- The huge-page strategy changes TLB behaviour and so the result (PLAN F7); it is a
  recorded choice of the run, and a strategy the runner picked silently would be a
  measurement nobody could repeat.

## Decision

The working set — and the three control-state copies of PLAN F18 rule 3 — are mapped
with **ordinary pages**, **populated** (`MADV_POPULATE_WRITE`, whose failure is reported,
unlike `MAP_POPULATE`'s), **locked** (`mlock`), and **bound to the worker's NUMA node**
when the setup names one. A refusal at any step is `RunnerFault::kResources` with
platform/memory's words naming the mapping and the step. There is no fallback: a run
that cannot lock its working set does not start, rather than start on memory whose
findings mean less than it says.

## Consequences

- An unprivileged worker under the default `RLIMIT_MEMLOCK` of 8 MiB cannot run a
  footprint above about 6.6 MB (journal fragment 02, measured). Root, `CAP_IPC_LOCK`, or
  a raised limit are needed for anything larger. The integration stream must either size
  the footprint within the limit or say why it refused.
- Transparent and explicit huge pages are unavailable to the runner until the contract
  carries a policy; the runner never chooses one on its own.
- *Rejected:* trying the lock and continuing without it — a silent downgrade of the
  evidence, F20's failure shape. *Rejected:* never locking — it leaves every finding
  open to the storage round trip above, on exactly the long stress runs where swapping
  is most likely.
- *Superseded when* `RunnerSetup` gains a memory policy chosen and recorded by the
  controller (journal fragment 02, contract question 1): the runner then maps what it is
  told and the run says what that was.
