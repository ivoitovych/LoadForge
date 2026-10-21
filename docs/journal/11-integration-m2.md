# Journal fragment 11 — the M2 integration

**Written:** 2026-10-20 22:10 UTC
**Change:** `src/main/`, `src/controller/` and `src/worker/` (top-level files),
`src/workloads/registry/`, `src/verification/oracle/registry/`; `tests/supervision/`,
`tests/integration/m2/`, the unit tests of those modules, the ledger rows of all of them
(stream 11, wave 2).

Measured on an x86-64 container: Linux 6.18.44, glibc 2.39, GCC 13.3, Clang 18.1.3, four
vCPUs of an Intel Xeon at 2.80 GHz (AVX2 and AVX-512 both present), running as root with
an 8 MiB `RLIMIT_MEMLOCK`. The ARM64 half of each claim below is CI's.

The point of this stream is the first run of the component model: workloads, oracles, the
runner, the executor, the isolation ladder and the controller were each built from a
contract by someone who never read the others. They fitted on the first try in the sense
that `loadforge stress --test compression|dense_linear|monte_carlo|sort` ran, checked
every unit by its oracle and reported nothing wrong, with no change to any of them. The
places where they did not agree, or where a contract left a gap that the integration had
to fill, are the next section. Everything else in this file is how the join was made.

## Contract disagreements found

Each paragraph names the components and the clauses, says what was observed when the real
ones ran together, and says what the integration does about it. Where a test in
`tests/integration/m2/disagreements_test.cpp` shows it, the test is named. None of the
five is a component failing its own contract; each is two contracts that are each
complete and do not compose, or a contract silent where its neighbour needs an answer.
**No component is wrong against its contract**, so no failing test is filed against one.

1. **`simd::select_path` (dispatch.hpp) against `Descriptor::paths` (workload.hpp).**
   `select_path(std::nullopt, supported, compiled)` is "the first path in `kPreference` both
   sets hold", and its signature has no way to hear what a workload has kernels for. The
   worker is told to choose "once", before the workload exists; `dense_linear` has no
   AVX-512 kernel and refuses the configuration (`kConfiguration`). On this machine, which
   has AVX-512, the path the contract chooses is one `dense_linear` refuses
   (`Disagreements.TheUnrequestedPathSelectIsNotAlwaysOneAWorkloadHas`). Journal fragment 06
   predicted it (its question 2); this is the observation. Because the path is also needed
   to create the workload that would say which paths it has, the controller creates each
   workload twice: once for scalar, which every workload has, to read `descriptor().paths`,
   and once for the path it chose from the three sets (build, machine, workload), to have the
   configuration's refusal before the fork rather than in a worker after it
   (`controller/plan.cpp`, `choose_isa`). **Needs:** a way to ask a registry what paths a
   workload has without creating one (`Registry::descriptor(name)`), or `select_path` taking
   the workload's set.

2. **`RunnerSetup` (runner.hpp, "kResources") against the runner's decision to lock the
   whole working set (journal fragment 02, question 1).** The contract has no memory policy,
   so a process that may not lock the footprint cannot run: an unprivileged user's default
   allowance is 8 MiB, and `loadforge stress --test compression` with its default 32 MiB
   footprint ends, in every worker, with `runner resources: working set: cannot map 33586944
   bytes, ordinary pages, populated, locked ... mlock(...): Cannot allocate memory`
   (reproduced as uid 65534 with `setpriv`, and by
   `Disagreements.AnUnprivilegedProcessCannotLockTheWorkingSetTheRunnerAlwaysLocks`, which
   drops to that uid in a child). `--footprint 7MiB` runs. The refusal is clean and names the
   cause, so the integration leaves it; the default footprint stays at 32 MiB because a
   footprint inside the cache tests nothing about memory. **Needs:** a lock policy in
   `RunnerSetup`, chosen by the controller and recorded with the run (journal 02's proposal),
   so that an unprivileged run can choose not to lock and say so.

3. **`Rerunner` (isolation.hpp, "the unit is the finding's provenance: workload,
   workload_version, iteration, seed, isa") against `Descriptor::determinism` (workload.hpp,
   `kBitExactFixedDecomposition`).** The input of a unit is a function of those five. Its
   OUTPUT is also a function of the decomposition, which the provenance does not carry:
   `dense_linear` at panel depth 16 and at 256 gives different bits for the same unit
   (`Disagreements.AUnitsOutputDependsOnADecompositionTheProvenanceDoesNotCarry`), both within
   tolerance, so a re-run that took the unit from the record alone and used the default would
   check a different output from the one the finding is about. The same holds for the
   footprint (not for the bits, but for the slot a unit sits in). The integration fills it
   from the run: the controller knows each worker's decomposition and footprint
   (`WorkerWork`), and `ChildAwareRerunner::use()` tells the re-runner which run the next
   findings came from. **Needs:** the decomposition (and the resolved one, not the requested
   0) in `Provenance`, or in the `Rerunner` contract's words.

4. **`Provenance::logical_cpu` (one CPU) against the executor (one unit spread over every
   thread) and `Placement::cpu` (isolation.hpp, "the logical CPU it ran on").** A unit
   computed by an executor of several threads ran on all of their CPUs; a record has room for
   one. The runner is given a `base` provenance and sets what it likes; the integration sets
   `logical_cpu` only when the worker's threads are all on one CPU, and otherwise leaves it
   unset, as record.hpp asks ("a producer leaves unset what it does not know"). A finding with
   no CPU cannot be isolated: the ladder's first rung is "the original CPU". The report says
   so, in words, on the finding (`FindingReport::not_isolated`: "the worker ran on several
   CPUs, so the record names none to repeat it on"). The consequence is that `--test` runs one
   single-threaded worker per CPU, each pinned to its own, because that is the shape in which
   every finding can be isolated; a configuration with `threads = "all"` runs and verifies and
   isolates nothing. The concurrency-dependent rung is reachable only when the worker's
   threads share one CPU, and the T7 test builds exactly that. **Needs:** a decision whether a
   multi-threaded unit is isolated at all, and if so on which CPU set (the rung would need a
   `cpus` list).

5. **`Provenance::physical_core` (one `uint32`) against `topology::CoreAddress` (package and
   core).** Core ids repeat across packages (core 0 of package 0 and of package 1 are
   different cores). The integration records `core.core`, so on a two-package machine two
   cores share a number. The isolation stream's `other_cores` is computed from the snapshot
   with the full address, so isolation is not affected; the record's field is. **Needs:** the
   field to carry the package or the pair, or its meaning stated as "within the package".

**One defect that is a component's own, found by the build and not by a contract.**
`verification::provenance_of` (`src/verification/record.hpp`, verification core) ends in
`return std::get_if<VerificationNotPerformed>(&record)->provenance;`, an unchecked
`get_if`. Inlined into a consumer at `-O3` with GCC 13 and `-Wnull-dereference -Werror` (the
`static` and `release` presets' flags), it is `error: null pointer dereference` at the
consumer's call (here `controller/orchestration.cpp`, `take_error_record`); the debug and
sanitizer builds compile it. It is not a wrong answer today (the variant has three
alternatives and the first two are tested), so no runtime test can fail on it; the evidence is
the command that fails, in `build/static` before this stream read the provenance through
`std::visit` instead. `provenance_of` is left as it is (not this stream's file); the
controller no longer calls it. `controller/results/document.cpp` and `controller/isolate.cpp`
call it too and happen to compile. **Needs:** a visit, or `std::get<>`, in the provider.

Smaller readings that differ and agree on every test either side has:

* **`finish()`'s records and provenance.** The contract does not say whether the two records
  a finished run adds ("oracle over X units", "input-integrity bracket over X units") carry
  the base provenance. The provider's runner puts the base provenance on them (workload,
  version, seed, ISA, CPU); this stream's model of the runner first left them empty, and the
  provider's suite passed both. The controller accepts either (a record whose provenance names
  no workload is the worker's own) and refuses one that names another workload.
* **`Tally::oracle_checked` for a unit the oracle answered `NotChecked`.** Counted as checked
  by both readings ("units given to the oracle"), with its blocks at 0, which is why
  `oracle_blocks` and not `oracle_checked` is what says whether anything was compared.
* **A run's heartbeat against the length of a unit** (see Decisions, "A hang is ten
  seconds"): runner.hpp says a slice's time is proportional to its units and "the worker sizes
  slices from measured time to keep its heartbeat fresh"; workload.hpp says nothing bounds a
  unit. One unit is the smallest slice, so a unit longer than the controller's staleness
  allowance (M1: two seconds) is a healthy worker called a hang. A sort unit is 0.4 s in this
  debug build and would be ten times that under a sanitizer.

## Established by experiment

### A configuration the workload accepts, its oracle and the runner agree on, everywhere

`tests/integration/m2/sweep_test.cpp` creates every workload with every decomposition it
lists (and 0, the default), on every ISA path this machine and build can run, hands the
oracle registry the plan the workload resolved, and runs two units through the real
input-integrity runner on one thread and, for the default decomposition, on three, from unit
0 and from unit 2^64 − 2 (the second unit of which wraps the index). 80 configurations (the count the test prints)
in this build; every one ran with no record, every oracle compared at least one block, and
the contract each oracle holds is the one its workload declares. Where a workload has no
kernel for a path it said `kConfiguration` and nothing else. No disagreement was found.

### The registers are asserted twice, and a change in between is reported, not repaired

The worker establishes the required state, asserts it, makes the pool, and asserts it again
after the workload, the oracle and the executor exist. On this machine all of that agrees on
every run (`stress --test` banner: rounding to nearest, flush-to-zero off, denormals-are-zero
off). A *change in between* is the only way the second assertion fails, and the unit tests
drive it by an oracle registry that dirties the registers when it creates an oracle: the
worker reports a configuration error, `floating-point environment: changed during start-up`,
and does not repair it (journal fragment 07, question 3 left the choice to this stream).

### The isolation ladder against really re-run workers

`tests/supervision/ladder_test.cpp`: two workers on two CPUs run a compression workload that
corrupts every fifth unit's output in the way `LOADFORGE_FAULT` names, the controller collects
the records, and the real re-runner executes the same program once per rung, pinned where the
rung says. Corrupting everywhere: same core yes, another core yes, single thread skipped
(the original ran one) — not core-local, high confidence. Corrupting only on the first
worker's CPU: same core yes, another core no — core-local, and the second worker finds
nothing. Corrupting only in the original run: the first rung is clean — a non-reproducible
transient, and the ladder stops. Corrupting only with several threads, both workers' two
threads on one CPU: same core yes, single thread no — concurrency-dependent. Each ends in the
finding the ladder's table gives, and none of them could be reached by a fake.

### A persistent fault fills the record pipe faster than one read a tick empties it

With one `read(2)` per worker per tick, a worker that finds a wrong result in every unit
writes records faster than the controller reads them, fills the 64 KiB pipe, blocks in
`write(2)` inside `run_slice`, stops beating, and is then called a hang by the controller
that was not reading. The controller therefore drains a woken channel until `poll` says it is
empty, at most 64 reads a tick (`kMaxReadsPerTick`); the cap exists so that a channel that
never stops cannot hold the loop. Seen in the unit tier with 259 records queued; the
supervision tier's faulty worker, at one record in five units, never fills the pipe.

### The CLI over the fake of the system calls spent 25 seconds in the hwmon scan

`run_stress` with the capability probe turned on, over `FakeSyscalls`, took 25 s per test.
The fake answers every path with the same file, and the thermal source walks every chip
number to a bound, reading `temp<N>_label` for each; over the real `/sys` the same scan is
milliseconds. `StressOptions::capabilities` is false in the tests that run over the fake. It
is the fake that is wrong about the kernel here, not the source (journal fragment 09 has the
real-tree tests).

### A bug of this stream's own that a test caught

`plan_worker` resolved a configured absolute footprint against an empty budget when the
memory budget could not be read, and refused every such run as "past the ceiling of 0". The
unit test that planned a worker with an unread budget found it. Fixed: with no budget an
absolute size stands and a share cannot be resolved, so the default stands.

## Decisions

### Where the join lives, and what is added to what

`worker::run` is the null workload's loop when it is given no `--workload` and
`worker/verified_run.cpp` when it is, so the supervision tier's fake worker and the M1
tests keep working unchanged. The six services a worker is built from (the two registries,
the registers, the thread starter, the ISA probe, the runner's factory) are an `Services`
struct of borrowed pointers: `ProductionServices` for the shipped binary, doubles for the
unit tests, and the real registries around a corrupting workload for the faulty worker of the
supervision tier. Nothing in the shipped binary can be told to misbehave: the corruption is a
test program (`tests/supervision/faulty_worker.cpp`), not a switch (`tools/check-no-test-
hooks.sh` still passes on the shipped binary).

### What a worker sends, and the tally frame

The record channel carries, besides the M1 text records, error records (frame type 16, the
verification-core codec) and tallies (frame type 17, new: version byte and seven counters,
little-endian). The tally is **cumulative**, sent after every slice, so a frame the controller
never read costs it a stale figure and not a wrong one, and the last frame is the answer. The
brief says "its tallies with its heartbeats"; the heartbeat slot is a fixed 64-byte seqlock
that already has a progress counter (units computed), and a tally of seven counters does not
fit it, so the tally travels in the frame of the same tick, not in the slot. Considered:
widening the slot (changes the M1 protocol and its tests for no reader that needs a lock-free
tally), and a second region (a second thing to attach and to fail). The frame costs a few
bytes a tick.

### Records are believed only when they cannot be garbage

An error record is kept only when the frame's checksum verified, the payload decodes to a
record the contract allows, and it is about the workload the worker was told to run; a tally
that is smaller than the last is not a running total. Each refusal closes the channel and
makes the worker `kChannelCorrupt` with no finding (exit criterion 3 of the brief: "a
hardware finding never comes from garbage on the record channel"). Tested by a table of seven
kinds of garbage, by a bit flipped in every byte position of a frame (checksum, header and
payload alike, 400 positions) and by 300 well-framed random payloads of every type the
controller reads; in none was a finding made of garbage.

### What the controller keeps, and what it isolates

It keeps the first 256 error records of each worker and counts the rest exactly. A machine
with a persistent fault makes a record a unit, for as long as it runs, and a report of a
million findings helps nobody. Isolation runs **after the workers are gone**, over the
report, at most 16 wrong results per worker (per worker so that a fault on one core does not
starve another worker's findings of theirs), one at a time: the re-runner reaps with
`wait_any`, which must not compete with the run it is about. A finding that is not isolated
says why (a check that was not made; the worker ran on several CPUs; past the limit; no
topology). A `VerificationNotPerformed` is never isolated (isolation.hpp, "only wrong results
enter"), by type and by test.

### What a re-run is, and what its ending means

A process of this binary with `--rerun`, pinned to the rung's CPU, with the rung's thread
count (all threads on that CPU), one unit, every check on. It answers by exit code: 0 clean,
20 reproduced, 21 not performed, 22 bad arguments. **Anything else — another exit code, any
death by a signal, a re-run that never ended and was killed after the timeout — is "not
performed", never "clean"**: a signal names a fault and not its cause (EXT-03), and the
ladder's most alarming finding is reached through a clean re-run, so only a re-run that was
made and passed may be one. A re-run whose workload version differs from the finding's, or
whose oracle answered `NotChecked`, is not performed. The wait is a count of 100 ms polls on
a pipe the child holds, not a clock reading; the child's death closes it.

### Placement: one worker per CPU, each pinned

`stress --test NAME` starts one single-threaded worker per CPU the process may use (up to 64,
or `--workers N`), each pinned to its own, because that is the shape in which a finding
names a CPU. The CPU list is what the kernel permits intersected with what the snapshot
found; when the kernel will not say, the snapshot's CPUs, and a withheld one is refused by
name when the worker is placed. A configuration file's `threads = N` gets N CPUs starting at
its slot times N, wrapping, and `all` gets every CPU; those workers verify and are not
isolated (disagreement 4). Each worker has its own seed, `seed + slot × 0x9E3779B97F4A7C15`,
so no two workers run the same units, and each is reproducible from the provenance of its own
records. The default duty cycle is every unit checked, oracle and bracket both; there is no
command-line or configuration surface for it yet (the duty cycle of F17 is M3's).

### A name that is not a registered workload is the null workload, as at M1

The M1 configuration used names as labels (`first`, `second`) and its tests and fixtures
still do. A name that is registered now runs that workload; any other still runs the null
workload, and the banner says so (`first: null workload`). `stress --test` refuses an
unknown name with the list of the registered ones. A typo in a configuration file therefore
runs the null workload and says so in the banner; making it an error would break the M1
fixtures and is M5's call, when the mixed workload reshapes the configuration.

### The cache-line size is the topology's, or the run is refused

The runner takes a line size and the records classify with it ("never an assumed 64").
The snapshot's caches may not report one (a virtual machine without cache information). The
controller takes the largest line any cache reports and refuses to start a workload when none
does, with the reason. An assumed 64 would classify every multi-bit error by a boundary
nobody measured.

### A hang is ten seconds for a verifying worker, two for a null one

`run_stress` raises the staleness allowance to ten seconds when any configured workload is
registered, for the reason in "Smaller readings". Two seconds stays for null workloads, whose
whole job is to beat. It is a constant, not a configuration key: how long a unit may take is a
property of the workload and the build, and a configuration key would be a thing a user
tunes to make a failure go away.

### The mutation gate takes every test executable, and results merge by mutant

`tools/mutation-gate.py` now accepts several test binaries and `--report-dir` more than once, and
`tools/mutation-shard.py targets` lists the shards by module. A mutant survives only if it
survives every executable that covers its line; a mutant killed by any one is killed. The merge
is covered by fixtures (`tests/tools/test_tooling.sh`, `merge-a/b/c`); a real Mull run was not
possible in this container, so the first full sweep is still the roadmap's item and is not claimed.

### Merging the two wave-1 branches

Both merged to main (#38, #39) while this stream ran, along with other streams' work (adaptive
mesh, raytrace, primitives, telemetry sources). `git fetch origin && git merge origin/main` with
a signed merge took them with no conflict; the build, the ledger gate and the whole debug suite
(3357 tests) pass on the merged tree. The two workloads that arrived (adaptive_mesh, raytrace)
are not in the registry here: the brief names compression and dense_linear, and registering a
workload is one table row and one oracle adapter for whoever integrates it.

### The exit code

0 clean; 3 a worker did not complete (M1's code, unchanged); **4** a wrong result was found
(outranks 3 and 5: it is the finding); **5** nothing wrong was seen and something was not
seen — a check was not performed, or a verifying worker's oracle or bracket compared nothing
(a run that checked nothing is not a pass). The verdict is the results module's
(`results::derive_verdict`), applied to the controller's report converted by
`cli::to_run_result`; the controller does not link the results module (the results link the
controller), so the conversion is in `main/outcome`. A run of nothing but null workloads has
nothing to verify and is 0 or 3.

### The selftest digest

`--selftest-digest` now folds the scalar outputs of two units of `compression` and of
`dense_linear` (unit 0 and unit 4097, seed `0x5E1F7E57`, 8 MiB, one thread) into the core
digest, whatever the machine's ISA. CI's portability job compares the scalar build's digest
across architectures; the unit test compares it against a three-thread computation of the
same units here. Two units, not three, because the dense scalar kernel in a debug build is
the digest's whole cost.

### The milestone suffix

`LOADFORGE_MILESTONE` is `m2`; the version string is `0.0.0-m2`.

## Contract questions

1. **`Registry` has no way to ask a workload's paths without creating one** (disagreement 1).
   Reading taken: create for scalar, read `descriptor().paths`, create again for the chosen
   path. Proposal: `Registry::descriptor(name)` or `Workload` creation independent of the path.
2. **`RunnerSetup` has no memory policy** (disagreement 2). Reading taken: the runner's own
   (lock always), the refusal surfaced as the worker's setup failure.
3. **`Provenance` lacks the decomposition and the footprint** (disagreement 3). Reading taken:
   the run's, supplied beside the record by the controller. The resolved decomposition would
   belong in the provenance of every record, next to the ISA path.
4. **A multi-thread unit has no CPU** (disagreement 4). Reading taken: unset, not isolated,
   said so.
5. **`physical_core` is one number** (disagreement 5). Reading taken: the core id within its
   package.
6. **`Rerunner` with `threads == 0`.** Journal 10 (question 2) asks whether this is a
   precondition. Reading taken: `kNotPerformed`, without starting a process.
7. **Where a worker that finds the control state untrustworthy ends.** `run_slice` answers
   `kFinished` after the runner stopped itself (journal 02, question 4). Reading taken:
   `StopReason::kRunnerStopped`, the final report is still sent, the worker exits 0 and the
   finished record carries the reason; the controller sees a `VerificationNotPerformed` and
   a verdict of incomplete (exit 5). The alternative, a distinct exit code, would let the
   controller call the machine's memory event the worker's setup failure.
8. **`workload_contract.hpp` and `oracle_contract.hpp` register two suites each.** The
   registries instantiate `RegistryContract` and `OracleRegistryContract` only and allow the
   other in their test files, as journal fragments 04, 05 and 09 already propose to avoid.
9. **`SelectPathContract` is described as "a consumer with its own stand-in".** The
   controller's three-set `choose_isa` and a stand-in written from the contract alone
   (`ModelSelect`) both pass it; the former is the one the controller uses.
10. **The contract corrections in progress** (`chore/contract-corrections-wave1`) would change
    nothing here that this journal depends on, except that question 6 and journal 10's
    questions 1 and 2 would settle the `Rerunner`'s edge cases, and EXT-01 (PR #79) would make
    the controller split a signal it attempted from one it established; the re-runner's
    treatment of a death by a signal as "not performed" already assumes that split.

## What was not done, and why

* **M2b exit criterion 5, a thermal result on real hardware (T11).** This container is a
  virtual machine with no hwmon chip, no thermal zone, no fan and no cpufreq; the capability
  report the run starts with says so for each (`not offered by this kernel`, with the reason
  and the remedy). Recorded as T11 in `tests/hardware/`, with the procedure and the evidence
  that this machine cannot produce the result. Open until someone runs it on hardware.
* **M2a exit criterion 6, the mutation gate over `verification/` with every test executable.**
  See the entry for the gate's change below; the first full sweep has not run (roadmap item
  10), which the brief names as the precondition.
