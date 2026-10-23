# Verdicts on reviews A (worker control, findings 1-9) and C (journal and run loop, findings 1-7)

Written 2026-10-23 00:12 UTC, against origin/main unpacked in a private copy (debug preset). Nothing in any repository was changed.
Reproducing sources: verify-ac-tests/
(worker_ac.cpp, channel_ac.cpp, controller_ac.cpp, misc_ac.cpp; each includes the repository's own test file for its rig; CMake
snippet in CMakeLists.snippet.txt; gtest output in run-final.log; real-process experiments and their outputs in exp/).
All 12 reproducing tests FAIL on the current code, the failure message showing the claimed behaviour.
Run: `build/debug/tests/verify_ac_tests --gtest_filter='Verify*'`.

## Summary

| id | verdict | severity | note |
|---|---|---|---|
| A1 | CONFIRMED | medium | good records before a framing fault in the same read are dropped; the control-side header claim is a stretch |
| A2 | CONFIRMED | medium | zero tally in the controller-lost error line; also seen in a real run |
| A3 | CONFIRMED | medium | budget sees start footprints only; later steps hold up to 1.25-1.6x more in the seeds tried |
| A4 | PARTLY | low | uncounted, but pinned by an existing test and the field's documented meaning is narrower |
| A5 | CONFIRMED (mechanism) | low | no beat during a replacement, no allowance; not reproduced against a real controller |
| A6 | PARTLY | low | growth and linear search are real; impact overstated |
| A7 | PARTLY | low | behaviour confirmed; evidence is not lost and it is arguably a reset |
| A8 | CONFIRMED | low | problem() ignores the start contents and label repeats |
| A9 | PARTLY | low | overflow real, threshold misstated; the other two points hold by derivation |
| C1 | CONFIRMED | low | wrapped duration; real run reproduces it |
| C2 | CONFIRMED | medium | real run: exit 1, no report |
| C3 | CONFIRMED | medium | no fsync (strace), leftover .partial, discarded first path; close error by code only |
| C4 | PARTLY | medium | confirmed for the controller-error path; ENOSPC sub-claim REJECTED for a disk that stays full |
| C5 | CONFIRMED (mechanism) | low | real run reproduces it at --rotation 1ms only; 10ms and 100ms are fine here |
| C6 | CONFIRMED | low | both scenarios reproduced |
| C7 | CONFIRMED (a, b, c) | low | three tests |

---

## A1  A bad frame in the same read discards the good records before it

Verdict: CONFIRMED. Severity: medium (record side: the findings in the same read are lost, but the run is still failing through channel_corrupt, so the verdict stands and the detail is what is lost; control side: low, the controller never writes a bad control frame).

Evidence: src/worker/ipc/record.cpp:253-262 (drain returns `channel_error(next.error())` from inside the loop that fills `drained.records`). Tests in channel_ac.cpp:
- VerifyA1.ControlSide_StopFollowedByBadMagicInOneRead_StopIsStillHandled: a whole Stop frame then 32 bytes 0xAA in one read. Output: `looked.value().messages.size() Which is: 0, expected 1: the Stop that arrived whole before the bad bytes was discarded`; lost = kControlChannelBroken.
- VerifyA1.RecordSide_GoodRecordsBeforeABadFrameInOneRead_AreNotLost: three valid records then one frame with a bad checksum in one read. Output: `drain() returned the fault (the frame's checksum does not match its bytes ...) and the 3 whole, valid records read in the same chunk are gone`.
Caveat on the review's wording: control_channel.hpp:16 says "the stop is handled first" in the context of a pipe that has said stop and gone (hang-up), not of a framing fault, so the quoted header is not violated as written; the existing test MessagesBeforeAnythingBrokenAreStillHandled covers an undecodable payload only, as the review says. The consumer at orchestration.cpp:906-921 does treat the fault as channel_corrupt and closes the channel, so the earlier records of that read are never ingested.

Smallest fix: in RecordSource::drain return the records read so far together with the fault (or keep the fault in the reader and return it on the next drain after handing back the records).

## A2  A worker that leaves because the controller is gone reports a zero tally

Verdict: CONFIRMED. Severity: medium (wrong report text; the controller keeps its own tally, so no evidence is lost).

Evidence: result_.tally is assigned only in conclude() (verified_run.cpp:307); the Step::kLeave path returns ticker.result() (verified_run.cpp:585-587) with a default Tally, while progress was set at tick start (line 261). Test VerifyA2 (worker_ac.cpp): stale controller after some slices. Output: `outcome.value().tally.units_computed Which is: 0 / outcome.progress Which is: 10`.
Real run (exp/c5.out, produced by the C5 experiment): the worker's error line reads
`controller lost: the controller's heartbeat went stale after 102 unit(s), 13981 ms; 0 unit(s) computed; oracle checked 0 unit(s) over 0 sampled block(s), 0 wrong, 0 not performed; bracket checked 0 unit(s), 0 failed`
while the controller's own line for the same worker says `verification: 102 unit(s) computed; the oracle checked 102 of them over 11828 sampled block(s)`.

Smallest fix: in the leave path set result_.tally = whole() (the carried tally plus the live runner's total) before returning, or have entry.cpp omit the tally sentence when the reason is a controller-lost one.

## A3  Memory budget is checked for the start footprints only

Verdict: CONFIRMED. Severity: medium. It is a safety limit that can be exceeded, which would be high, but docs/safety.md:122-133 and :232 state the check is made once before any worker starts and that memory is not watched afterwards, so the gap is the schedule defeating a documented start-only check rather than a broken promise; the mixed schedule makes the start-only check understate the peak.

Evidence: safety.cpp:102-123 sums config.workloads[].memory; schedules.cpp:68-72 builds those from the start footprints; mixed.cpp:105-118 draws whole or half of the footprint for every Switch (the start is "the first rotation, made as any other", mixed.cpp:233-241). Test VerifyA3 (misc_ac.cpp): plan_mixed with 8 workers and a 64 MiB ceiling over seeds 1..400; budget = exactly the start total; check_memory passes; the peak total held over the schedule is larger. Output:
`seed 1: start 384 MiB passes the check against a ceiling of 384 MiB, later steps hold up to 480 MiB`
`seed 5: start 288 MiB ... later steps hold up to 448 MiB`
`seed 6: start 384 MiB ... later steps hold up to 416 MiB`

Smallest fix: when a schedule is planned, check workers x footprint_bytes (the worst case of every Switch) against the budget and limits.memory, or build config.workloads from the worst case.

## A4  A command written to a worker whose pipe is already closed disappears from the accounting

Verdict: PARTLY. Severity: low.

Evidence: orchestration.cpp:659-666 returns Ok on EPIPE before ++sent; dropped is only incremented at :700 (cap). Test VerifyA4 (controller_ac.cpp): one command to a worker whose control pipe has no reader. Output: `accounted 0, expected 1: the schedule told worker 0 one command; control: 0 message(s) sent, 0 done, 0 refused, 0 unanswered, 0 not sent`.
What does not hold: this is deliberate and pinned: the existing test ControllerControl.AFailureToWriteOnAControlPipeIsTheControllersAndAWorkerGoneIsNot (orchestration_test.cpp ~2127-2152) asserts `sent == 0` with the comment "nothing is counted as sent", and report.hpp:92 defines dropped as "not sent, because the worker had too many unanswered", not as every command not sent. Only the label "not sent" in describe() (orchestration.cpp:118) over-promises.

Smallest fix: count the EPIPE case in dropped and say so in report.hpp, or rename the printed label to "dropped (too many unanswered)".

## A5  The worker beats nothing while it replaces its runner

Verdict: CONFIRMED for the mechanism; not reproduced against a real controller. Severity: low (needs a replacement longer than the 10 s floor, i.e. a very large footprint or a loaded machine; the controller then kills a healthy worker, so it would be high if reachable).

Evidence: the beat is published only at tick start (verified_run.cpp:260-266); reconfigure (:468-506) runs close_runner, release, prepare and possibly a second prepare between ticks with no publish. The controller judges staleness by timestamp age alone (orchestration.cpp:795-803, ipc::liveness in heartbeat.cpp:68-78) and the only allowance, idle_allowance_ns, is set solely from a SetDuty Done (orchestration.cpp:1052), so a pending Switch or Rebind gives none; the allowance applies only to the no-progress check, not to the stale-timestamp check, so even SetDuty would not cover this. Test VerifyA5 (worker_ac.cpp): the replacement runner's creation advances the monotonic clocks by 30 s. Output: `gap_ns 30010000000 vs 10000000000: the worker's beat went 30010 ms without being refreshed while the Switch replaced the runner`.
Not shown: an actual SIGTERM from a real controller (would need a real 10 s+ replacement).

Smallest fix: publish a beat (progress unchanged) at the start of reconfigure, between close_runner and prepare, and again before a restoring prepare; or let the controller extend the stale allowance after sending a Switch or Rebind until its acknowledgement arrives.

## A6  Unbounded or quadratic growth in per-worker controller state

Verdict: PARTLY. Severity: low.

Evidence: orchestration.cpp:670-673 (told.push_back per Switch, duplicates included), :977 (linear find per error record), notes.push_back at :958, :1016, :1043 with no cap. Holds: no cap on notes; told grows to one string per Switch (at most kMaxSteps = 100000, mixed.hpp:106) and is scanned linearly per error record.
Does not hold as stated: a refusal note needs a worker acknowledgement, and the unanswered cap (64, :147) rate-limits commands, so notes cannot reach 4,000,000 faster than the worker answers; the cost per error record is at most about 100000 short-string compares (sub-millisecond), small against the 2 s / 10 s stale allowances; the product kMaxCommands = 4,000,000 is a schedule bound, not a reached state. Not measured; derived from the code only.

Smallest fix: keep `told` as a std::set (or unordered_set) of names and cap notes with a count of dropped notes, as findings does.

## A7  Rebind and Switch can silently revive a runner that stopped itself

Verdict: PARTLY. Severity: low.

Evidence: slice_runner.cpp:376-383 halt() sets halted_ inside the slice that finds the fault and the slice returns Ok; only the next run_slice answers kFinished (:130-131); finish() refuses only a second finish (:145-148), so close_runner works on a halted runner and prepare builds a fresh one. Test VerifyA7 (worker_ac.cpp), a runner that halts on slice 1 and queues a Switch, then a scripted successor: the run continues and ends with `reason is 0 (kDurationElapsed=0)` instead of kRunnerStopped; the test's other assertions (the successor ran slices; the halt's VerificationNotPerformed record was sent) pass.
What does not hold as a defect: the halt record is sent before the replacement (finding count 1 in the test), so the controller still sees a not-performed record (a verified and incomplete run, exit code 6), and the replacement builds fresh control state, which is a legitimate reset. The doc (docs/journal/11-integration-m2.md:364) defines kRunnerStopped for a runner that stops itself, without saying a rotation may not follow. It is a decision to record, with a test, not a bug.

Smallest fix: if the decision is "a halted run stays halted", have close_runner/reconfigure check a halted flag (from run_slice's last answer) and end with kRunnerStopped instead of replacing the runner.

## A8  The schedule check does not look at the start list or at label uniqueness

Verdict: CONFIRMED. Severity: low (the two builders produce valid starts and unique labels).

Evidence: schedule.cpp:85-122: no check of Start contents, no check that labels differ; schedule.hpp:32-37 says phase labels are "different at every step". Test VerifyA8 (misc_ac.cpp): two Start{"",0} and two steps both labelled "same". Output: `problem() found nothing wrong with Start{"",0} x2 and two steps both labelled "same"`.

Smallest fix: in problem() refuse a Start with an empty workload or zero footprint and a step whose phase equals the previous step's.

## A9  Smaller points

Verdict: PARTLY. Severity: low (unreachable from the controller; hand-run worker only).

- next_slice_units product (verified_run.cpp:97-109): CONFIRMED as an overflow, threshold misstated. last_units x target wraps at 2^64, i.e. last_units = 2^20 and target above 2^44 ns = 4.88 h. The target is interval/2 (verified_run.cpp:230, kTargetDivisor = 2) and further capped by the busy remainder (:531), so the interval must exceed about 9.8 h, not 4.9 h. Test VerifyA9: `next_slice_units(kMaxSliceUnits, 1, 2^44) Which is: 1, expected 1048576`.
- static_cast<int>(interval_ns / 1e6) (verified_run.cpp:229, scheduler.cpp:132) and the clamp with a low bound above its high bound (verified_run.cpp:330): holds by derivation; the worker accepts any interval up to kMaxMilliseconds (entry.cpp:46, 202); the cast wraps from 2^31 ms (24.8 days), not 4.9 h as the wording suggests, and gives clamp(…, 1, hi) with hi <= 0 from 2^31 to 2^32 ms. Not run.
- roster_of (roster.cpp:35-45): holds by derivation: footprint *= 2 wraps to 0 at 2^64, and `0 <= ceiling` keeps the loop running if nothing is creatable. Not run (needs a ceiling above 2^63).
- creatable-at-128 KiB-not-192 KiB: speculative, no such workload looked for; not verified.
- Stop queue behaviour "by design": not disputed.

Smallest fix: compute the product in unsigned __int128 or divide first for the slice sizing; bound the worker's interval argument to int range in pacing_of; bound the roster loop by the width.

---

## C1  Run duration overflows int64 in the controller

Verdict: CONFIRMED. Severity: low (absurd input), but `--check-config` accepts it.

Evidence: orchestration.cpp:429, no bound on the --config path (config.cpp:297-305 refuses only <= 0; core/duration.cpp:46-50 accepts up to INT64_MAX ms). Real run (exp/c1.toml, `duration = "10000000000s"`): `--check-config` prints `ok: c1.toml: stress for 10000000000s, 2 workload(s)`; the run prints `[0.000 s] deadline reached: stop requested`, both workers die with `--duration-ms: expected a whole non-negative number, got '-8446744073709'` (exit 125), `run ended after 0.005 s: 0 of 2 worker(s) completed`, `verdict: incomplete`, exit code 3. The banner still says `limit duration: enforced (10000000000s)`.

Smallest fix: refuse in config validation (or in prepare()) a duration above the cap the command line already uses (kLongestMilliseconds = INT64_MAX / 1e6 / 8).

## C2  SIGCHLD inherited as ignored makes every run abort at the first full reap

Verdict: CONFIRMED. Severity: medium (no report, no fates, no result files, exit 1 for a run whose workers completed; trigger is environmental).

Evidence: real run via a launcher that sets SIGCHLD to SIG_IGN and execs (exp/sigchld_ign.py), config tests/fixtures/config/short-stress.toml: output ends `[1.006 s] deadline reached: stop requested` then `loadforge: reap: waitpid(any child, without waiting): No child processes (errno 10)`, exit 1; the same command without the launcher prints the full report and exits 0. Code: orchestration.cpp:386 ignores only SIGPIPE; platform/syscalls.cpp:502-509 and 139-147 turn ECHILD into an error.

Smallest fix: set SIGCHLD to SIG_DFL (ignore_signal-style sigaction) at the top of Run::prepare().

## C3  Result file is published without fsync and with the close error unread

Verdict: CONFIRMED. Severity: medium (the one artifact kept is the weakest written; consequence needs a power loss or a close-time error, neither reproduced).

Evidence: result_files.cpp:44-56. strace of a real run (`-e trace=fsync,fdatasync,rename,close`, exp/strace.out): the only relevant call is `rename("out1/run.json.partial", "out1/run.json")`; no fsync, no directory fsync. Failure run on an 8 KiB tmpfs: `loadforge: cannot write the result: cannot write small/run.json.partial: No space left on device`, exit 4, and `run.json.partial` (4096 bytes, truncated) is left behind (ls output). The first-format-published-but-unlisted point holds by code: write_results (result_files.cpp:114-116) returns only the error and discards `paths` on a later failure (a two-format failure was not run). The unread close error holds by code (destructor close of the ofstream; flush() only checks the buffered write) and was not reproduced.

Smallest fix: write through a descriptor, fsync the file, check close(), rename, fsync the directory; remove the .partial on failure.

## C4  The terminator is written even when the run was aborted or journaling had already failed

Verdict: PARTLY. Severity: medium (an aborted run's journal is erased by the next start; the cause was on stderr and the exit code was 1).

Confirmed, by a real run: cli.cpp:974 calls end_journal for a ControllerError. Using the C2 trigger to cause a controller error with `--journal j.bin`: exit 1, `loadforge journal j.bin` reads `journal j.bin: ended cleanly; 2 entries`, and the next `stress --journal j.bin` prints `previous journal j.bin: ended cleanly; 2 entries; ...` and the file is replaced (only j.bin, no j.bin.1). So the terminator means only "the run function returned", as docs/JOURNAL.md §2.53 and cli.cpp:754-757 say, and an aborted run's journal is deleted as clean.
Rejected in part: the ENOSPC scenario. With the disk staying full (8 KiB tmpfs filled), a journal fault stops journaling (`journal: append: storage failed: write(fd 3): No space left on device`), then the terminator write also fails: stderr says `the journal could not be closed, and will read as ended abnormally`, and the file (0 bytes) replays as `empty` and is not "terminated". The claim holds only for a fault that clears before the close (not reproduced). The fault is also reported in the run's report (journal_report_.fault) and the event line.

Smallest fix: write the terminator only when the run returned a report (or write a distinct "aborted" record on the error path, and none after journal_fault).

## C5  Blocking fsync and unbounded schedule catch-up inside the loop that publishes the controller heartbeat

Verdict: CONFIRMED for the mechanism; reproduced at an extreme setting. Severity: low (absurd input unless the filesystem stalls for more than the allowance); the symptom is a wrong exit code (3) for a live controller.

Evidence: orchestration.cpp:757-768 (append and fsync per forced write), :631-640 (while loop over due steps), beat at tick start (:590). Real runs on this machine (exp/c5*.out): `stress --test mixed --duration 20s --workers 2 --rotation 1ms --footprint 4MiB --journal j5.bin`: workers print `controller lost: the controller's heartbeat went stale after 102 unit(s), 13981 ms`, exit with code 124, `run ended after 15.484 s: 0 of 2 worker(s) completed`, `verdict: incomplete`, exit 3, while the controller was alive (it wrote 3985 journal entries, each synced). The same command with `--rotation 10ms` and `--rotation 100ms` completes 2 of 2, exit 0, so the review's 10 ms example does not fail on this disk. Not shown: a stalling filesystem.

Smallest fix: publish the controller beat inside the schedule loop (every step or every few) and do not fsync on every forced entry (sync once per tick), or refuse a rotation below the tick interval.

## C6  --journal PATH on a file that is not a journal moves that file; two runs on one path are not excluded

Verdict: CONFIRMED. Severity: low.

Evidence (real runs, exp/c6, exp/j3.bin*): `stress --config config.toml --journal config.toml` prints `previous journal config.toml: corrupt; 0 entries; kept as config.toml.1`; config.toml.1 is the user's file, config.toml is the new journal. Two runs started 0.3 s apart on j3.bin: the second prints `previous journal j3.bin: ended abnormally between records; 1 entry; ...; kept as j3.bin.1`; the first run goes on appending to the renamed file and writes its terminator there, so j3.bin.1 (the "kept crash evidence") reads `ended cleanly; 3 entries`.

Smallest fix: refuse a non-empty file that does not start as a journal (corrupt at the first record) instead of renaming it, and take an exclusive flock on the journal file at open.

## C7  Replay is looser than its format comment

Verdict: CONFIRMED (a, b, c). Severity: low (a CRC guards against accident, not against a crafted file; both endings are kept and printed).

Evidence (misc_ac.cpp):
- (a) VerifyC7.AFlippedLengthBitInTheLastRecordIsATornTailNotCorrupt: two records, bit 8 of the last record's length flipped. Output: `replay.ending Which is: <03> (kTornTail), expected kCorrupt: ended abnormally inside a record`; code format.cpp:155-165 classifies "more than the bytes left" as TornTail before any checksum, against format.hpp:15-18.
- (b) VerifyC7.ANegativeOrBackwardsElapsedTimeReplaysAsGood: entries at 9 s and -5 ns with valid checksums. Output: `replay ending: ended abnormally between records, entries 2` (no refusal), against format.hpp's stated non-negative, non-decreasing time and zero bytes behind a clear presence bit (decode_entry :112-142 checks neither).
- (c) VerifyC7.ALabelWithAnEscapeSequenceIsPrintedRaw: a phase label of ESC [2J, a newline and text. Output: `line.find('\x1b') Which is: 14`, `line.find('\n') Which is: 18`; render_entry (journal_rig.cpp:60-65) prints labels raw.

Smallest fix: in scan, when the length exceeds the bytes left only call it a torn tail if the remaining bytes are fewer than a header or the record is the last, and decode_entry should refuse a negative or backwards time; escape non-printable bytes in render_entry.
