# Journal fragment 17 — workload: sort

**Written:** 2026-10-20 13:40 UTC
**Change:** `src/workloads/sort` (stream 17): the sort workload, its tests in five tiers,
its ledger fragment.

## Established by experiment

- **The specification is implementable and its independent reading agrees.** The golden
  digests in `tests/fixtures/determinism/workloads/sort/golden.txt` were made by
  `make_golden.py`, a Philox4x32-10 written from Random123 (checked against three of its
  published vectors before anything is written) and Python's stable `sorted` on
  `(key, position)`. The C++ generator and sort reproduced all five digests on the first
  run, for P = 1, 16 and 64. Made on x86-64, Python 3.13; the ARM64 jobs are the check
  that the same bits come out there (the sort is integer-only).
- **A skipped move task has a predictable footprint.** With scratch and output prefilled
  `0xFF`, dropping one range of the first pass's move leaves that range's destination
  slots as key `2^64-1`, index `0xFFFFFFFF`; they sort last, so the checker reports
  "index out of range" at position `kKeys - kKeys/P` exactly. T5 asserts it.
- **A task run twice is harmless** (count and move are idempotent writes of the same
  values), so an executor that breaks "exactly once" by repeating is not detectable by
  output, and the sort does not need to defend against it.
- **Cost, release build, x86-64:** about 25 ms per unit (compute) at P = 16 on one thread.
- **Coverage:** 100% line, branch and function over `src/` (5562 / 4467 / 1210) with the
  workload included. Returning errors from `create()` through `noexcept` string builders
  left no landing-pad branches (JOURNAL §1.13).

## Decisions

- **Radix, not merge.** A stable LSD radix sort gives the specification's order (ties by
  input position) by construction, with no comparisons and no data-dependent branches; a
  merge sort needs the tie rule in every comparison. Both are linear-memory; radix wins on
  evidence of correctness.
- **Eight one-byte passes, none skipped.** 16-bit digits need 64 K counters per range
  (16 MiB at P = 64); 11-bit digits give 6 passes but ragged shifts. Eight passes is even,
  so the last pass writes the output and the first reads the input directly (its indices
  are the positions, so no index array is initialised). A pass is never skipped when all
  keys share a digit: a stress workload's load should not depend on its data, and a skip
  would break the parity.
- **Work by range, never by thread.** Per pass: P count tasks, a serial prefix in
  (digit, range) order on the calling thread, P move tasks. The ranges write disjoint
  bytes; the output does not depend on the executor.
- **Scratch.** `scratch_bytes` = a second copy of keys and indices (1.5 MiB) plus
  `P * 256 * 4` counter bytes; it is used from the start of the span given, whatever the
  thread count, so with N threads (N-1) pieces are unused. The counters are indexed by
  range, not slot, so nothing in scratch is slot-owned.
- **Refusals.** In this order: decomposition (not 0 and not in `kChunkCounts`), ISA path
  (only scalar: the sort has no vector kernels, `paths = {scalar}`), footprint (less than
  one unit of 2.5 MiB). Each detail names the offending value.
- **Plan costs.** `logical_bytes` = 131072 × (28 + 7 × 32) = 33 030 144 (histogram read
  8 B per key; scatter read 8 B in pass 0 and 12 B after, write 12 B). `operations` =
  keys × passes × 2 (a count and a move) = 2 097 152.
- **Entry point.** `workloads::sort::create(const WorkloadConfig&)`; the contract has no
  factory signature, the integration stream wires the registry.
- **T5/T5b for a workload.** The workload has no verification of its own, so P4 is its
  tests' checker (`tests/oracle/workloads/sort/check.hpp`, from the specification, never a
  sort). T5 corrupts outputs and drops executor tasks and asserts verdict and position;
  T5b gives the checker wrong-sized evidence and asserts NotChecked with 0 compared.
- No ADR proposed: nothing here meets ADR 0001's test.

## Contract questions

- `Executor::run` does not say that the caller's writes made before `run()` are visible to
  the tasks. The sort writes the prefix sums on the calling thread between two `run()`
  calls and relies on it; read as: a pool must publish them (any thread-start or queue
  hand-off does). Worth stating in `executor.hpp`.
- `Plan::scratch_bytes` is per slot, but a unit-wide buffer cannot be split by slot; the
  sort takes it from the first piece and wastes the rest. A per-unit scratch field would
  say what such workloads need.
- Which of several refusals is reported when a configuration has more than one fault is
  unspecified; chosen as above.
- The specification's "oracle may need kKeys bits" is the oracle's; not touched here.
