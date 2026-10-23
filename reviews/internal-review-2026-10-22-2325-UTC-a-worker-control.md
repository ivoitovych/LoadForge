# Review A: worker control and the mixed workload

Scope read: src/worker/control.hpp, control/control.cpp, control_channel.*, duty.*, scheduler.*, verified_run.*, entry.cpp, frames.cpp, ipc/record.*; src/workloads/mixed/*; the control parts of src/controller/orchestration.cpp (command, run_schedule, watch, take_ack, request_stop, reap, finish); background journal §1.51-§1.56, §2.49-§2.53 and docs/journal/29-integration-m5.md. Method: reading only. No build was made and no file was created in the repository or in any worktree. Nothing below was run; each finding says what it rests on.

## Findings

### 1. A bad frame in the same read discards the good records before it (control pipe and record pipe)
- File: src/worker/ipc/record.cpp:253-262 (RecordSource::drain), consumed at src/worker/control_channel.cpp:56-66 and src/controller/orchestration.cpp:906-921.
- Defect: drain() builds `drained.records`, and when `reader_.next()` returns a fault later in the same read it returns the error and throws the already-parsed records away.
- Scenario, control side: one read (up to 4096 bytes) holds a whole Stop frame followed by bytes that fail the magic or checksum. look() returns `lost = kControlChannelBroken` with an empty `messages`. The Stop is never handled or acknowledged. The header of control_channel.hpp says "the stop is handled first".
- Scenario, record side: a read holds N valid error records (a wrong-result finding) and then one frame whose checksum fails. The controller sets `channel_corrupt`, closes the channel, and the N findings are never ingested. The run is still not clean, because `channel_corrupt` is a failing fate, but the evidence of the hardware fault is lost. This is the case the corruption check exists for.
- The test `MessagesBeforeAnythingBrokenAreStillHandled` (tests/unit/worker/control_channel_test.cpp) only covers a frame that verifies and has an undecodable payload. It does not cover a framing fault, so it does not pin the claim made in the header.
- Confidence: high that the code behaves so (read directly). Medium that it matters, because the controller never writes a corrupt control frame. It is the record-side loss that matters.
- Fix direction: return the records read so far together with the fault, or defer the fault to the next drain.

### 2. A worker that leaves because the controller is gone reports a zero tally
- File: src/worker/verified_run.cpp:585-587 with 302-312, consumed at src/worker/entry.cpp:667-668.
- Defect: `result_.tally` is set only in `conclude()` (line 307). The `Step::kLeave` path returns `ticker.result()` with a default `Tally`, while `outcome.progress` was set at the start of the tick.
- Scenario: a verifying worker has computed 5000 units and then loses its controller (stale beat, EPIPE on an acknowledgement, hang-up). The error record and stderr line read "controller lost: ... after 5000 unit(s), ...; 0 unit(s) computed; oracle checked 0 unit(s) over 0 sampled block(s), 0 wrong, 0 not performed; bracket checked 0 unit(s), 0 failed". The two halves of the sentence contradict each other.
- The controller keeps its own tally from the frames, so no evidence is lost. The error path reports the wrong thing.
- Confidence: high.

### 3. The memory budget is checked for the start footprints only, not for what a schedule later switches to
- File: src/controller/safety.cpp:102-123 (check_memory sums `config.workloads[].memory`); src/main/schedules.cpp:68-72 (config workloads are built from `Start.footprint_bytes`); src/workloads/mixed/mixed.cpp:105-118 (assignment draws the whole or half of the footprint for every Switch).
- Defect: the start footprint of a worker is the whole or half of the ceiling, by a coin. The budget check passes on that sum. Every later Switch may take the whole footprint, and nothing checks the sum of those against the budget or `limits.memory`.
- Scenario: 8 workers, ceiling F = 1 GiB, a budget that allows 6 GiB. The start draw gives, say, 4 whole and 4 half (6 GiB) or all half (4 GiB), and the check passes. The "all-join" rotation switches every worker to a footprint drawn from {F, F/2}; with all whole, the run holds 8 GiB, past the ceiling the controller promised to enforce.
- Confidence: medium-high. I read the check and the only place it is called; I did not run a budgeted mixed run.
- Fix direction: check `workers x footprint_bytes` against the budget when the schedule is planned.

### 4. A command written to a worker whose pipe is already closed disappears from the accounting
- File: src/controller/orchestration.cpp:659-666 (command()).
- Defect: on EPIPE, command() returns Ok without counting anything, so `sent` is not incremented and `dropped` is not incremented. Yet `describe(CommandReport)` labels `dropped` as "not sent".
- Scenario: a worker exits on its own duration between ticks, and the schedule's next step (or a rotation) writes to it before the reap. The commands are never counted as sent, refused or dropped. The report says "N sent, N done, 0 unanswered, 0 not sent" for a worker that was told more than that.
- Confidence: high (read). Severity low: reporting only.

### 5. The worker beats nothing while it replaces its runner, and the controller has no allowance for it
- File: src/worker/verified_run.cpp:468-506 (reconfigure: close_runner, release, prepare, possibly a second prepare to restore); src/controller/orchestration.cpp:771-822 (watch).
- Defect: there is no heartbeat between the start of a Switch or Rebind and the next tick. A refused replacement does two prepares. The controller treats a stale beat as a hang and sends SIGTERM, and a pending Switch or Rebind gives no extra allowance (compare the idle allowance for SetDuty).
- Scenario: Switch to a workload whose creation (allocation, filling, oracle construction, thread start, memory lock) takes longer than `stale_after_ns`, or a Switch that is refused so that the old runner is rebuilt as well. The healthy worker is killed as hung, and the "refused command costs a pause and not the run" promise (§2.50) fails.
- The tests hide it: tests/supervision/control_test.cpp sets `stale_after_ns = 10 s` and uses 2 MiB footprints. The verifying default in src/main/cli.cpp is also 10 s. So it needs a multi-gigabyte footprint or a loaded machine, and I have not reproduced it.
- A related effect: after a gap longer than the allowance, ControllerWatch::gone (scheduler.cpp:105-111) counts the time from the last good sample, so one torn read of the controller slot right after a long replacement makes the worker leave as "controller unreadable".
- Confidence: medium on the mechanism (read), low on how often it happens.

### 6. Unbounded or quadratic growth in the controller's per-worker state
- File: src/controller/orchestration.cpp:670-673 (`worker.told.push_back` for every Switch, duplicates included) and 977-981 (`std::ranges::find` over it for every error record); `worker.notes.push_back` on every refusal (line 1043) and unknown record (line 958).
- Defect: `told` grows by one string per Switch and is searched linearly per record; `notes` has no cap, while `findings` has `kMaxFindingsKept`.
- Scenario: a long run with a short rotation (up to kMaxSteps = 100000 Switches per worker) and a worker with a persistent fault: each error record costs up to 100000 string compares inside the controller's tick, which also publishes the controller's own beat. Separately, a schedule whose commands are all refused (every command to a null-workload worker, or every Rebind outside the cpuset) adds one note per command, up to kMaxCommands = 4,000,000 in the worst case.
- Confidence: medium (read). Severity low. A set of distinct names for `told` would fix the first.

### 7. Rebind and Switch can silently revive a runner that stopped itself
- File: src/worker/verified_run.cpp:508-516 (slice) against 468-506 (reconfigure).
- Defect: a runner that has halted (a vote that decides nothing, see slice_runner.cpp:128-131) is discovered only by the next `run_slice()`. If a Rebind or Switch is handled between the halting slice and the next one, `close_runner` calls `finish()` (which works on a halted runner) and a fresh runner is built, so the run goes on as if nothing happened. The halt record was sent, but `kRunnerStopped` never fires.
- Scenario: slice k halts the runner (control state cannot be trusted). The controller's rotation arrives in the same tick or the one after; the worker replaces the runner and continues, ending "completed" with one VerificationNotPerformed in the records.
- Confidence: low-medium. It depends on how a halted run is meant to be treated; the rotation's fresh control state is arguably a legitimate reset. Worth a decision and a test either way.

### 8. The schedule check does not look at the start list or at label uniqueness
- File: src/workloads/mixed/schedule.cpp:84-122 (problem()) against schedule.hpp:32-37 and :57-60.
- Defect: `problem()` checks count, label length, ordering, slots and command rules, but not that each `Start` has a valid workload name and a non-zero footprint, nor that phases differ at every step as the header requires.
- Scenario: a hand-made schedule with `Start{"", 0}`. problem() returns nothing; the launch then fails in the worker with bad arguments (exit 125) for every worker at once.
- Confidence: high (read). Severity low: the two builders in this directory produce valid starts and unique labels.

### 9. Smaller points
- src/worker/verified_run.cpp:97-109: the comment "the product stays far inside 64 bits" is wrong for an interval above about 4.9 hours (`last_units x target` passes 2^64 at last_units = 2^20). The controller's default interval is far below that; the worker's own command line accepts up to 9.2e12 ms. The same width of interval makes `static_cast<int>(interval_ns / 1e6)` (verified_run.cpp:229, scheduler.cpp:132) wrap past 2^31 ms and gives `std::clamp` (verified_run.cpp:330) a low bound above its high bound. Unreachable from the controller; only by running the worker switch by hand.
- src/workloads/mixed/roster.cpp:35-45: `footprint *= 2` against a `ceiling_bytes` above 2^63 wraps to 0 and the loop never ends if no footprint is creatable. Not reachable from a sane command line.
- roster_of finds the least power-of-two multiple of 64 KiB at which a workload is created; mixed.cpp then asks for the whole or half of an option footprint that need not be such a multiple. A workload that is creatable at 128 KiB and not at 192 KiB would be refused at run time (a counted refusal, the worker goes on). Speculative; I did not look for such a workload.
- Messages queued behind a Stop in one batch are not acknowledged (worker side, by design in §2.50). The controller excludes Stop from "unanswered", so this does not show up in the counts.

## Checked and found correct
- Wire codec (control.cpp): every field read is bounds-checked; a Rebind count is checked against the bytes left before anything is allocated; a Switch text length is checked against the bytes left; decode refuses trailing bytes, unknown kinds and versions, and invalid commands. `is_valid` is the one rule used by the worker's decoder, by schedule `problem()` and by `refusal_of` in mixed.cpp, so a schedule that passes `problem()` is never undecodable at the worker.
- Pipe handling: RecordWriter loops over short writes, bounds consecutive EINTR, and treats a zero-byte write as an error; EPIPE is separated from other failures in `deliver()` and in `command()`; SIGPIPE is ignored on both sides; the pipes are created close-on-exec and each end is closed by the process that does not own it, so a dead controller is seen as end of file by the worker (the backup is the stale beat).
- Stop and hang-up in one read: both loops (null workload `answer_all` and `Ticker::once`) handle the messages of a look before the `lost` reason, and a Stop is acknowledged before the finished record. Truncation at end of file is a hang-up; any other framing fault is a broken channel.
- Sequence and acknowledgement accounting: the sequence is consumed only after a whole frame is written, acknowledgements must arrive in order, the 64-message cap counts what is unanswered, and a Stop is exempt from the cap and from "unacknowledged". The 32-bit sequence cannot wrap within kMaxCommands. Effects (paused, idle allowance) are taken only from a Done.
- Replacement: the old runner's records and tally go out before it is released; `carried_` is not double counted on any path that keeps running; the tally sent is cumulative and monotone, which matches the controller's "went backwards" check; members are destroyed in reverse order of construction; a refused replacement restores the old one and acknowledges kRefused with the reason; `select_path(...).value()` cannot fail because the scalar path is always taken when nothing else qualifies.
- Duty arithmetic: `busy_ns` cannot overflow for any positive period; `DutyClock::at` handles a clock that went back; the idle wait is rounded up, at least 1 ms and at most one interval, so a worker in the idle part or paused still beats within an interval. The controller's allowance `stale_after + idle_ns` is set from the Done acknowledgement, which is sent before the worker's first idle part can begin, so the allowance is in place before the silence starts.
- Mixed schedule: the code follows the header's rules for phase order, every third rotation as a move, the set `(s + r/3) mod n`, the pools for balanced, the join taking the same entry, and the footprint rule. Step times are below the duration, the counts are bounded before anything is built, and the commands product is checked without overflow.
- Cycling: only commands that change a worker's state are produced, in the order Resume then SetDuty; the step count and command count are bounded; the level times cannot overflow.
- Memory and tally frames decode with exact length checks; the memory detail rule (only with kFellBack) matches the producer.

## Tests that assert more than the code promises
- None found that are wrong. The two notes are the gap in finding 1 (frame-level fault after good frames is untested) and that tests/supervision/control_test.cpp runs with `stale_after_ns = 10 s`, which is why finding 5 cannot show there. Timing assertions such as `told.tally.units_computed < left.tally.units_computed` rest on the pause lasting most of the run, which leaves a wide margin.

The project still needs an outside reviewer.
