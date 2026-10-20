# Review request

- **Date and time:** 2026-10-20 03:52 UTC.
- **Series:** review requests are dated and kept, one file each, in `reviews/`; the newest is the current one.

LoadForge has had no outside review. Every pull request so far was checked by CI gates
and by the author, and one internal read-through of the whole tree (`reviews/`) found a
real memory-ordering defect that every gate had passed. That is the case for asking you.

This page is everything you need to review any part of it. Pick one area, or several; an
hour on one area is worth more than a skim of all of them.

## What LoadForge is, in one paragraph

A Linux hardware-reliability and stress tool in C++20. A controller process runs workloads
in separate worker processes (fork, then exec of the same binary), supervises them through
a lock-free shared-memory heartbeat and a checksummed record pipe, and classifies how each
one ended: completed, faulted, timed out, OOM-killed, killed, corrupt channel. The rule the
project lives by is that a wrong verdict is worse than no verdict: a tool that tests
hardware must never report its own bug as a hardware fault. Milestone M1 (the supervision
framework) is nearly complete; no real workloads exist yet.

## Getting started

```bash
git clone https://github.com/ivoitovych/LoadForge && cd LoadForge
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

GCC 13 or Clang 18 on Linux. Read `README.md`, then `docs/PLAN.md` §4 (architecture) for
the design, and `docs/JOURNAL.md` for what was learned by experiment. Every source file
explains its own decisions in comments; disagreeing with a stated reason is a finding.

## The review areas, most valuable first

Each item names the files and the questions. "Is this right?" is always in scope.

### 1. Memory ordering and lock-free shared state

- `src/worker/ipc/heartbeat.cpp` and `.hpp`: a single-writer seqlock per slot in a memfd
  shared across processes. The synchronisation argument is written out in
  `tests/unit/worker/ipc/heartbeat_test.cpp` (section "the protocol (P5)").
- Is the writer's fence placement correct on ARM64? Is the reader's? Is the argument
  complete, or does it rely on something x86 gives for free?
- A slot that cannot be read consistently is treated as stale only after the staleness
  allowance (journal §2.22). Can a live writer be misjudged, or a dead one missed?

### 2. Process supervision

- `src/controller/orchestration.cpp`: the one loop. Beat, deadline stop, staleness
  escalation (stop request, SIGTERM, SIGKILL, each after a grace), poll, drain, reap.
- `src/controller/supervision.cpp`: the classification table, a pure function.
- `src/platform/process.cpp`, `worker_pool.cpp`: fork, `PR_SET_PDEATHSIG` with its race
  closed, each worker in its own session, the group killed on reap.
- Questions: Can any ordering of events misclassify a worker, especially a fault reported
  as anything else, or a kill reported as a fault? Can the controller hang, or exit leaving
  a worker or a grandchild running? Are there signal-safety problems? Is anything between
  fork and exec unsafe?

### 3. Reading the kernel correctly

- `src/resources/`: `MemAvailable`, the memory cgroup found through `/proc/self/cgroup`
  and `/proc/self/mountinfo`, the binding limit under cgroup v1 and v2, the OOM-kill count.
  The OOM verdict is all-or-none over a run (journal §2.23); is that the right call?
- `src/topology/`: CPU, cache and NUMA discovery with cross-checks, and a written list of
  checks deliberately not made. Do any of the checks refuse a real machine? Do any of the
  omissions let a wrong topology through?
- `src/platform/memory.cpp`, `affinity.cpp`: `mbind`, `mlock`, huge pages, affinity; is
  each errno classified correctly?

### 4. The record channel

- `src/worker/ipc/record.cpp`: CRC-32 framed records over a pipe; a reader that never
  resynchronises after a bad frame. Can a corrupt or truncated stream be read as valid?
  Can the controller be made to allocate without bound, or to block?

### 5. Are the tests and gates actually strong?

- The project claims 100% line, branch and function coverage with zero exclusions, a
  per-module test-obligation ledger (`tests/obligations.toml`,
  `tools/check-test-obligations.py`), and a mutation gate (`tools/mutation-gate.py`).
- Pick any module and ask: do its tests assert behaviour, or only execute lines? Find a
  change to the code that breaks it and that no test would catch.
- Every module above the kernel is tested against one shared fake,
  `tests/support/fake_syscalls.hpp`. It has already been wrong once (it modelled a write to
  a closed pipe as `EPIPE` without the `SIGPIPE` the kernel sends first). Where else does it
  disagree with the real kernel?
- Are the gates fail-closed? `tests/tools/` feeds each gate evidence it must refuse; can
  you make one pass when it should fail?

### 6. Design and doctrine

- `docs/PLAN.md`, `docs/IMPLEMENTATION.md`, `docs/adr/`: is the architecture sound for a
  tool that will run five-hour stress tests on real hardware? ADR 0003 (components share
  only contracts, never tests or mocks) governs how the rest is built.
- `docs/verification.md` and `docs/determinism.md`: the verification and determinism
  contracts every workload must meet. Are they achievable? Are they enough?

### 7. Safety of running it

- A stress tool runs near thermal and memory limits, sometimes as root. Can it leave a
  machine in a bad state: orphaned workers at full load, locked memory, a run that cannot
  be stopped? What happens on Ctrl-C, on SIGTERM to the controller, on a machine that
  suspends mid-run?

## Already known, so you need not report them

`reviews/comprehensive-project-review-2026-10-19-0449-UTC.md` lists what the internal review
found; most of it is fixed and the rest is queued in `docs/ROADMAP.md`. `docs/JOURNAL.md`
§6 lists known open threads, including one known gap: a worker's grandchild survives when
the controller itself is SIGKILLed.

## How to report

Whatever is easiest for you:

- a GitHub issue per finding, or one issue with a list;
- comments on any open pull request, or on a specific line through a commit;
- an email to the maintainer, Iaroslav Voitovych, with whatever you found.

For each finding, the file and line, what goes wrong, and how you would make it happen.
"I don't understand why this is safe" is a finding too: if the reason is not written down
where you looked, that is a defect in the documentation.

Thank you. Every finding will be answered, and every one that holds will be fixed with a
test that fails without the fix.
