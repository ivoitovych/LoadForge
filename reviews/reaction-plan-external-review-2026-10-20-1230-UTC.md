# Reaction plan for the independent review response of 2026-10-20

- **Written:** 2026-10-20 12:30 UTC.
- **Review:** [review-response-2026-10-20-0352-UTC.md](review-response-2026-10-20-0352-UTC.md). This is the first review from outside the project, answering [the request of 2026-10-20 03:52 UTC](review-request-2026-10-20-0352-UTC.md). It reviewed `main` at `c7e862f`.
- **Assessment:** all three findings are accepted. Each was checked against the source before this plan was written. None repeats a finding of the earlier reviews.

## Assessment of each finding

**EXT-01, accepted as major.** `kill(2)` succeeds on a zombie that has not been reaped. So a successful `send_signal` proves only that a signal was *attempted*, not that it caused the death. `Run::send()` records `signal_sent` on success and on ESRCH alike. `classify_signal` then treats a matching SIGKILL as the controller's own act, and the OOM predicate refuses to consult the kernel's counter. A worker killed externally (by the OOM killer above all) in the same tick as an escalation is reported as timed out or stopped by the controller.

The review is right on two further points. Reaping before signalling only narrows the window. And the shared fake models `kill` on a dead child as ESRCH, which is the same class of fake-versus-kernel divergence as the SIGPIPE bug (journal §1.23).

**EXT-02, accepted as major.** `started_` is taken in `prepare()`. `spawn()` then forks every worker without a controller beat, and every worker's first-heartbeat allowance and progress clock is anchored at `started_`. A slow spawn of many workers (more than the 2 s staleness) can therefore cause two things:
- early workers leave with `kControllerStale` while the controller is alive;
- late workers are judged stale at the first tick.

The run's duration is also counted from before the spawn.

**EXT-03, accepted as a documentation and contract fix.** A worker killed by SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE or SIGTRAP has faulted. Whether the cause is software or hardware is not established by the signal. `supervision.hpp` says "a hardware event", and that is wrong. The product's verdict on hardware is the verification and isolation layers' (streams 01 to 04 and 10), never the signal's.

## Work items

Each lands as its own PR with a regression test that is red before the fix and green after, and with the full gate set. The items are ROADMAP 10b to 10d. They belong to M1, whose supervision verdicts they correct, and M1 is not formally complete until they land.

1. **EXT-01: evidence of an attempted signal is not proof of its effect.**
   - Split `Evidence::signal_sent` into "attempted" and what can be established.
   - Before escalating, reap or peek (`waitid(..., WEXITED | WNOHANG | WNOWAIT)`) any worker that has already exited, so that a dead worker is never signalled.
   - Where a controller SIGKILL and a SIGKILL death still cannot be told apart, report the provenance as ambiguous, and always consult the cgroup OOM count.
   - Tests:
     - a real-kernel T7 case: a child dies of an external SIGKILL, is held unreaped with `WNOWAIT`, and is then escalated;
     - a companion case where the controller's own kill of a running child is reported as before;
     - a standalone kernel-behaviour test pinning that `kill` succeeds on a zombie;
     - the shared fake corrected to model that, with its own test.
   - Journal: a §1 entry for the kernel fact and a §2 decision for the new evidence model.
2. **EXT-02: startup is its own supervised phase.**
   - The controller beats between forks.
   - Each worker's first-heartbeat allowance and progress clock is anchored at its own launch.
   - The configured duration starts when the last worker has been launched; the decision is recorded in the journal and stated in the operator documentation.
   - Tests: a delay injected at the fork boundary through the seam, longer than the staleness allowance, with no `kControllerStale` from early workers and no stale verdict for late ones. Plus a duration test pinning when the clock starts.
3. **EXT-03: the signal names a fault, not its cause.**
   - Reword `supervision.hpp` and the operator documentation.
   - Add a real-worker `abort()` case and a software SIGBUS case (a mapped file read past its end) to the supervision catalogue. Each asserts the raw signal and that no hardware attribution is made.
   - State in `docs/verification.md` that a hardware verdict comes only from verification and isolation.

Order: EXT-01, EXT-02, EXT-03, as the review advises. Items 1 and 2 both change `src/controller/orchestration.cpp`, so they land one after the other, in one working branch each.
