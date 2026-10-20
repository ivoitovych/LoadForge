# Independent review response — request of 2026-10-20 03:52 UTC

- **Request:** [review-request-2026-10-20-0352-UTC.md](review-request-2026-10-20-0352-UTC.md).
- **Reviewed:** `main` at `c7e862fb68ab133af6b684b23ee2cf69e45e28b5` (2026-10-20).
- **Scope:** process supervision, signal classification, startup/heartbeat timing and the record channel; targeted review of related tests, kernel adapter and documentation. Not a complete review of all modules or of open feature-stream PRs.
- **Method:** remote source and test review through the repository; one standalone Linux kernel experiment confirming `kill(SIGKILL)` succeeds on an unreaped zombie. The LoadForge binary was **not** compiled or executed in this review environment; source-derived reproductions are proposed tests, not reported as already passing or failing.
- **Prior work excluded:** [comprehensive project review](comprehensive-project-review-2026-10-19-0449-UTC.md), [M1 exit review](m1-exit-review-2026-10-20-0444-UTC.md), [ROADMAP](../docs/ROADMAP.md), and JOURNAL §6 were consulted. In particular, the previously fixed seqlock fence and SIGPIPE problems and the already-known grandchild-on-controller-SIGKILL gap are not rediscovered findings.

## Verdict

**Changes requested before relying on M1 supervision verdicts for hardware fault attribution.** Two newly identified conditions permit wrong causal or liveness verdicts. The record-frame reader looks appropriately bounded in the actual controller call path and was not found to silently recover after a damaged frame. Neither observation constitutes a claim that all other paths have been proved safe.

| ID | Priority | Finding | Confidence |
|---|---|---|---|
| EXT-01 | **Major: attribution correctness** | Sending SIGKILL to an already dead worker can convert an external/OOM kill into a claimed controller stop/timeout | High; kernel behavior separately reproduced |
| EXT-02 | **Major: liveness correctness** | A controller busy spawning workers does not heartbeat; late workers inherit an already-expired startup allowance | High from control flow; timing repro not yet executed |
| EXT-03 | **Documentation / interpretation risk** | “Fault signal” is described as a hardware event even for software-generated signals | High for the documentation counterexample |

## EXT-01 — A successful `kill` does not prove the controller caused the SIGKILL death

**Where**

- `src/controller/orchestration.cpp` lines 408–425: `Run::send()` calls `send_signal(worker.pid, signal)` and sets `worker.evidence.signal_sent = signal` on success **or ESRCH**; `Run::reap()` collects the actual wait status later (lines 488–510).
- `src/controller/supervision.cpp` lines 79–94: a matching `signal_sent` overrides other explanations and labels a `SIGKILL` as `kTimedOut` or `kStoppedByController`.
- Same file, lines 122–128: an alleged controller-sent `SIGKILL` is excluded from OOM candidacy.
- `tests/unit/controller/supervision_test.cpp` tests the same-number comparison but does not establish that the controller's signal *caused* the reported death.

**What goes wrong**

The controller polls/drains and only then reaps; it may attempt an escalation against a child that exited earlier in the tick and has become an unreaped zombie. On Linux, `kill(zombie_pid, SIGKILL)` can return **success**, although it cannot change the already recorded cause of death. This was checked independently with a short `fork` → child `_exit(0)` → parent `kill(pid,SIGKILL)` → `waitpid` experiment: `kill` succeeded and `waitpid` still reported exit 0.

For the dangerous case, let the worker be killed **externally by SIGKILL** before the controller's escalation. The controller's subsequent `kill(pid,SIGKILL)` on the unreaped zombie succeeds. `signal_sent=SIGKILL` is recorded. Reaping yields a SIGKILL exit status, but `classify_signal` treats the matching number as proof of controller causation and the OOM candidate predicate refuses even to consult the kernel's counter. The report can say **timed out** or **stopped by controller** when that was not the cause.

The same issue extends to a race where the external SIGKILL and controller escalation cross before reaping. Importantly, changing the handling of ESRCH **alone** does not solve this: a successful kill of a zombie is the confirmed counterexample.

**Reproduction/regression test**

1. Add a real-kernel T7 case that establishes that the child has exited from an external SIGKILL but remains unreaped (use `waitid(..., WNOWAIT)` in the test harness, or another deterministic synchronisation barrier).
2. Make a simulated controller escalation issue `SIGKILL` before the controller processes that wait status. Preserve the already-existing worker PID until reaping.
3. Assert that the report does **not** claim the controller's kill *caused* the death or suppress OOM consideration merely because `kill` returned success. A companion case with an actual running child killed by the controller is needed.
4. Retain the standalone Linux kernel behavior test so the fake cannot model successful `kill(zombie)` as ESRCH and accidentally hide the bug.

**Suggested correction**

Separate **signal attempted** from **cause established** in `Evidence`. A controller-issued SIGKILL plus a SIGKILL wait status is not itself proof of delivery-before-death. Reap known exits before signalling where practical, but that only shrinks the race; it does not close it. If the interface cannot establish exact causality, report the ambiguous provenance rather than a definitive stop/timeout or definitive OOM kill. `pidfd` can improve identity/race handling, but PID identity alone does not demonstrate which of two racing SIGKILLs terminated the process.

## EXT-02 — Spawn time consumes heartbeat and deadline allowances before the monitoring loop starts

**Where**

- `src/controller/orchestration.cpp` lines 208–219: `started_` is assigned and slot 0 published in `prepare()`, **before pipe setup**.
- Same file, lines 245–302: `spawn()` constructs argv, calls `pool_.spawn(argvs)` for the entire worker set, then assigns **every** `progress_changed_ns` and `last_read_ns` from the old `started_.running_ns`. No controller beat occurs inside the spawn sequence.
- Same file, lines 366–408: `watch()` treats a never-published worker heartbeat as stale based on whole-run `elapsed_ns`, and compares other progress times to the same old start.
- `src/worker/scheduler.cpp` lines 49–78 and 115–151: a worker monitors slot 0 and leaves with `kControllerStale` when the controller's heartbeat is older than the allowance.
- `src/controller/orchestration.hpp`: defaults are 100 ms polling and **2 s** staleness. The source also promises that the controller forks before starting threads, so a separate heartbeat thread is not a current solution.

**What goes wrong**

A busy, but correct, controller can take more than two seconds to fork/exec many workers on a loaded machine, or can be delayed by the scheduler during spawn. During that entire interval the controller's shared-memory heartbeat is frozen. Early workers have started their null workload and are polling that old beat: they can exit **124 / controller lost** despite the controller being alive and continuing to spawn. When the controller eventually enters `tick()`, workers spawned late but not yet scheduled to publish their first heartbeat can immediately be deemed **stale** and sent SIGTERM, because the grace was counted from *before* they existed.

There is a related duration effect: `duration_ns_` is compared against `started_` established before spawning. If spawning takes a substantial share of a short configured duration, the first tick can request the deadline stop immediately, even though late workers had little or no runtime.

**Reproduction/regression test**

1. Configure N >= 2 workers, staleness 2 s, interval 100 ms.
2. Arrange for the **controller's worker-spawn loop** to take >2 s after the first worker is running, without killing or wedging the controller. Use a test-only injected delay at a fork boundary through the syscall seam / worker-pool harness, not an assumed naturally slow CI runner.
3. Assert that the early worker does not emit “controller lost” while the controller is making legitimate spawn progress.
4. Delay a late worker's initial scheduling/publish until just after `spawn()` returns; assert that its first unobserved heartbeat is not an instant timeout merely because `started_` predates its fork.
5. Test and explicitly document whether configured duration covers startup or active workload time.

**Suggested correction**

Make startup its own bounded supervision phase. Refresh the controller heartbeat throughout potentially long provisioning/spawn activity (possibly by allowing a callback/beat between successful forks), without adding an unsafe post-fork thread. Anchor each worker's first-heartbeat allowance to its launch/admission time or the transition into active supervision, rather than the beginning of `prepare()`. Decide explicitly whether the configured run duration begins only once workers can make progress; test the chosen contract. Simply increasing the global 2-second timeout trades false positives for slower real-hang detection.

## EXT-03 — A crash signal is not evidence, by itself, of faulty hardware

**Where**

- `src/controller/supervision.hpp` lines 14–18 says a worker killed by SIGBUS is “a hardware event.”
- `src/controller/supervision.cpp` lines 46–49 and 79–86 treats SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, and SIGTRAP as `kFault`.
- `tests/unit/controller/supervision_test.cpp` explicitly locks in these six members.

**Why the interpretation is unsafe**

These signals establish a **process termination**, not the origin of that termination. `std::abort()` raises SIGABRT due to software logic; accessing a file-backed mapping past EOF can raise SIGBUS because the application used an invalid mapping; `raise(SIGTRAP)` is software-generated. A SIGSEGV may be a mundane null-pointer bug. A fault-labelled worker exit should be preserved as evidence, but should not be presented as evidence of defective hardware on its own.

The user-facing `describe(kFault)` currently says only “fault,” which is suitably noncommittal; this is chiefly a **documentation and downstream interpretation** risk rather than a request to discard crash signals or merge them with ordinary errors.

**Recommended regression/documentation test**

Use a real worker `abort()` case and a software-induced SIGBUS case, assert the raw signal and provenance in the report, and ensure any product-level hardware verdict remains **unattributed / unknown**, not “hardware fault proven.” Replace “is a hardware event” in the header with “is a process fault that may have either software or hardware causes.” Treat the distinction as part of the M2+ verification and isolation contract.

## What appears sound in the focused reading

- `src/worker/ipc/record.cpp` checks magic, version, length <= 64 KiB, and CRC before returning a record; it makes the first detected frame fault permanent and does not resynchronise. In the actual `RecordSource::drain()` path it feeds one bounded `kChunkBytes` read and parses records before feeding another chunk. I found no unbounded allocation caused by an attacker-controlled frame length **in that path**.
- `Run::drain_after_death()` has both a drain-count and a wall-time bound, reflecting the earlier review's fix.
- The worker-pool partial-spawn unwind and descriptor CLOEXEC hand-off are explicit. The existing known issue of grandchildren following the controller's uncatchable death remains distinct from the present findings.

## Testing limits and recommended priority

- **Performed:** static inspection of the cited implementation, adjacent headers/tests, previous reviews, and one isolated Linux experiment proving successful `kill(SIGKILL)` on an unreaped zombie.
- **Not performed:** building LoadForge, its `ctest` suite, TSan/ASan, ARM64 execution, mutation sweeps, or reproducing these races using the LoadForge executable. They must not be inferred from coverage reports.
- **Priority:** first EXT-01 (it can change the *cause* in an existing worker report), then EXT-02 (it can report a living controller as dead and kill healthy workers during legitimate setup), then EXT-03 (tighten attribution language before hardware-verification output is added).
- **Expected closure evidence:** a new regression that is demonstrably red before each fix and green after; for EXT-01 include a real kernel exercise because the original SIGPIPE bug showed that the shared fake can be semantically wrong.

The review is deliberately scoped rather than claiming a full audit or a milestone sign-off.