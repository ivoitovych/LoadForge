# Journal fragment 34b — SIGINT and SIGTERM stop a run gracefully

**Written:** 2026-10-23 09:06 UTC
**Change:** `src/controller/stop_signals.{hpp,cpp}` (new), `src/controller/{orchestration,report}.*`, `src/platform/syscalls.*` (`divert_signal`; EINTR in `poll_readable`), `src/explore/search.*`, `src/main/{cli,outcome,run_result,trial_runner,explore_command}.*`, `src/reporters/console.cpp`, `README.md`, `docs/{safety,architecture,result-schema,IMPLEMENTATION,ROADMAP}.md`

## What is done

`loadforge stress` and `loadforge explore` catch `SIGINT` and `SIGTERM` from just before the workers are started until the run has ended. The handler writes the signal's number to a pipe and does nothing else; the tick's poll waits on that pipe with the workers' record pipes, so a Ctrl-C ends the wait at once and is read in ordinary code. The first signal stops every worker exactly as the deadline does (`request_stop`, the grace, the escalation of `watch()`), then the controller reaps, takes its last telemetry round, and returns a report like any run: the console report, the result files and the end of the journal are written as for a run that reached its deadline. A signal that arrives while the workers are being stopped kills them at once with `SIGKILL`. A run a signal stopped exits with 128 plus the signal: 130 for `SIGINT`, 143 for `SIGTERM`.

Shown on the real binary (two `sort` workers, a 60 s run, `kill -INT` after 4 s; the numbers are from this container):

```
[3.966 s] SIGINT received: stop requested
[4.002 s] worker 0 (pid 6719): exited with code 0
[4.050 s] worker 1 (pid 6720): exited with code 0
run ended after 4.050 s: 2 of 2 worker(s) completed
verdict: clean
the run was stopped by SIGINT: its exit code is 130
result written: out/run.json
journal j: ended cleanly; 3 entries
  t=4.0s phase "stopped by SIGINT" workload sort units 8 workers 2 alive 0 stale
```

`explore` stopped by `SIGINT` six seconds into its first trial printed `worst case: none; the search was stopped by SIGINT. What was measured before it follows; no candidate was ranked`, every candidate `stopped with the search`, and exited 130.

## Established by experiment

### A second signal can be written before the first, unless the handler holds every signal while it runs

The first version of the two-signal integration test sent `SIGINT` and then `SIGTERM` to the binary and found the run reporting `stopped by SIGTERM; a later signal killed the workers at once`. The handler had been installed with an empty mask, so `SIGTERM` was delivered inside the `SIGINT` handler, wrote its byte first, and the first signal of the pipe was the second one sent. The handler is now installed with every signal masked (`sigfillset`): two signals that arrive together are written in the order the kernel delivers them (the lower number first when both are pending), and one never runs inside the other. A test reads the mask back (`real_syscalls_test.cpp`).

### `poll` is not restarted by `SA_RESTART`, so `EINTR` from the tick's wait had to stop being an error

`SA_RESTART` makes the kernel restart a `read` or a `write` that a handler interrupted; `poll` is never restarted, whatever the flag says. Without a change the first Ctrl-C would have come back from the tick's wait as `record channel: poll: Interrupted system call` and ended the run with no report, which is the thing this change removes. `RealSyscalls::poll_readable` now answers an interrupted wait as a wait that ended with nothing ready (the same answer as an expired timeout), and the loop's next look finds the byte. Tested against the kernel with an `alarm` and a handler without `SA_RESTART`.

### The workers do not receive the terminal's Ctrl-C

A worker calls `setsid` (`WorkerLauncher::become_worker`) and leads a session of its own, so the foreground process group of the terminal, which a Ctrl-C signals, holds the controller and nothing else. A test starts the binary in a process group of its own and sends `SIGINT` to the whole group, as a terminal does: the run reports `2 of 2 worker(s) completed`, which workers killed by a `SIGINT` of their own would not have been. In the short window between `fork` and `setsid` a worker holds the handler it inherited and shares the pipe; a Ctrl-C there would have been counted as a second signal and killed the other workers. The handler therefore writes only in the process that installed it (`getpid()` against the installer's, `diverts_here`), which is async-signal-safe and costs no extra call in the worker.

### `sort` refuses a footprint under one unit

`--footprint 1MiB` is refused by `sort` (`a footprint of 1048576 bytes is smaller than one unit of 2621440 bytes`). The first run of the integration test waited 120 s for a console line that never came; it now stops waiting when the process has ended, and uses 8 MiB.

## Decisions

### D1. A pipe the handler writes to, not a `signalfd`

**Options.** (a) `signalfd`: block `SIGINT` and `SIGTERM` with `sigprocmask` and read them from a descriptor, no handler at all. (b) A handler that sets a flag the tick reads. (c) A handler that writes to a self-pipe the tick's poll waits on.

**Chosen: (c).** (a) blocks the signals in the controller, and a signal mask is inherited across `fork` and `exec`: every worker would start with `SIGTERM` blocked, which is the escalation's second step. The fix is an unblock in the child between `fork` and `exec`, one more call at the one place in the program where the order of calls is most delicate (journal §2.33), for no gain. (b) leaves the tick's wait to end by its timeout: up to `interval_ns` (100 ms) between a Ctrl-C and the stop, and the flag is a data race unless it is a `sig_atomic_t` that something re-checks. (c) wakes the poll the moment the byte is written, does one async-signal-safe thing in the handler, and works with the seam as it is: a fake of `Syscalls` can put the byte in the pipe itself, so every arm is tested without raising a signal. The handler is installed with `sigaction`, so a worker's `exec` gives it the default action again; the write end is non-blocking, so that a flood of signals fills the pipe and is dropped instead of blocking the process inside the handler.

### D2. Caught from just before the run to just after it

The controller installs the pipe after everything else is set up (the telemetry probed, the journal opened) and just before `Controller::run`, and releases it when `run` returns. A Ctrl-C during the setup, which can take a second on a slow machine, has the default action, as it did; a Ctrl-C during the isolation re-runs or the writing of the result files does too. **Options.** Install at the start of the command, so that the setup is covered: a Ctrl-C then waits in the pipe until the run begins and stops it at once, a run that starts only to be stopped; and a signal during the re-runs would have to be read by something. Rejected for the first; and nothing reads the pipe after the run. **Known edge:** a signal that arrives after the last tick and before `release()` is dropped (the run was over; it exits as it would have). `explore` keeps the pipe for the whole search, because the search runs between the controller's runs and asks the object whether a signal has come.

### D3. The first signal gets the deadline's grace; the second is the way out

**Options.** (a) The first signal gets a shorter grace than the deadline's, because a user waiting for the prompt is not waiting for a deadline. (b) The same grace. (c) The first signal kills at once.

**Chosen: (b), with the second signal as (c).** A verifying worker is asked to stop between two units and given 10 s for the unit it is in, then 1 s after `SIGTERM` (`docs/safety.md` §2); that grace exists because a worker killed in the middle of a unit is a worker whose unit is lost and whose fate says so, and a Ctrl-C is not a reason to make that likelier. A user who does not want to wait presses Ctrl-C again, and the controller sends `SIGKILL` to every worker not yet reaped, with no `SIGTERM` and no wait; the run still reports what it has, the killed workers' fate is `stopped by the controller`, and `stop_forced` in the report (and the console line `stopped by SIGINT; a later signal killed the workers at once`) says why. A third signal does nothing more: every worker already has its `SIGKILL`. **Open for the owner:** a shorter first grace for a signal (the same question D6 of 34a left for the ceiling).

### D4. The exit code: 128 plus the signal, below 5, 7 and 4, above 3 and 6

**Options.** Which number, and where in 5 > 7 > 4 > 3 > 6 > 0.

- *0, because the stop was asked for and the run reported.* Wrong: a script that runs `loadforge stress` and goes on to the next step on 0 cannot tell a run that was cut short from one that reached its duration, and the verdict can be `clean`.
- *A new number (8).* Any number is a new fact to learn, and the shell already has one for exactly this: a process that died of `SIGINT` is 130, of `SIGTERM` 143. A script that tests for 130 to stop a loop (`make`, `xargs`, a `for` in a shell that checks `$?`) keeps working, and it is the code the program gave before this change (README and `docs/safety.md` recorded 130 and 143 for the unhandled signal). The convention is also what lets a wrapper tell a Ctrl-C from a fault.
- *Rank.* Below 5 and 7: a wrong result found before the stop, and a stop by heat, are the things to read first (the argument of 34a D7). Below 4: a result file that could not be written is news the signal does not carry; a user who pressed Ctrl-C knows the run was cut short, and does not know that the report of it is lost. Above 3 and 6: both are consequences of the stop (a worker killed past its grace is a worker that did not complete; a check not made is a check cut short), and the signal is why. So 5 > 7 > 4 > 130/143 > 3 > 6 > 0, with `with_output_failure` turning 130 and 143 into 4 and keeping 5 and 7.
- *Which signal when two came.* The first: it is what asked for the end. The second only changes how the workers are stopped (`stop_forced`).

### D5. `explore`: the trial stops, the search reports what it has, and the code is the same 130 or 143

`ControllerTrialRunner` is given the signals object; its trial's run is stopped by the first signal as in `stress`, and the trial comes back as `TrialFault::kRun` ("the trial was stopped by SIGINT"), which the frozen `TrialRunner` contract allows and which needs no new fault. The search asks `SearchConfig::stop_signal` before every trial and after every trial that failed; when a signal has come it runs no more trials and returns what it measured, as a reference that found something already does: every candidate still in the search is `stopped with the search`, no candidate is ranked, there is no worst case, and the report says `the search was stopped by SIGINT`. The settle wait (up to `--settle-max`, minutes) polls the signals' pipe and ends on a signal. The exit code is 128 plus the signal, below 5 (a finding made before the stop is still a finding) and above 6 (a trial that did not finish, which the stop causes), and 3 never applies: a stop is not a failure to run a trial. **Rejected:** treating the stop as the search's failure (exit 3, no report): it loses the replicates the user waited for, and "what it measured so far" is what a user who presses Ctrl-C on a long search wants.

### D6. Where a stopped run says so: the console, the journal, and each worker's `termination`

As for the ceiling (34a D8): the result schema has no run-level field for why a run ended, and the verdict is the checks' alone. A run a signal stopped therefore shows in the console report (`stopped by SIGINT`, and a line under the verdict with the exit code), in the journal (an entry at the stop with the phase `stopped by SIGINT`, then `done`, and the replay says `ended cleanly`), and in every worker's `termination` in the result files (`; the run was stopped by SIGINT`). **Questions for the owner**, the same two as D8: a run-level `ended_by` at the next schema version, and whether a stopped run's verdict should be `incomplete`. Today a run stopped after every unit checked is `verdict: clean` and exit 130.

## What is not done

- A signal during the setup and during the re-runs and the writing of the results still has the default action (D2).
- `stress` does not read the pipe after the last tick, so a signal that arrives in the few milliseconds between the last reap and the release is dropped.
- A stop at the very start (a signal pending when the workers are about to be forked) stops the run at its first tick, after the workers exist; it does not skip the fork.
- The project still needs an outside reviewer.
