# M1 exit review

- **Date and time:** 2026-10-20 04:44 UTC.
- **Scope:** every exit criterion of `docs/IMPLEMENTATION.md` §4 M1, every transition of its
  process state machine, every row of its misbehaviour catalogue, and the milestone-wide
  obligation stated above the milestones (coverage, exclusions, ledger, mutation).
- **Branch reviewed:** `feature/m1-exit`, from `main` at `eb8cf5d`, with the four changes
  this review made (listed at the end).
- **Series:** exit reviews are dated and kept, one file per occurrence, in `reviews/`.
- **Reviewer:** the project's own author. No outside reviewer has looked at M1; the project
  still needs one.

## Verdict

| # | Criterion | Verdict |
|---|-----------|---------|
| 1 | Null workload for the configured duration, limits honoured | **Met** |
| 2 | A passing T7 test for every state-machine transition, controller-killed included | **Met**, after two gaps found here were closed |
| 3 | OOM-kill classified distinctly from a fault, asserted | **Met** locally; in CI through a new root step whose first run is the PR's |
| 4 | Every P2 path has an injected test; the obligation gate agrees | **Met** (audit below) |
| 5 | Config round-trips through TOML; every rejection tested for its message | **Met** |
| 6 | 100% line and branch, zero exclusions; ledger current | **Met** |
| — | Milestone obligation: mutation gate with zero unexplained survivors | **Not yet known** |

**M1 is not marked complete.** The six numbered criteria hold on this branch. What remains
is the mutation clause of the obligation every milestone exits under: the pinned Mull has
never swept `src/`. It cannot be run from this environment (the package host is refused by
the egress proxy, 403), the credential that works here cannot dispatch a workflow, and the
weekly job's first run is Monday 2026-10-26 03:17 UTC. M1 is complete when that run, or an
owner's dispatch of `mutation.yml` before it, reports zero unexplained survivors over
`loadforge_unit_tests`. Any survivor it reports is a test to strengthen first.

## What this review found and changed

1. **START to RUNNING had no implementation.** The first transition is "topology snapshot
   taken (before fork)"; PLAN §4.1 has the controller discover the machine once before it
   forks, and the ledger said the rule was the controller's to honour. `loadforge stress`
   forked without describing the machine at all. Fixed in `b5295b0`:
   `topology::take_snapshot` (CPUs, caches, NUMA, whole or not at all), taken by the stress
   command before the run, refused on a `/sys` that will not describe the machine, carried by
   `RunOptions::topology` into the plan and the banner. Journal §2.25.
2. **The OOM row had no end-to-end test** (known; ROADMAP item 10). Added in `de13fef`: a
   fake worker really OOM-killed under a 256 MiB memory cgroup the test makes and moves
   itself into, reported `kOomKilled` beside a `SIGSEGV` fault and a completion. Journal
   §1.25, §2.24.
3. **The catalogue row "exit just before the deadline stop" had no T7 test.** It had a unit
   test over the shared fake, whose `EPIPE` model is the one ADR 0003 records as having hidden
   the bug, and a real-binary run that shows the fix only statistically. Added in `3549da4`:
   a worker that closes its control pipe, so the stop byte meets a pipe with no reader on
   every run. With `SIGPIPE` left at its default the test process dies by signal 13, every
   time. Journal §2.26.
4. **`SupervisionTest.AControllerKilledLeavesNoWorkerBehind` was flaky.** Found by repeating
   the supervision tier 30 times under `ctest -j4`: a controller killed before a worker's
   started record left that worker exiting 119 ("could not report"), which the test did not
   accept. The fix is in the worker, not the test (`438af86`): `EPIPE` on the record pipe means
   the reader is gone, which is exit 124, controller lost; every other refusal stays 119.
   Journal §2.27.

## Criterion 1 — the null workload for its duration, limits honoured

- Duration: `SupervisionTest.TwoWorkersCompleteForTheConfiguredDuration` (two real workers,
  elapsed ≥ 600 ms, both completed with their finished records);
  `ControllerTest.TwoWorkersRunToTheDeadlineAndAreAskedToStopAndComplete` (the stop byte at
  the deadline, exact tick accounting); `cli_stress_runs` (the shipped binary, "2 of 2
  worker(s) completed").
- Limits: `LimitsTest.TheDurationIsAlwaysEnforcedAndTheOthersSayWhatTheyAre`,
  `LimitsTest.MemoryIsCheckedAgainstTheBudgetBeforeAnyWorkerStarts`,
  `ControllerTest.RefusesBeforeSpawningWhenTheConfigurationCannotBeRun`,
  `Cli.RunStressChecksMemoryAgainstTheBudgetItReads`,
  `Cli.RunStressSaysWhyMemoryIsNotEnforcedWhenTheBudgetCannotBeRead`, and
  `cli_stress_enforces_memory` (the real binary on the real machine says the memory limit is
  enforced). The temperature limit is reported NOT ENFORCED until M3, which is the honest
  form of honouring a limit that cannot yet be measured.

## Criterion 2 — every transition has a T7 test

| Transition | T7 test (`tests/supervision/`) | Other tiers |
|---|---|---|
| START → RUNNING: topology snapshot before the fork | `SupervisionTest.TheMachineIsDescribedOnceBeforeTheFirstWorkerIsForked` (new: this machine's snapshot in the plan before the spawn of two real workers; CPU count against `sysconf`) | `ControllerTest.ThePlanDescribesTheSnapshotItWasGivenBeforeAnyWorkerExists`, `Cli.StressRefusesAMachineItCannotDescribeBeforeAnyWorkerStarts`, `SnapshotTest.*`, `cli_stress_describes_the_machine` |
| RUNNING → WORKER: spawn | every test in the tier; `SupervisionTest.TwoWorkersCompleteForTheConfiguredDuration` | `WorkerPoolTest.*`, `WorkerLauncherTest.*`, `RealWorker.*` |
| worker exit 0 → COMPLETE | `MisbehaviourCatalogue.ExitZeroWithItsRecordIsCompletionAndLeavesNoZombie` | `ClassifyTest.ACleanExitIsCompleteOnlyWithItsFinishedRecordInHand` |
| SIGSEGV / SIGBUS (and SIGABRT) → FAULT, classified | `MisbehaviourCatalogue/FaultSignal.IsAFaultNamingTheSignalWithTheTimelineUpToIt/{segv,bus,abrt}` | `ClassifyTest.SignalsAreFaultsKillsOrTheControllersOwn` |
| OOM-killed → OOM, classified distinctly | `MisbehaviourCatalogue.AWorkerOomKilledUnderALimitIsOomKilledAndNotAFaultOrAKill` (new) | `OomClassifyTest.*`, `JudgeOomTest.*`, `ControllerOomTest.*`, `RecordRealTest.*` |
| hangs → TIMEOUT (heartbeat stale) | `MisbehaviourCatalogue.ASilentHangHoldingItsDescriptorsIsTimedOutByItsHeartbeat`, `MisbehaviourCatalogue.ASpinWithoutProgressIsTimedOutWhileASlowWorkerCompletes` | `ControllerTest.AWorkerWhoseBeatGoesStaleIsTerminatedThenKilledAndTimedOut` |
| → telemetry preserved, run reported | asserted in every row above: records and progress up to the death (`FaultSignal` asserts progress read before the fault and the started record) | `ControllerTest.AFaultIsReportedWithTheRecordsThatArrivedBeforeIt` |
| controller SIGKILLed → every worker gone within a bounded time | `SupervisionTest.AControllerKilledLeavesNoWorkerBehind`; `MisbehaviourCatalogue.AWorkerDeafToItsControlPipeIsEndedByTheParentDeathSignal` (the only test the death signal alone can pass) | `RealWorker.TheDeathSignalIsActuallyArmedInARealChild` |

Every case runs N ≥ 2; the grandchild case runs N = 3; the OOM case runs N = 3.

The rest of the misbehaviour catalogue, each a T7 test against the fake worker: exit
non-zero (`ExitNonZeroIsAWorkerErrorAndNotAFault`), close the record pipe early
(`ARecordPipeClosedEarlyIsTruncatedEvidenceDespiteExitZero`), exit just before the deadline
stop (`AStopByteToAPipeNobodyReadsIsEpipeAndNotDeathBySigpipe`, new), a truncated record
(`ATruncatedRecordIsRejectedAndTheEvidenceIsTruncated`), a bad checksum
(`ABadChecksumIsAChannelCorruptVerdictOverAnExitZero`), ignore SIGTERM
(`AWorkerIgnoringSigtermIsKilledOneGraceAfterIt`), fork a grandchild then die
(`GrandchildrenOfDeadWorkersAreKilledWithThem`). "Die holding a process-shared lock" has no
lock to hold, and `tools/check-test-obligations.py` checks on every run that `src/` holds
none undeclared. D-state is absent by design (§4 M1 says why).

*Recorded, not closed (journal §2.20, §6):* when the controller itself is killed, a
grandchild of a worker survives, because the death signal is cleared across fork and nobody
is left to sweep the group. The transition's subject is the workers, and every worker is
gone; the real worker forks nothing, so nothing is exposed. The first workload that forks
must close it.

**Repetition.** The whole supervision tier plus the real-binary stress tests (25 tests)
passed 30 of 30 repetitions under `ctest -j4` (750 of 750) after the fixes above; before the
worker fix, one repetition in thirty of the controller-killed test failed, which is how
finding 4 was made.

## Criterion 3 — OOM classified distinctly from a fault

- End to end, real kernel, real record:
  `MisbehaviourCatalogue.AWorkerOomKilledUnderALimitIsOomKilledAndNotAFaultOrAKill`. The
  test makes a memory cgroup limited to 256 MiB with swap off, moves itself into it so the
  workers are born there, makes the OOM record as `loadforge stress` makes it (from
  `/proc/self/cgroup`) and asserts its scope is the new cgroup's count. Worker 1 raises its
  `oom_score_adj` to 1000 and allocates until the OOM killer ends it; worker 2 dies by
  `SIGSEGV`; worker 0 completes. Asserted: `kOomKilled` with "killed by signal 9", the
  controller's note naming the scope and "counted 1"; `kFault` for the `SIGSEGV` with no OOM
  note; the count 0 before and 1 after. Falsified: with the table's OOM row inverted, it fails
  (and so does its companion).
- The other half: `MisbehaviourCatalogue.ASigkillFromOutsideIsAKillWhenTheRecordCountedNone`
  (the same kind of record, a `kill -9` from the test, `kKilled` and "counted no OOM kill").
  It needs only a readable count, so it runs under `ctest` wherever there is one.
- Repetitions: 30 of 30 alone on the debug build, 30 of 30 inside the tier under `ctest -j4`,
  and 30 of 30 under each of ASan and TSan (Clang 18, as CI builds them), together with its
  companion and the stop-byte row.
- **In CI.** The hosted runners are cgroup v2 and run tests as an unprivileged user, so
  under `ctest` the OOM test skips and quotes the kernel's refusal (checked here by running
  it as `nobody`). `ci.yml` now runs it once more on all four matrix entries under `sudo`
  with `LOADFORGE_REQUIRE_OOM_LIMIT=1`, which turns a skip into a failure, and refuses a
  filter that matched nothing. The v2 branch of the test's cgroup guard (a sibling of its
  own cgroup, then a child of the hierarchy's root) has not run on a v2 host before that
  step's first run, because none was available; if it is wrong the step fails loudly, which
  is the intent.

## Criterion 4 — every P2 path injected; the gate agrees

A P2 path is read here as a distinct arm of handling at a call site: each errno the code
treats differently is its own path, and the errnos it treats alike share one. Where an errno
is classified by a pure function (`classify_refusal`, `classify_placement`,
`Source::classify`, `exit_for_unsent`), the function is tested row by row and each arm is
reached through the seam at least once. "Every documented errno" lists are used where the
code promises to carry any errno through verbatim. Every failing call above the seam is
listed; the calls that cannot fail (`getpid`, `getppid`, `setsid` in a fork's child) are
documented as such in `syscalls.hpp` and have no failure arm to test.

| Call site | Arms | Injected by |
|---|---|---|
| `platform/fs` `open` | any errno, carried with the path | `OpenFailure.IsReportedWithTheCallAndThePath` (each documented errno), `FileSystemTest.FirstLinePropagatesTheUnderlyingFailure` |
| `platform/fs` `read` | EINTR retried; EINTR bound; any other errno after a partial read; the size limit (EFBIG, ours) | `FileSystemTest.RetriesAfterEINTRRatherThanFailing`, `.AbandonsAReadThatIsInterruptedWithoutEnd`, `.TheInterruptBoundCountsConsecutiveRunsNotTheWholeRead`, `ReadFailure.IsReportedAndTheDescriptorIsStillClosed`, `.PartialContentIsDiscardedWhenTheReadFails`, `.RefusesAFileLargerThanTheLimitRatherThanTruncating` |
| `platform/fs` `close` | reported after a good read; not retried on EINTR; outranked by a read failure | `FileSystemTest.ACloseFailureAfterAGoodReadIsReported`, `.CloseIsNotRetriedWhenItIsInterrupted`, `.AReadFailureOutranksACloseFailure` |
| `platform/clock` `clock_gettime` ×3 | any clock failing fails the reading | `ClockTest.AFailureFromAnyOneClockFailsTheWholeReading`, `.AnUnsupportedClockIsReportedRatherThanSkipped` |
| `platform/affinity` `sched_getaffinity`, `sched_setaffinity` | ESRCH; EINVAL (cpuset model); EPERM; unexpected | `AffinityTest.PermittedPropagatesAFailure`, `.AMaskEntirelyOutsideTheCpusetIsRefusedAsNoUsableCpu`, `.AVanishedThreadIsReportedAsGoneNotAsABadMask`, `.AnotherUsersThreadIsNotPermitted`, `.AnUnexpectedErrnoIsCarriedThroughVerbatim` |
| `platform/memory` `mmap`, `mbind`, `madvise` ×2, `mlock`, `munmap` | each stage named; ENOMEM, pool exhaustion, ENOSYS, EINVAL, RLIMIT_MEMLOCK (EAGAIN/ENOMEM, EPERM), EFAULT; unmap reported at release | `MappingTest.AMapFailureIsClassifiedAndCarriedThroughFieldByField`, `.ExplicitHugePagesComeFromThePoolInWholePagesAndGoBackOnUnmap`, `.AKernelWithoutNumaIsReportedAsUnsupported`, `.AFailedAdviceUnmapsAndNamesTheAdviseStage`, `.AFailedPopulateIsThePopulateStageNotTheAdviseStage`, `.ALockPastTheAllowanceIsExhaustedAndALockWithNoAllowanceIsNotPermitted`, `.AnUnexpectedLockErrnoIsCarriedThroughVerbatim`, `.ReleaseReportsTheUnmapAndDisarmsTheDestructor`; the classifier row by row |
| `platform/process` `fork`, `prctl(PR_SET_PDEATHSIG)`, `fcntl`, `execv`, `readlink` | every documented errno of each; the orphan race; truncation and the empty link | `WorkerLauncherTest.EveryDocumentedForkErrnoIsCarriedThrough`, `.AFailedDeathSignalStopsTheWorkerBeforeItExecs`, `.ASupervisorThatDiedInTheRaceWindowIsDetected`, `.ADescriptorThatCannotBeHandedOverStopsTheWorkerBeforeExec`, `.EveryDocumentedExecErrnoIsKept`, `ExecutablePathTest.EveryDocumentedReadlinkErrnoIsCarriedThrough`, `.ATruncatedLinkIsRefusedRatherThanReturnedShort`, `.AnEmptyLinkIsRefusedRatherThanReturnedAsAPath` |
| `platform/worker_pool` fork unwind, `waitpid`, `waitpid(WNOHANG)`, `kill` | partial spawn unwound; EINTR retried and bounded; ECHILD; other errno; ESRCH tolerated; first real error kept | `WorkerPoolTest.AFailedForkKillsAndReapsTheWorkersAlreadyStarted`, `.AForkThatFailsFirstLeavesNothingToCleanUp`, `.TheUnwindStopsRatherThanSpinningWhenReapingFails`, `.ReapingRetriesOnEintrAndKeepsTheDeathThatFollows`, `.AnEndlessEintrStormIsAbandonedRatherThanSpunOn`, `.NoChildrenLeftIsReportedAsEchildRatherThanAsSuccess`, `.AnUnexpectedWaitErrnoIsReportedAsItself`, `.ReapingWithoutWaitingReportsNothingThenOursAndSkipsStrangers`, `.EsrchOnOneWorkerIsARaceAndNotAFailure`, `.TheFirstRealErrorIsTheOneReported` |
| `worker/ipc` heartbeat `memfd_create`, `mmap` (create, attach), `munmap`, `close` | create fails; map fails and the file is closed; attach of a non-region; release reports unmap and still closes, reports close | `HeartbeatRegionTest.AFailedMemoryFileIsReportedWithTheKernelsWords`, `.AFailedMappingClosesTheFileItCreated`, `.AttachRefusesADescriptorThatIsNotARegion`, `.ReleaseReportsAFailedUnmapButStillClosesTheDescriptor`, `.ReleaseReportsAFailedClose` |
| `worker/ipc` record `write`, `read` | short writes; EINTR retried and bounded; a write of nothing; EPIPE; read EINTR bounded, EAGAIN, EBADF | `RecordWriterTest.WritesTheWholeFrameAcrossShortWrites`, `.RetriesInterruptionsUpToTheBoundAndReportsTheBound`, `.AWriteThatTransfersNothingIsASendFailureNotASpin`, `RecordSourceTest.ReadFailuresAreReportedAndInterruptionsRetriedUpToTheBound` |
| `worker` entry and loop: `sigaction`, attach, started and finished records, placement, `poll`, control `read`, clock | each its exit code and message; EPIPE on a record is controller lost (new) | `WorkerRunTest.SetupFailuresExitWithTheirOwnCodeAndSayWhich`, `.ARecordPipeWithNoReaderIsTheControllerLostNotASetupFailure`, `NullWorkloadTest.EveryFailureNamesItsStage` |
| `controller` every call in prepare, spawn, the loop and finish | `sigaction`; `memfd_create`; clock at every read; either pipe; fork mid-spawn; either close of a child's end; `poll` in the loop and in the final drain; drain `read`; reap; `kill` (ESRCH tolerated, EPERM fatal); the stop byte (EIO fatal, EPIPE tolerated); the group sweep (found, ESRCH, EPERM recorded); release; the OOM count unreadable at either end | `ControllerTest.EveryFailureBeforeTheLoopNamesItsStageAndLeavesNothingBehind`, `.EveryFailureInsideTheLoopNamesItsStageAndKillsTheWorkers`, `.FailuresAtTheEndAreReportedAfterTheWorkersAreGone`, `.AClockThatFailsDuringTheFinalDrainIsTheControllersFailure`, `.AWorkerWhoseBeatGoesStaleIsTerminatedThenKilledAndTimedOut`, `.WhatAWorkerLeftRunningInItsGroupIsKilledAndRecorded`, `.AGroupThatCannotBeKilledIsRecordedAndTheRunGoesOn`, `ControllerOomTest.ACountUnreadableAtEitherEndLeavesAKillWithTheReason`; EPIPE at the stop byte for real in `MisbehaviourCatalogue.AStopByteToAPipeNobodyReadsIsEpipeAndNotDeathBySigpipe` |
| `topology` and `resources`, every source through `Source` | ENOENT/ENODEV/ENXIO absent (vanished once seen), EACCES/EPERM denied, everything else unreadable; per source | `SourceTest.*` (each errno group, through the seam and, for ENOENT, EISDIR and ENOTDIR, the real kernel); each resources source denied through the read script in `cgroup_test.cpp`, `memory_budget_test.cpp`, `cgroup_oom_record_test.cpp` |
| `main` the stress command | the executable link; the snapshot (new) | `Cli.StressRefusesToRunWhenItCannotNameTheWorkerExecutable`, `Cli.StressRefusesAMachineItCannotDescribeBeforeAnyWorkerStarts` |

`config::load` uses C stdio, outside the seam by PLAN §4.2. Its two failure arms ("cannot
open", "cannot read") do not distinguish errnos and are each reached by a real failure:
`ConfigFileTest.AMissingFileIsRefusedAsUnopenable` and
`ConfigFileTest.ADirectoryOpensButCannotBeRead`. Its ledger row declares no P2 and says why.

The gate: `tools/check-test-obligations.py` reports 10 modules ok; every module whose code
has a failure arm behind the seam declares P2 and a T2 tier that has tests; no process-shared
lock in `src/`.

No P2 gap was found beyond the ones fixed above (the record pipe's EPIPE arm in the worker
is new, and tested).

## Criterion 5 — config round trip and every rejection's message

`ConfigRoundTripTest.WhatIsWrittenParsesBackToTheSameConfiguration`,
`.TheRenderingIsTheDocumentAReaderExpects`, `.EqualityNoticesEveryField`; every
`ConfigRefusalTest.*` asserts the message and the line (unknown keys, missing and mistyped
values, each value domain and its boundary, the second workload's fault naming the second
workload); `ConfigFileTest.*` for the file itself; `cli_check_config_ok`, `_missing`,
`_refused` through the binary.

## Criterion 6 — coverage, exclusions, ledger

On this branch after all four changes: lines 100.0% (4403 of 4403), functions 100.0% (882 of
882), branches 100.0% (3995 of 3995); `check-coverage-complete.sh`: every source file under `src/` in
the report; `tools/check-exclusions.sh`: 0 markers; the ledger gate: ok, with the controller,
worker, topology and resources rows updated for what changed.

## Other gates run locally

`ctest --preset debug` (all tests), clang-format 18 over every changed file, clang-tidy 18
over every changed file in `src/` and the fake worker, and the full suite under the `asan`
and `tsan` presets built with Clang 18 (all pass; the three `mlock` tests that skip under the
sanitizers skip as before).

## Noted for the owner, not changed

- Several documents say CI runs the tests as root (journal §6 on `mlock`; the platform
  ledger row on `RLIMIT_NPROC`; the affinity note). The workflows run `ctest` as the runner
  user and use `sudo` only to install packages. Not checked against a CI log from here;
  worth confirming, since it decides which of those statements' consequences hold.
- ADR 0003's cross-execution of the OOM-record contract (the reader's contract tests against
  the controller's fake) waits for ROADMAP 12a, as journal §2.23 says.

## Changes made by this review

- `de13fef` Test the OOM row of the misbehaviour catalogue end to end
- `b5295b0` Take the topology snapshot before the first fork
- `3549da4` Test the stop byte to a pipe nobody reads as a T7 row
- `438af86` Report a record pipe with no reader as the controller lost
