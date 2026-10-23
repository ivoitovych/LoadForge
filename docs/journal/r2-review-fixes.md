# Journal fragment r2 — confirmed findings of the internal review, controller and command line

**Written:** 2026-10-23 03:10 UTC
**Work item:** R2 of `reviews/reaction-plan-2026-10-23-0030-UTC.md`; verdicts in `reviews/internal-review-verdicts-2026-10-23-0030-UTC-a-c.md`
**Branch:** `fix/review-controller-cli`, from the head of PR #107 (`d091e66`)
**Change:** `src/controller/{safety,orchestration,report}.*`, `src/controller/journal/{file_ops,file_storage}.*`, `src/config/config.*`, `src/platform/syscalls.*`, `src/main/{cli,journal_rig,result_files}.*`; tests; `tests/obligations.d/main.toml` (a note). No contract header, nothing under `tests/contracts/`, none of the files R3 is editing, and none of `README.md`, `docs/safety.md`, `docs/architecture.md` was touched.

Each finding below was reproduced before it was fixed (the verdict file holds the evidence); each fix has a test that fails without it. Decisions are recorded with the options that were not taken.

## A3. The memory budget holds for the whole of a schedule

**Fix.** `check_memory` takes the run's schedule. It follows the footprints of the workers step by step (the starts, then every `Switch` replacing its worker's footprint), takes the largest total any step leaves them holding, and checks that against the ceiling and `limits.memory` as it checks the start total. The check is made again in `Run::prepare` after `mixed::problem` has accepted the schedule (the first check, before the floating-point environment, is kept for the configuration alone, so a refusal there reads as it did). The refusal names the step: `the schedule holds up to 260MiB (at the step "r1"), past the ceiling of 250MiB`. Test: `LimitsTest.MemoryIsCheckedForTheLargestTotalAnyStepOfAScheduleHolds` (unit) and `ControllerControl.AScheduleThatOutgrowsTheMemoryBudgetIsRefusedBeforeTheFirstFork` (the controller, no fork).

**Decisions.**
- *Where the worst case is computed.* Options: (a) in `mixed::` as `peak_footprint(Schedule)`; (b) in `check_memory`. Chosen (b): `src/workloads/mixed` is the schedule's, and the budget is the controller's; the controller reads the schedule as data (`Switch::footprint_bytes`) and needs nothing from the builder.
- *Worst case of every Switch, or the schedule's actual steps.* The review's smallest fix was `workers x footprint_bytes`. That is the bound for the mixed builder's options but not for a schedule in general, and it refuses runs that never hold that much (a schedule that gives one worker the whole footprint at a time holds one footprint, not `workers` of them). The schedule is data made before the run, so the exact largest total is known and is used.
- *The old runner and its replacement.* A worker frees the old footprint before it makes the new one (`verified_run.cpp`, `reconfigure`), so they are not held together and are not counted. The one exception is a refused `Switch`, where the old runner is made again: the same footprint as before. Said in `safety.hpp`.
- *Saturation.* A total past 2^64 saturates and is refused, not wrapped into room.

**Sentences R4 must change.** `docs/safety.md` §1 (the memory paragraph, lines 122-133 and 232 at the time of the verdict): "checked once before any worker starts" now includes "for the largest total any step of a schedule can hold"; the banner's `checked at start against a ceiling of ...` (from `limit_statuses`) is unchanged and still says "at start": it is the moment the check is made, which is still true.

## A4. The "not sent" label

**Fix.** The console and report line now reads `... 0 dropped (too many unanswered)` where it read `0 not sent`. `report.hpp` says what `dropped` counts, and that a command written to a pipe already closed is in no count. Test: the existing `ControllerControl.AFailureToWriteOnAControlPipeIsTheControllersAndAWorkerGoneIsNot` now asserts the whole line, and the pinned line in `AcknowledgementsAreCountedAndARefusalIsToldWithTheWorkersReason`.

**Decision.** Options: (a) count the EPIPE case in `dropped`; (b) rename the label. Chosen (b). `sent == 0` for a worker that is gone is pinned by an existing test with a stated reason (a worker gone is the reap's to report, its exit is the answer), and the field's documented meaning is "withheld because too many were unanswered". Counting an EPIPE as a drop would make `dropped` mean two things, and would count a command against a worker whose exit the report already shows. The label is the only part that over-promised.

**Sentences R4 must change.** Any document that quotes the `control:` line with `not sent`: none found in `README.md`, `docs/safety.md` or `docs/architecture.md` by search; if R4 finds one, the new text is `dropped (too many unanswered)`.

## A5. A slow but healthy runner replacement is not killed

**Fix.** The controller widens its allowance. While a `Switch` or `Rebind` written to a worker is unanswered, and for at most `RunOptions::replacement_allowance_ns` (60 s) after it was written, both of the controller's judgements of that worker (a beat older than `stale_after_ns`, and no progress for `stale_after_ns` plus the duty cycle's idle allowance) allow that much more. Past the window the usual allowance alone judges the worker again, so a replacement that never ends is still stopped, at about the window after the write. Tests: `ReplacementAllowance.*` (for a Switch and for a Rebind): not signalled until 30 ms and the 200 ms allowance, signalled after it; and an answered replacement allows nothing more.

**Decision.** Options: (a) the worker beats while it replaces; (b) the controller widens its allowance. Chosen (b), because (a) cannot be completed. The replacement is `release()` followed by `prepare()`, which allocates, first-touches and locks the new working set in one call that cannot beat; a beat before and after shortens the gap by the part before `prepare` and not by `prepare`, which is the part that is long for a large footprint. And a beat does not satisfy the controller's second judgement, no progress for the allowance, which the replacement also trips (the progress stays where the last slice left it). (b) covers both judgements and needs no change in the worker. Cost: a worker that is really hung during a replacement is stopped up to 60 s later than otherwise. 60 s is far above any replacement measured here (a Switch to a large footprint is seconds) and bounded.

**Sentences R4 must change.** `docs/safety.md` (the staleness paragraph, if it lists the allowances): add that a worker told to switch or rebind is given 60 s more, from the write until it answers.

## C1. A configured duration is bounded

**Fix.** `config::kLongestRunMilliseconds` (a quarter of what an `int64` of nanoseconds holds, 73 years, the bound `--duration` already had) is checked in the parse of `run.duration`: `duration is longer than any run can be (about 73 years)`. The command line's constant is now this one. Test: `ConfigParseTest.ADurationPastWhatARunCanBeIsRefusedAndTheLongestOneIsKept` (three values, and the longest accepted).

**Sentences R4 must change.** The configuration section of `README.md` / `docs/` that describes `run.duration` should say it is at most about 73 years.

## C2. SIGCHLD back to its default before the first fork

**Fix.** `Syscalls::default_signal(int)` (sigaction to `SIG_DFL`) is new; `Run::prepare` calls it for `SIGCHLD` right after it ignores `SIGPIPE`, before anything is created, and a refusal is stage `signals`. Tests: the controller's happy-path test asserts the call happened once before `spawned`; `EveryFailureBeforeTheLoopNamesItsStageAndLeavesNothingBehind` has its failure; `RealSyscallsTest` ignores `SIGCHLD`, defaults it and reads the disposition back, and is refused for `SIGKILL`; `SupervisionTest.AnIgnoredSigchldDoesNotMakeTheKernelReapTheWorkers` runs real workers with `SIGCHLD` ignored in the test process; `M5Binary.ARunStartedWithSigchldIgnoredStillReapsAndReports` starts the shipped binary that way and reads the report and exit code 0.

**Decision.** A new method on the seam rather than a sigaction call in the controller: `platform::Syscalls` is where every process-wide state the controller changes already goes (`ignore_signal`), and the fakes then record it. The four implementers in tests got the method.

## C3. Result files are durable before they are named

**Fix.** `FileResultSink::store` writes `<path>.partial` through the journal's file operations: any leftover partial is removed first, the bytes are appended (short writes and EINTR handled as the journal handles them), the file is fsynced, closed with the close read, renamed over `<path>`, and the directory is fsynced. A failure at any step before the rename removes the `.partial` and leaves the old result; a failure of the final directory sync says `cannot make <path> durable` (the file is in place; only its survival of a power cut is in doubt). When the second of two formats fails, the refusal ends `; written before it: out/run.json`. Tests: one per documented errno of the write, of the fsync of the file and of the directory (twice: the one inside the open of the partial and the one after the rename), and of the close of each, through a `FileOps` that fails one call on real files; `AnInterruptedCloseIsNotAFailureOfTheResult`; the write_results tests for the named file.

**Decisions.**
- *The seam.* The task asked for injection "through `platform::Syscalls`". The repository keeps file I/O out of that interface on purpose (`file_ops.hpp`: "Ordinary file I/O is not host introspection, so it stays out of platform::Syscalls (PLAN §4.2)"), and the journal already has the seam with fsync and close in it. The sink uses that one (`journal::FileOps`, errors `platform::SyscallError`), so there is one place for file calls and no second fake of them. `FileOps::lock` (flock) is added to the same seam. If the owner wants these in `Syscalls`, it is a mechanical move.
- *Reuse of `FileStorage`.* The sink opens the partial with `FileStorage` (append, short-write loop, EINTR retries, fsync of the directory on open) instead of writing a second loop. It gained `close()` (reports the close, releases the descriptor whatever it says) and `sync_directory` is public.
- *`close` and EINTR.* On Linux the descriptor is released when `close(2)` reports EINTR, and a second close could close a descriptor another thread was just given. EINTR from the explicit close is therefore success and is not repeated. The other errnos (EBADF, EIO, ENOSPC, EDQUOT) are reported.
- *A leftover `.partial` is removed before writing.* The storage appends. The old test that made the partial a symlink to `/dev/full` therefore no longer fails the way it did, and was replaced by the injected ENOSPC on the write.
- *The final directory sync fails after the rename.* The file is in place under its name; the exit code is still the write-failed one (4), because the guarantee the sink makes (durable) was not kept. The message says which.

**Sentences R4 must change.** `docs/safety.md` or `README.md` where result files are described as "written to a `.partial` and renamed": add "fsynced, and the directory fsynced".

## C4. A run that ended in a controller error leaves a journal that reads abnormal

**Fix.** `stress` writes the terminator only for a run that returned a report. After a controller error the journal is left as it is: it replays as ended abnormally, and the next start keeps it as `FILE.1`. Test: `CliJournal.ARunTheControllerFailedLeavesItsJournalWithoutATerminator` (replaces the test that pinned the old behaviour, `...ClosesItsJournalEvenWhenItCouldNotStartTheWorkers`).

**Decision.** Options: (a) no terminator on the error path; (b) a distinct ending. (b) needs a new `Ending` in `journal.hpp`, a contract header (ADR 0004): not made. (a) uses the endings that exist. A journal with no entry yet (an error before the first tick, such as a refused fork) replays as empty and is reused by the next start, which is right: it holds no evidence.
The ENOSPC sub-claim of the review was rejected by the verdict (the terminator write fails too), and nothing was changed for it.

**Sentences R4 must change.** `docs/safety.md` / `docs/architecture.md` where the terminator is described ("a run that ended writes a terminator"): it is written when the run returned a report, not when the controller failed. `docs/JOURNAL.md` §2.53 says the same and is not mine to edit in this change; the sentence is "the terminator means only that the run function returned".

## C5. The beat loop is not starved by a backlog of steps

**Fix.** `run_schedule` forces at most one journal write per tick, after the due steps, naming the phase the run is in then. Test: `ControllerJournal.ABacklogOfDueStepsForcesOneWriteAndNotOnePerStep` (five steps due in one tick: one entry, for the last).

**Decision.** Options: (a) one forced write per tick; (b) refresh the beat between the writes; (c) refuse a rotation shorter than the tick. (a) chosen: it bounds the number of fsyncs per tick at one whatever the backlog, needs no new state, and loses nothing the journal exists for (the phase at the time of a crash is the last one; the intermediate phases of a catch-up are still marks on the timeline and lines on the console). (b) leaves a tick as long as the sum of the fsyncs. (c) is a limit on a legal setting. The journal header said "at every phase of the schedule"; for steps run through in one tick that is now "at the last of them".
Not addressed: a filesystem whose single fsync takes longer than the staleness allowance; that is the hardware's, and one fsync per tick is the least the journal asks.

## C6. `--journal PATH` refuses a file that is not a journal; two runs on one path are excluded

**Fix.** `open_run_journal` takes an exclusive advisory lock (`flock`, non-blocking) on the journal file before it reads it; a held lock refuses the run: `the journal PATH is in use by another run; give this run a journal of its own, or wait for that one`. A non-empty file whose replay is corrupt with no entry (its first record does not verify) is refused and left where it is: `PATH exists and is not a journal (...)`. Tests: `RunJournal.*` over the in-memory files (not a journal; damaged after its first entry is still kept; torn in its first record is still kept; a second run refused and nothing moved; the lock of the old file held until the new one is locked; every lock errno), `RealFileOpsTest.LockIsExclusive...`, `FileStorageLock`, and the real binary: `M5Binary.AFileThatIsNotAJournalIsRefusedAndNotMovedAway`, `M5Binary.ARunOnAJournalAnotherRunHoldsIsRefusedAndTouchesNothing`.

**Decisions.**
- *"Not a journal" without a magic.* The format has no magic or header, and adding one is a format change (every journal ever written, the replay, the contract's tests). The smallest honest test is the first record: a file that is not a journal almost always fails its length and checksum there. Refused: corrupt with zero entries. Kept as evidence, as before: torn (including torn inside the first record), unterminated, or corrupt after at least one entry. The cost: a journal whose first record was damaged by the storage is refused too, not renamed; the message says to move it. A text file shorter than a header reads as a torn first record and is still kept (renamed), which cannot be told from a journal torn there.
- *Where the lock is.* On the journal file, as asked. The file is replaced when the previous journal is kept or removed, so the old descriptor (and its lock) is held until the new file is open and locked; otherwise a second run that opened the old name could take the old file's lock and then move the new journal away. A residual window remains for a run that opened the path after the rename and before the first run's `open` (two starts in the same microsecond): both then contend for the new file's lock and one is refused, which is the safe outcome.
- *Filesystems that cannot lock.* ENOLCK and EINVAL from `flock` (NFS without a lock daemon) refuse the run with the kernel's reason. The journal is the evidence a run exists to leave; a run that cannot be sure it is the only writer is refused as one that cannot write is.
- *`loadforge journal FILE` does not lock.* It reads.

**Sentences R4 must change.** `docs/safety.md` and `README.md` where `--journal` is described: add that a file that is not a journal is refused (not renamed), and that a journal held by another run is refused.

## C7c. Labels printed by `loadforge journal` are escaped

**Fix.** `render_entry` prints the phase and workload with every byte outside printable ASCII, the backslash and the quote as `\xNN`. Test: `JournalWords.ALabelIsPrintedWithItsControlCharactersEscaped`. (C7 a and b are R1's.)

## Gates

A container restart (2026-10-23, before 03:38 UTC) ended the session that made these changes
before it recorded its gate runs; every finding's commit had been pushed, and the last
recorded local result is a debug build that exited 0. The gates for this tree are CI's, on
both architectures, in the pull request that lands it.
