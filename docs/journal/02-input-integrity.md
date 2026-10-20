# Journal fragment 02 — input integrity

**Written:** 2026-10-20 11:40 UTC
**Change:** the provider of `verification/runner.hpp` — the slice runner, the
input-integrity bracket of verification.md §4a around every unit, its working set, and
the triplicated control state of PLAN F18 rule 3 (`src/verification/integrity/`).

## Established by experiment

### Locking the working set refuses an unprivileged run above about 6.6 MB

The runner locks its working set and its three control-state copies (decision below).
Measured on Linux 6.18.44 x86_64 (Intel Xeon @ 2.10GHz, the build container), with a
probe linking the runner and the rotor test workload, run as root and as `nobody`
(`setpriv --reuid=65534 --regid=65534 --clear-groups ./probe <footprint>`), under the
default `RLIMIT_MEMLOCK` of 8388608 bytes:

| uid | footprint | result |
|---|---|---|
| 65534 | 1048576 | ran 16 units, 0 records |
| 65534 | 7000000 | `working set: cannot map 8960384 bytes, ordinary pages, populated, locked: locking failed, the memory, huge pages or lock allowance are exhausted (mlock(8960384 bytes): Cannot allocate memory (errno 12))` |
| 65534 | 16777216 | refused the same way at 21475200 bytes |
| 0 | 16777216 | ran 16 units, 0 records |

The working set is larger than the footprint — strides rounded to whole lines, the
regeneration buffer, scratch and a line of slack — and the three copies take a page
each, so the usable footprint for an unprivileged worker is a little under the limit.
The refusal is the one the runner documents (`kResources`, naming the mapping and the
step); what the integration stream does with it is contract question 1 below.

### The half-built aggregate of journal §1.13, met again in record construction

`report.records.emplace_back(VerificationNotPerformed{run_provenance(), bounded(...),
bounded(...), 0})` and `return ExactBitCorruption{at(...), Side::kInput, "input against
its regeneration", ...}` left `slice_runner.cpp` at 192 of 221 branches with every line
executed: one missing branch was a real gap (a setup naming no workload, now tested), the
other 28 sat on the six lines that built a record from more than one allocating member.
Building each record from named locals, one allocation per statement, through a
`noexcept` helper that only moves, took the file to 190 of 190.
The rule is §1.13's; the shape is new — a record aggregate, where the earlier case was an
error aggregate.

### A test double's inline executor lambda reads as never executed (§1.22)

The two test workloads first defined `compute()` inline in the support header, each
calling `worker::for_each_index` with a lambda. Five test translation units include the
header; the whole-tree gate then reported `src/worker/executor.hpp` lines 79-80 — the
trampoline lambda inside `for_each_index` — at zero for the rotor workload's
instantiation, with that workload's units demonstrably computed hundreds of times. The
discarded COMDAT copies kept their zero counters under a record gcovr kept apart, as
§1.22 describes for a defaulted comparison. Defining both `compute()`s once, in a support
library both test executables link, made it one object and one record: 100% lines,
functions and branches over all 40 source files.

### What the real kernel and the fake give the runner to align

`mmap` on the real kernel returns page-aligned memory, so for every line size up to the
page the alignment slack is zero; at 16384 bytes it is used (tested,
`EverySlotIsLineAlignedAtEveryLineSize`). The system-call fake hands out heap memory,
aligned to 16 at best, so under the fake the slack is in general not zero (tested at
256). Both arms of the alignment are reached by real memory, not by a
branch: the computation has none.

### A control copy changed during a unit is overwritten when the unit ends

The runner writes all three copies after every unit, from the values voted at the unit's
start. So a copy corrupted while a unit is computed is rewritten without ever being voted
on — and, more to the point, a corruption of all three copies alike during a unit, the
one corruption a vote cannot see, does not reach the unit, which holds the values voted
before it began (`AUnitKeepsTheValuesVotedAtItsStartWhateverHappensToTheCopies`). The
vote therefore guards the state at rest: from the end of one unit to the start of the
next, which includes the whole interval between slices, the longest time it sits
unused. The tests that corrupt copies under a running runner do it there.

### `is_valid(Verdict)` and `blocks_of()` fail the static release build when inlined

The `static` preset (Release, `-O3`, the product's `-Wnull-dereference -Werror`) refused
`slice_runner.cpp`: `oracle.hpp:124` (`blocks_of`'s last arm,
`std::get_if<NotChecked>(&verdict)->blocks_compared`) "null pointer dereference", and
`basic_string.h:1072` "potential null pointer dereference" inlined from
`is_valid(const Verdict&, Contract)` at `oracle.hpp:109` (`unchecked->what.empty()`).
GCC 13.3 cannot see that the last `get_if` of a chain over a four-way variant is never
null. The debug, coverage and Clang builds were clean, and the contract headers' own
compile units (`tests/contracts/headers/`) are built without optimisation, so nothing
had met it. Calling `is_valid()` out of line with `[[gnu::noinline]]` did not help: the
warning moved into the wrapper. What works: `std::visit` for the blocks, `std::get` for
the last alternative, and `is_valid()` asked of a `Verdict` re-made from the held
alternative inside a visit, where the index is a constant. Any consumer of these two
helpers in an optimised build will meet the same refusal; contract question 9.

### GoogleTest refuses typed suites a test includes and does not instantiate

`runner_contract.hpp` and the other suite headers each declare two typed suites
(`OracleContract` beside `OracleRegistryContract`, `WorkloadContract` beside
`RegistryContract`). A test executable that includes a header and instantiates one of
its suites fails with `UninstantiatedTypeParameterizedTestSuite<...>` for the other.
`GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(Name)` is accepted for typed-parameterised
suites too, and the registries are the integration stream's, so this module allows them
by name rather than instantiating a registry it does not have.

## Decisions

- **Units run one after another; the executor goes to each unit's `compute()`.** Running
  units concurrently over the executor would leave `compute()` no executor of its own
  (the contract forbids calling `run()` from inside a task) and bring a P5 obligation to
  the runner. Serial units keep the §4a temporal invariant by construction: nothing but
  the unit touches its slot between its write and its closing check. *Rejected:* units
  over the executor with a serial executor for `compute()` — it would change what a
  parallel workload is measured doing. P5 is therefore not declared, with the reason in
  the ledger note.
- **One mapping holds the working set**: every slot's input, every slot's output, the
  bracket's regeneration buffer and the executor slots' scratch, each region aligned to
  the line size inside one line of slack. `buffer_offset` is the offset in that mapping.
  The layout is a pure function (`plan_layout`) so the overflow arm is tested directly.
- **The working set is mapped with ordinary pages, populated, locked, and bound to the
  worker's node when the setup names one** — "mapping, advising, binding and locking" of
  the brief, where the advising is `MADV_POPULATE_WRITE` (platform/memory's populate
  step). *Not* transparent huge pages: the huge-page strategy is a recorded choice of the
  run (PLAN F7), and the runner has no field to record it in. Locked always, because a
  page swapped out and back is a different physical page and a round trip through
  storage, and a finding on it would be attributed to memory it never stayed in. The
  cost is the experiment above. Written up as `docs/adr/proposed/02-input-integrity-working-set-memory.md`.
- **What the control state protects**: the seed the records carry, the next unit's
  index, the duty cycle's two periods and the line size — each value whose silent
  corruption becomes a false finding (a wrong index regenerates another unit's input and
  the bracket reports a difference that never happened) or a wrong record. *Not* the
  slot geometry: a corrupted stride or slot count writes out of bounds or crashes, which
  the supervisor sees as a crash, a different and loud failure, and the geometry lives in
  ordinary memory beside the code that uses it.
- **The vote decides when at least two verified copies agree**; every other copy is
  rewritten and counted as a repair. One verified copy beside two that fail their
  checksums is a failure (`kTooFewVerified`), not a decision: it is a winner picked, not
  a majority found, which PLAN F18 forbids. Verified copies that disagree are
  `kDisagreement`. Either stops the run.
- **The vote happens once per unit**, at its start; every step of the unit uses the
  decided values, and the next index is written to all three copies when it ends.
  *Rejected:* a vote before every step — five reads of three copies per unit for no
  protection the per-unit vote lacks, since the steps' working values live in registers
  and on the stack, which no triplication covers.
- **A run that cannot decide its control state stops**: the slice reports a
  `VerificationNotPerformed` about the run (no iteration, no buffer), runs no further
  unit, and every later `run_slice()` answers `kFinished` with the reason; `finish()`
  still closes the run once. Never an `ExactBitCorruption`, never routed to isolation
  (verification.md §6, last bullet).
- **Findings the contract does not spell out:** an oracle that throws, or answers a
  verdict `is_valid()` refuses, is a `VerificationNotPerformed` whose blocks are not
  counted; a `NotChecked` verdict's own `blocks_compared` is counted in `oracle_blocks`
  ("summed over verdicts"); text longer than `kMaxTextBytes` — the setup's workload name,
  an exception's message — is cut to fit, so no record the runner makes fails to decode.
- **An oracle whose contract differs from the workload's descriptor is refused** as
  `kConfiguration`, beside the oracle for another workload.
- **`next_index` in a report is an unprotected shadow** of the voted index, updated after
  each unit. Nothing is regenerated from it; it is the progress the worker publishes.
- **Doubles and seams.** The three consumed contracts have this stream's own doubles —
  an exact workload (`rotor`) and a bounded one (`mean`), their oracles written from
  their one-line specifications, a serial and a threaded executor — each run against its
  contract's suite. The system-call seam is not one of ADR 0004's contracts; the tests
  use the shared `FakeSyscalls` behind a decorator of this stream's own that fails the
  n-th call of a kind, which the fake cannot, and records the mappings it hands out so a
  test can reach the memory the runner holds. The contract suite itself runs over the
  real kernel.
- **The brief asks for a pull request when the stream is done; this run's instructions
  give that to the manager, so none was opened.** The branch is pushed.

## Contract questions

1. **`RunnerSetup` has no memory policy.** Nothing says whether the working set is
   locked, which huge-page strategy it uses, or whether it is populated. *Reading:*
   populated, ordinary pages, locked always; refused as `kResources` naming the step
   when the kernel says no. *Proposal:* a `platform::MappingRequest`-shaped policy in
   `RunnerSetup`, chosen by the controller and recorded with the run, so an unprivileged
   run can choose not to lock and say so.
2. **`RunnerSetup` names one NUMA node, not the machine's.** F18 asks for the control
   state's copies on different nodes where there are several; the runner knows only
   `base.numa_node`. *Reading:* the store spreads its copies round-robin over the nodes it
   is given (tested with two), and the runner gives it the worker's own node, or none.
   *Proposal:* the node list in `RunnerSetup`.
3. **The seed regeneration depends on lives in the workload.** `generate(index, input)`
   takes no seed; the workload's factory consumed `WorkloadConfig::seed` and keeps it in
   the workload's ordinary memory, where the runner cannot protect it. A corrupted copy
   there regenerates different data in both the write and the bracket's checks, so the
   bracket stays consistent, but every input is then a different unit's. *Reading:* the
   runner protects the index it passes and the seed it records. *Proposal:* the seed as
   an argument of `generate()`, so the runner passes the voted one.
4. **How a stopped run answers.** The header gives `run_slice()` one error, `kFinished`,
   "called after finish()". *Reading:* a run stopped on its control state answers
   `kFinished` with a detail that says why, and `finish()` still closes it once.
   *Proposal:* a `RunnerFault::kStopped`.
5. **A control-state repair has nowhere to go.** A repaired copy is evidence of a memory
   event on the control pages — neither a wrong result nor a check not made. *Reading:*
   counted inside the store (`ControlState::repairs()`) and not reported. *Proposal:* a
   `Tally` field, so a run with repairs says so.
6. **"Tolerances and expected checksums"** (F18 rule 3) are not the runner's: the
   tolerances live in the oracle, the checksums in the workloads that keep any. Each
   provider that holds such a value owes it the same protection, and the oracle contract
   does not say so.
7. **An oracle of the other contract.** The header refuses "an oracle for another
   workload"; it does not mention an oracle whose `contract()` differs from the
   workload's `descriptor().contract`. *Reading:* refused as `kConfiguration` too.
8. **A verdict that breaks `is_valid()`.** The header does not say what the runner does
   with one. *Reading:* a `VerificationNotPerformed`, its blocks not counted (decisions).
9. **`is_valid(const Verdict&, Contract)` and `blocks_of()` do not compile in the
   static release build** once inlined into a consumer (above). *Reading:* call them only
   where the alternative is known (a visit), at the cost of one copy per checked unit.
   *Proposal:* write their last arms with `std::get` or a visit, as record.hpp's comment
   on `std::variant`'s `operator==` already does for clang-tidy, and compile
   `tests/contracts/headers/` under the release flags too.
