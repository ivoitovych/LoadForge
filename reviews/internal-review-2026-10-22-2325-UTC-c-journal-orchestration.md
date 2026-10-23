# Review C: crash journal and controller run loop (read-only)

Reviewed at HEAD of main, by reading only (nothing built or run; no copy unpacked, nothing to delete).
Files read in full: src/controller/journal/*, orchestration.{hpp,cpp}, supervision.*, safety.*,
src/main/outcome.*, result_files.*, journal_rig.cpp, the stress wiring in cli.cpp (lines 60-100, 750-990),
and the supporting platform/worker_pool.cpp, process.cpp, syscalls.cpp (pipes, wait, signals), core/crc32.cpp,
core/duration.cpp. Docs: JOURNAL 1.52, 2.48, 2.53, safety.md section 3, README exit codes.

## Findings

### 1. Run duration overflows int64 in the controller; no bound on the --config path
- src/controller/orchestration.cpp:429 `duration_ns_ = config_->run.duration.count() * kNanosecondsPerMillisecond;`
- Defect: signed multiplication with no bound; core::parse_duration (core/duration.cpp:46-50) accepts any value up to INT64_MAX milliseconds, config.cpp:297-305 only rejects <= 0, and the 73-year cap (kLongestMilliseconds) exists only for `--test` (cli.cpp:321) and for the sampler sizing (cli.cpp:928, where the author clamps with std::min, so the hazard was known there).
- Scenario: `run.duration = "10000000000s"` in a config file (1e13 ms) gives 1e19 ns, which wraps to about -8.4e18. The first tick sees elapsed >= duration_ns_, requests Stop, prints "deadline reached", and the run ends within a tick or two reporting a duration the config never asked for. Other values wrap to a large positive number instead (a run that effectively never ends). It is also undefined behaviour, and the same wrapped value is passed to every worker as Pacing.duration_ns. The same product is not re-checked anywhere else on this path (explore caps at 30 days, `--test` at 73 years, `--config` at nothing).
- Fix: refuse in config validation (or in prepare()) a duration above the same cap.
- Confidence: high that it overflows; low severity (needs an absurd value).

### 2. SIGCHLD inherited as ignored makes every run abort at the first full reap
- src/controller/orchestration.cpp:386 prepare() ignores SIGPIPE but never sets SIGCHLD to default; src/platform/syscalls.cpp:502-509 and 139-147 turn waitpid ECHILD into an error.
- Defect: a SIG_IGN disposition survives exec. If the tool is started from a launcher that ignores SIGCHLD, the kernel auto-reaps the workers; waitpid(-1, WNOHANG) then returns 0 until the last worker is gone and ECHILD after, and waitid in has_exited returns ECHILD for a worker that already died.
- Scenario: the controller's `reap` (or the stop escalation's `signal` step) returns ControllerError once the last worker exits: exit 1, "reap: waitpid ...", no report, no fates, no result files, although the workers ran to completion. The journal gets a terminator (see 4).
- Fix: `ignore_signal`-style call setting SIGCHLD to SIG_DFL at the top of prepare().
- Confidence: medium. The kernel semantics are certain; I did not run the binary under an ignoring parent, and whether a given launcher does this is environmental.

### 3. Result file is published without fsync and with the close error unread
- src/main/result_files.cpp:44-56 (store).
- Defect: bytes are written to run.X.partial with an ofstream, `flush()` and `good()` are checked, but the stream is closed by its destructor (error discarded) and the file is never fsynced before the rename; the rename is then reported as "result written".
- Scenario: (a) on a filesystem that reports a write error only at close (NFS, quota, some FUSE), the truncated file is renamed over run.json and the run exits 0 with "result written: out/run.json". (b) This tool exists to run on machines that may lose power or reset under load: after a power loss following the exit, rename-without-fsync can leave an empty or short run.json under the final name (ext4 only partly mitigates this for rename over an existing file, and not for a new one). The journal gets the opposite treatment (every entry fsynced), so the one artifact the user keeps is the weakest.
- Also: when the second of two formats fails, the first stays published but is not listed (write_results returns only the error, result_files.cpp:114-116), and the leftover `.partial` is not removed on failure.
- Fix: write with a descriptor, fsync, check close, rename, fsync the directory.
- Confidence: high for the missing fsync and unread close; medium for the user-visible loss.

### 4. The terminator is written even when the run was aborted or journaling had already failed
- src/main/cli.cpp:757-768 (end_journal), called at cli.cpp:974 for a ControllerError and at 983 after a normal return; orchestration.cpp:341-349 (journal_fault just stops journaling); journal_rig.cpp:122-139 (a Terminated journal is removed at the next start).
- Defect: the terminator means only "the run function returned", so a run that died of a controller error (clock, pipe, reap, signal, heartbeat release) with its workers killed by teardown, or one whose journal stopped at its first append/sync error, is closed with the same record as a clean run. The next `--journal FILE` start prints "previous journal ...: terminated" and deletes it (journal_rig.cpp:125-129).
- Scenario: a 10-hour run hits a controller error at hour 3 (or a journal ENOSPC at second 5, after which the journal has no marker that it stopped and the run continues). The journal is terminated and later removed; the evidence of where it stopped is only a stderr line that is gone. Worse for ENOSPC: the machine then power-cycles at hour 9, the file holds entries up to second 5 and no terminator, and the replay shows "t=5.0s" as the last sign of life.
- 2.53 documents "the terminator claims only that the run returned", so this is the stated design; the defect is that the design deletes the only record of an aborted run. A terminator written only on a run that returned a report, plus a final "aborted" entry (or no terminator) on the error path, would keep the evidence.
- Confidence: high that it behaves so; low-medium that it counts as a defect.

### 5. Blocking fsync and unbounded schedule catch-up inside the one loop that publishes the controller heartbeat
- src/controller/orchestration.cpp:757-768 (`write_journal` after every due step, forced), 631-640 (`while next_step_ ... at_ns <= elapsed_ns`), beat only at tick start (line 590).
- Defect: the controller's beat (slot 0) is the one every worker watches (stale_after_ns: 2 s for null workloads, 10 s for verifying ones). Journal append+fsync, sampler rounds and a backlog of schedule steps all run in the tick before the next beat. Each due step costs a forced append+fsync.
- Scenario: `--journal` on a slow or stalling filesystem (network mount, USB stick, a journal on a disk in error recovery) with fsync taking longer than the allowance, or `stress --test mixed --rotation 10ms --journal J` (rotation only has to be >0, mixed.cpp:147 caps steps at 100000) where several steps are due per tick: the beat is not refreshed, every worker concludes "controller lost" and exits 124, and the run ends with all workers not completed (exit 3) although the controller is alive.
- Confidence: low-medium; the mechanism is certain from the code, the thresholds are not measured.

### 6. `--journal PATH` on a file that is not a journal moves that file; two runs on one path are not excluded
- src/main/journal_rig.cpp:122-134 (open_run_journal).
- Defect: any non-empty file whose replay is not Terminated is treated as a crashed journal and renamed to PATH.N; there is no lock. Both are safe against data loss (the file is kept, only a valid terminated journal is deleted) but surprising.
- Scenario: `--journal config.toml` renames the user's config to config.toml.1 and prints a previous-journal line of "corrupt". Two stress runs started on the same path: the second finds the first's live journal, renames it to PATH.1 while the first still appends to the renamed inode (and later writes its terminator into it), then starts its own at PATH; the "kept" crash evidence is now a terminated-looking file.
- Confidence: high that it happens; low severity.

### 7. Replay is looser than its format comment; torn versus corrupt can be misreported
- src/controller/journal/format.cpp:155-165 and 112-142.
- (a) `scan` classifies "length in range but more than the bytes left" as TornTail before any checksum is looked at. A flipped length bit in the last or second-to-last record (the header comment says it must be "a failed check") that makes the length larger than the remainder reads as a benign torn tail instead of Corrupt. Both endings are kept and printed the same way by the command line, so the effect is the wording ("between records" versus corrupt), not lost data.
- (b) `decode_entry` does not enforce what format.hpp states: the bytes of an absent optional are zero, labels are text, `elapsed_ns` is non-negative and non-decreasing across entries. A record with a valid checksum but a negative or backwards time, or nonzero bytes behind a clear presence bit, replays as good.
- (c) Labels are printed raw by render_entry (journal_rig.cpp:60-65): a crafted journal (checksum is not a secret) can put escape sequences or newlines into the terminal output of `loadforge journal FILE`.
- Confidence: high on all three; low severity (a CRC protects against accident, not against a crafted file).

## Checked and found correct

- Journal framing: length and CRC cover each other (crc32_update chains correctly, core/crc32.cpp:36-47); zero or >512 length is corrupt; the biggest possible entry is 196 bytes, so kMaxPayloadBytes cannot reject a valid entry; a terminator must be the sole payload and the last bytes of the file, so trailing bytes are Corrupt; an unknown record type is Corrupt; a prefix cut at any byte reads as TornTail or Unterminated, never as a shorter valid journal with a wrong ending.
- Durability order: the file is created with O_APPEND|O_CLOEXEC, the directory is fsynced at open (file_storage.cpp:51-64, so the name is durable before the first entry), each entry is appended and then fsynced before the count is incremented (orchestration.cpp:757-768), fsync is retried only on EINTR, a short write continues and a zero-length write is an error. The rename of an old journal to FILE.N and the creation of the new one are in one directory, so the directory fsync at the next open covers both.
- After a journal fault the controller never appends again (journal_live_ false), so no valid record can follow a torn one from the controller itself; the terminator written after a partial record reads as Corrupt, the safe direction.
- Descriptors: journal files, pipes (pipe2 O_CLOEXEC), the heartbeat region (MFD_CLOEXEC), perf events (PERF_FLAG_FD_CLOEXEC) and read-only sysfs opens are all close-on-exec; the child clears the flag only on its three descriptors (syscalls.cpp:436-441). Pipes are created before any fork, but since all are close-on-exec a worker does not inherit its siblings' record ends, so a dead worker's pipe does reach end of file. The controller closes its copies of the worker ends after the fork (orchestration.cpp:539-549).
- Worker lifetime: PDEATHSIG is SIGKILL, armed before exec (it survives exec), with a getppid check against the pre-fork supervisor pid to close the arming race; each worker is a session leader and its group is killed on reap (kill_group) and in teardown; teardown kills groups and workers and waits; WorkerPool unwinds a partial spawn itself, so gating teardown on `spawned_` leaves no worker behind; the child of a failed exec never kills siblings and does not close the shared journal. SIGPIPE is ignored before any pipe write.
- Stop and kill: request_stop covers every unreaped worker, a Stop is sent regardless of the unanswered count and an EPIPE only means the worker is already gone; the escalation SIGTERM, then SIGKILL, comes from escalation_due on a monotonic clock and is evaluated for every unreaped worker each tick; a stale worker is signalled at once in either the none or requested step; a worker already found exited is not signalled (no false attribution). The main loop has no wall-clock bound, but the SIGKILL stage cannot be skipped (a worker in uninterruptible sleep would hold the run, which nothing here can change).
- Exit codes: exit_code_for gives 5 (findings or corrected errors), then 3 (not all clean), then 6 (verified and incomplete), then 0; with_output_failure keeps 5 and turns anything else into 4. Together that is the README order 5, 4, 3, 6. A null-only run is 0 or 3 as documented. The assemble-failure path (cli.cpp:795-796) also goes through with_output_failure. end_journal is called in the parent only and never for the exec-failed child.
- Grace and stale allowance wiring: allow_for_verified_units takes the max of the defaults and 10 s, so it cannot shorten a larger value; the scheduled 5 s stop grace is set before it and is raised to 10 s for verifying workloads; kill_ns stays 1 s after SIGTERM, which is consistent with a worker that has no SIGTERM handler.
- Other arithmetic: escalation and staleness are differences of one monotonic clock; drain_after_death rounds its poll timeout up and bounds both time and count; the sampler sizing in cli.cpp clamps the duration before summing.

## Not checked
Heartbeat region, worker-side loop and ipc framing (outside the named scope); isolation re-run code beyond confirming it uses the same launcher (so PDEATHSIG applies); no tests, sanitizers or runs.

The project still needs an outside reviewer.
