# The M2 contract corrections: five disagreements, decided

**Written:** 2026-10-21 18:02 UTC
**Branch:** `fix/m2-contract-disagreements`
**Authority:** ADR 0004, "Changing a contract": the header, its suite and its reference change
together, every in-tree instantiation of the suite still passes, the reason is in the header and
here. The evidence for each disagreement is under "Contract disagreements found" in
`docs/journal/11-integration-m2.md` and in its entry "CI's lock allowance"; the way the last
round was recorded is `m3-contract-corrections-2026-10-21-1145-UTC.md`. The tests of
`tests/integration/m2/disagreements_test.cpp`, which stated what was observed, are now
`corrections_test.cpp`, which states what the corrected contracts guarantee.

The shape of the whole change, so that a reader sees what it costs: one new function on a
registry (`Registry::descriptor`), one lock policy and its outcome on the runner, three fields on
the provenance (`physical_package`, `decomposition`, `footprint_bytes`), the record format
going from 1 to 2, the result schema from 2 to 3, two frames' worth of worker protocol (frame 18,
and a `--lock` argument), and no change to `select_path`.

## 1. select_path against a workload's paths

**Question.** `select_path` picks the first path both of its sets hold and cannot see what a
workload has kernels for; on an AVX-512 machine it picks a path `dense_linear` refuses. The
controller worked around it by creating every workload twice.

**Options.**
(a) `select_path` takes the workload's set as a third argument. It would not remove the double
creation: the set is the workload's, and the workload cannot be made before its path is chosen.
Something would still have to say the set without a workload.
(b) `Registry::descriptor(name)`: what a workload says about itself, without making one. The
choice is then made from three sets by the caller, and `select_path` stays a pure function of two.

**Choice: (b).** It is the only one of the two that removes the problem, since (a) needs (b)'s
information anyway. A descriptor is constant for a workload's lifetime and does not depend on a
configuration, so the method takes a name only. The contract suite checks that it equals what a
created workload says and is the same on every call, and that an unknown name is refused with
`create()`'s own words. Each of the four registered workloads gained a `workload_descriptor()`
that its own factory also uses, so the two cannot differ. `controller::choose_isa` stays: it is
where the three sets meet, and where a requested path the workload lacks is refused in words.
`dispatch.hpp` says so, and says the unrequested call chooses from `compiled` intersected with the
workload's paths.

**Files.** `src/workloads/workload.hpp`, `registry/registry.{hpp,cpp}`, the four workloads'
headers and sources, `src/controller/plan.{hpp,cpp}`, `src/primitives/simd/dispatch.hpp`; suite:
`tests/contracts/workloads/registry_contract.hpp` (two tests), reference: `reference_workload_test.cpp`;
the controller's model: `ScriptedRegistry` in `tests/unit/controller/consumer_models.hpp`, run
against the suite in `consumer_models_test.cpp`. **Proof:**
`PlanWorker.AWorkloadIsMadeOnceForTheChosenPathAndItsPathsAreAskedWithoutMakingOne` (one creation,
one description, no AVX-512 for `dense_linear` on a machine that has it) and
`Corrections.SelectPathAloneStillKnowsNoWorkloadAndTheRegistryGivesItsPathsInstead`.

## 2. RunnerSetup has no memory policy

**Question.** The runner always mlocks its working set, so an unprivileged process (8 MiB allowance)
cannot run the default 32 MiB footprint, nor any sort configuration. Is "sort's working set is about
3.2 units" right?

**Sort's working set, checked.** A unit of sort is 2.5 MiB (1 MiB of keys in, 1.5 MiB out). The
runner maps the units that fit the footprint, plus one more input (the bracket's regeneration),
plus the scratch of every executor thread (1.5 MiB and a histogram for sort), plus a line of slack.
At one unit that is 2.0 units on one thread and 3.2 on three, which is the 8437824 bytes CI saw: the
3.2 was the three-thread figure. It is right. What was wrong is that `workload.hpp` said scratch is
"outside" the footprint and "the runner sizes the footprint to leave room for scratch", which it
does not: the footprint counts what the units hold, and the working set is larger by the regeneration
buffer and the scratch of each thread. The header now says so
(`Corrections.SortsWorkingSetIsItsUnitsPlusOneInputPlusScratchForEveryThread`).

**Options for the policy.**
(a) A policy in `RunnerSetup`: lock, try, or none (journal 02's proposal).
(b) A bare boolean. It cannot express "try, and say what happened".
(c) Fix it outside the runner: the controller lowers the footprint to what the allowance holds. It
changes what is measured to suit the process's rights, silently.

**Choice: (a).** `LockPolicy { kRequire, kTry, kNone }` in `RunnerSetup` (default `kRequire`, what
the runner did), and `SliceRunner::memory_lock()`: the policy and its `LockOutcome` (`kLocked`,
`kNotLocked`, `kFellBack` with the kernel's refusal in words). `kTry` falls back only when the
kernel refuses at the lock step; every other refusal is still `kResources`. The control state's
three copies follow the same policy through one function (`integrity/locking.cpp`), and the outcome
is `kFellBack` if either the working set or a copy fell back.

The controller chooses (`VerificationOptions::lock`, default **try**: a run that does not start
for want of a right is worse than one that says its pages were not locked; `stress --test --lock
require|try|none` overrides). The choice is told to every worker and re-run (`--lock`); the
runner's outcome travels to the controller in a new frame (type 18, once, before the first slice:
the policy is the controller's, the outcome is the worker's fact) and is recorded per workload in the
result (`lock_policy`, `memory_lock`; null when the worker never said), schema 3. It is a note on
the worker as well. It does not change the verdict: an unlocked run still checked every unit.

**Files.** `runner.hpp`, `integrity/{locking,control_state,slice_runner}`, `worker/{entry,frames,verified_run}`,
`controller/{plan,report,orchestration,rerun}`, `main/{cli,outcome}`, `controller/results/`;
suite: `runner_contract.hpp` (three tests), reference and the worker's model runner:
`memory_lock()`. **Proofs:** an injected test for every refusal at every step under each policy
(`locking_test.cpp`, `runner_test.cpp`, `control_state_test.cpp`), the frame's both sides
(`frames_test.cpp`, `orchestration_verified_test.cpp`), and as a real unprivileged process under an
8 MiB allowance `Corrections.AnOrdinaryUserCannotLockTheDefaultFootprint...` and
`...EveryWorkloadRunsAtItsSmallestFootprint...`.

**Can the CI lift go?** See "The lock allowance in CI" below.

## 3. The Rerunner rebuilds a unit from five fields

**Question.** The output also depends on the decomposition and the footprint, which the controller
supplied from the run.

**Options.** (a) Leave it to the controller's side channel (`use(work)`). A record then cannot be
repeated without the run, and a record from one run read against another's `WorkerWork` is a re-run
of a different unit. (b) Put both in the provenance. (c) Only the decomposition.

**Choice: (b).** The decomposition decides the bits of a `kBitExactFixedDecomposition` workload
(`dense_linear` at depth 16 and 256, shown in `corrections_test.cpp`), and it is the one the
workload `resolved` (`plan().decomposition`), never the 0 that asks for a default. The footprint
does not change a unit's bits, but it fixes how many slots the working set has and so where in the
mapping the unit sat (`buffer_offset`), hence which cache sets it used; a core-local fault in a
cache set is the thing the ladder looks for, and a re-run in a different layout could not meet it.
The `Rerunner` contract now names both and refuses to guess: a finding whose provenance lacks either
is `kNotPerformed`. `RealRerunner` takes them from the record; from `use()` it still takes the
cache-line size (a fact of the machine, which a numerical record has no field for) and the lock
policy (the run's).

**Files.** `record.hpp` (and the wire: below), `isolation.hpp`, `worker/verified_run.cpp`,
`controller/rerun.{hpp,cpp}`. **Proofs:** `Prepare.TheRunnerIsToldWhereTheUnitRuns...` (resolved, not
0), `RealRerunner.TheProcessIsToldTheUnit...` (the work and the record disagree on purpose; the
record's wins), `...AFindingWithoutTheDecomposition...IsNotRerun...`, and through the real worker
and channel `IsolationLadder.AFindingNamesTheDecomposition...`.

## 4. logical_cpu is one CPU, a unit is spread over its threads

**Question.** Are multi-threaded units isolated, and on which CPU set?

**Options.** (a) Carry the set (`cpus`) and isolate on it: rung 1 on the same set, rung 2 on a
disjoint set of the same size, rung 3 single-threaded on ... which of them? A fault on the third
core of the set would be clean when the unit is repeated single-threaded on the first, and the
ladder would call it "concurrency-dependent: could be a LoadForge bug", a false statement about
the hardware. Doing it properly is a bisection over the set (each CPU alone, then pairs), a new
design of the ladder, in the Isolator's own contract. (b) Do not isolate a unit spread over several
CPUs; state it in the contracts; keep the finding and say why. (c) Isolate on an arbitrary one of the
set. Wrong in the same way as (a)'s third rung.

**Choice: (b).** What exists is correct and was only unstated. `record.hpp` now says a provenance
holds ONE place and the four place fields are set only when the whole unit ran in one (every thread
on that CPU; the package, core and node from that CPU's place in the topology), and an unset field is
absence. `isolation.hpp` says a `Placement` has one CPU and a unit on several has none, and a caller
states the absence on the finding. The controller's glue (`plan_worker` sets them for a single CPU;
`isolate_findings` says "the worker ran on several CPUs, so the record names none to repeat it on")
is now the contract's behaviour, not a workaround. The cost is real and recorded: `threads = "all"`
runs, verifies and isolates nothing, and `--test` runs one single-threaded worker per CPU for that
reason. **Follow-ups:** a ladder for a set (bisection), which would add a `cpus` list to the record;
`numa_node` for a worker whose CPUs share one node (today unset, so the working set is not bound).
**Proof:** `IsolationLadder.AUnitSpreadOverSeveralCpusIsNotIsolatedAndNamesNoPlace` through the
real binary, workers and re-runner: no re-run, the stated reason, no place in the record, the unit
still named.

## 5. physical_core is a number, a core address is two

**Question.** Core ids repeat across packages.

**Options.** (a) State the meaning "within the package". (b) Carry the package: add
`physical_package`, and define the core as the pair. (c) One packed number (`topology::key`).

**Choice: (b).** (a) leaves the field unable to tell two cores apart on the machines where it
matters, and a hardware finding that names the wrong core of two is worse than one that names
none. (c) is a number nobody can read. `physical_core` is now "the core's id within its package",
`physical_package` is the package; the controller sets both from the topology's `CoreAddress`;
the console and JSON show both. **Proof:** `PlanWorker.ACoreIsAPackageAndACoreBecauseCoreIdsRepeatAcrossPackages`
(a snapshot of two packages whose cores are both numbered 0, built on disk and read by the real
snapshot code) and the ladder test above, through the real worker.

## The wire

Provenance gained `physical_package`, `decomposition`, `footprint_bytes`, appended to the
provenance fields. `kRecordFormatVersion` is **2**; a version-1 record is refused (`kVersion`).
Both sides of the codec are tested: the provider's byte-exact layout test, the reference codec the
contract suite is checked against, the worker's own model codec, and the fuzz generator. The result
schema is **3** (`docs/result-schema.md`). The worker protocol gained `--lock` and
`--physical-package` and frame 18.

## The lock allowance in CI

The five `prlimit` lifts (`ci.yml`, `sanitizers.yml`, `canary-2604.yml`, `portability.yml`,
`coverage.yml`) were there because the sweep (and the default footprint) could not lock under an
8 MiB allowance. **They go.** The sweep now maps with `kTry`, and the controller's default is `kTry`,
so the runs that were refused now run and say their pages were not locked. Measured here, as uid
65534 with no capabilities under `prlimit --memlock=8388608` (the command of CI's own reproduction):
the M2 integration binary passes (the sweep, 80 configurations, and the pipeline through the shipped
binary), and the whole debug `ctest` passes (exit 0; the three tests that need root skip). What this
does not show is ARM64 and Ubuntu 26.04, which are CI's: if one of them is red for want of the lock
the lift is a one-line revert, and the red would be a test that maps without a policy.

## Follow-ups

* The ladder for a unit on several CPUs (4); `numa_node` for a worker on one node's several CPUs.
* The output of `stress` says the pages were not locked only in the notes and the result; the
  console summary could say it.
* A `stress --config` has no key for the lock policy (the config schema is another component's
  contract): it runs the default, `try`.
* The project still needs an outside reviewer.

## The stop grace of a verified run (2026-10-22 01:34 UTC)

**Defect.** CI's TSan x86 job (2026-10-22 01:06 UTC), `loadforge stress --test dense_linear
--duration 6s --workers 2 --footprint 2MiB` on a 2-core runner, ended with "worker 0: sent signal 15",
"killed by signal 15", "0 of 2 worker(s) completed", verdict incomplete, and 4 units computed, 4
checked, 0 wrong. A verified unit takes about 1.5 s there; a worker sees the stop request only between
units, because it cannot beat from inside `compute()`; the controller's stop grace was the default 1 s,
so a worker mid-unit at the deadline was terminated. The healthy run was reported as failed. On main
the test passed only because the timing fell that way. The earlier change of the test to 6 s was a
wrong diagnosis (it moved the deadline, not the grace).

**Fix.** `allow_for_verified_units` in `src/main/cli.cpp` raises `grace.stop_ns` to
`kVerifyingStopGraceNs` (10 s) beside `stale_after_ns` (10 s) when an enabled registered workload is
present; null-workload runs keep 1 s and 2 s. The test is back to `--duration 2s`.

**Where else grace feeds in.** The telemetry timeline's `run_ns` already adds `grace.stop_ns`, so
it grows with the fix. The controller's final drain of a held channel waits one `grace.stop_ns`
(orchestration.cpp), which now is the longer one too, as it should be. The controller's own deadline
does not use the grace: it only decides when to ask for the stop, and escalation (`escalation_due`)
reads `grace`. The re-run and isolation path uses `RerunOptions::timeout_ns`, never `grace`, so it
needs nothing.

**Test.** The fake cannot easily run a parent-side worker through `run_stress`, so the allowances are
set by a public function the tests call directly and assert on the values the controller is handed.
