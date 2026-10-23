# Review of worst-case exploration (src/explore, trial_runner, explore_command, statistics)

Read-only. Nothing was built or run; every finding below comes from reading the code and deriving by hand. Line numbers are from HEAD of main.

## Findings

### 1. Corrected hardware errors are scored, ranked and exit 0 (contradicts README)
- src/explore/search.cpp:19-21 (`unscored`), src/main/explore_command.cpp:558-571 (`exit_code_for(report)`), tests/unit/explore/search_test.cpp:396.
- `unscored` treats only kFindings and kIncomplete as non-scores. A trial with verdict kCorrectedErrors is scored like a clean one, nothing in the report mentions it, and the exit code is 0 ("every candidate was scored").
- README line 218 says `explore` uses 5 "for the same things" as `stress`. `stress` maps kCorrectedErrors to 5 (src/main/outcome.cpp:55). The README table defines 5 as "an error the hardware reported, corrected or not".
- Failure: candidate A runs a worst-case load, and the hardware reports a corrected ECC error in every trial. Output is a normal worst-case report and exit 0. `stress` on the same load exits 5.
- The journal (33-explore-integration.md:49) admits "the report does not yet flag it". The unit test then pins the behaviour as intended, so the test asserts the opposite of the README.
- Confidence: high.

### 2. A trial that did not settle is scored and ranked like a settled one, with no trace in the report
- src/explore/search.cpp:147-170 (`measure` drops `TrialOutcome.settled` and `settle_ns`), src/main/trial_runner.cpp:372 (only a `--verbose` log line says "not settled"), src/main/explore_command.cpp:519-556 (render_report has no settle information).
- Failure: `--settle thermal.package` with a gauge that never flattens within 300 s. Every trial starts hot, the controlled starting state that trial.hpp promises is gone, and the report and exit code 0 look the same as for a settled search.
- Confidence: high that the flag is discarded (grep finds no use of `settled` outside trial.hpp and trial_runner). The impact is a silent loss of the controlled starting state.

### 3. `--settle-window` larger than `--settle-max` can never settle, and nothing refuses it
- src/main/trial_runner.cpp:105-116 (check_candidate does not compare the two), :399-409 (slope is evaluated only when `elapsed_ns >= window_ns`), src/main/explore_command.cpp:404-418 (defaults 30 s window, 300 s max).
- Failure: `--settle thermal.package --settle-max 20s`. The window stays at its default of 30 s, `elapsed >= 30 s` is never true before `elapsed >= max` returns `Settled{false, 20 s}`. Every trial waits the full 20 s and reports "not settled"; combined with finding 2, silently.
- It does terminate, so the phase cannot wait forever. It just cannot succeed.
- Confidence: high.

### 4. The load window starts at the last fork, so it holds worker start-up
- src/controller/orchestration.cpp:519-533 (`started_` is overwritten by each launch and ends as the last fork), :605 (stop at `started_ + duration_ns_`), src/main/trial_runner.cpp:364-366 (window is `[stop+1-D, stop+1)`).
- For a whole-millisecond duration the window is exactly [last fork, stop). It therefore includes exec, footprint allocation and unit set-up of the workers, during which they do no steady load. trial.hpp:73 says the window is "[load start, load start + duration_ns)".
- Failure: a power or frequency mean over a 2 s window with 150 ms of start-up has about 7% idle samples, and the bias is not identical between candidates with different footprints or thread counts (more threads fork sequentially and start later).
- Confidence: medium. The code path is clear; I did not measure the start-up time.

### 5. `delta` objective loses the part of the window before the first in-window reading
- src/statistics/summarizer.cpp:112-121 (a counter advance counts only pairs of readings both in the window), src/main/trial_runner.cpp:364-366.
- The window opens at the last fork, but the first round inside it is up to one sample interval later (the baseline round was taken before the fork, outside). So `delta` covers [first in-window reading, stop], not the window. `rate` is divided by the actual span and is fine.
- Failure: `--sample-interval 1s --duration 3s`, `delta` of an error counter. The counted span varies between about 2 and 3 s from run to run, depending on where the sampling grid falls. That is up to 33% noise or bias across replicates and candidates, which drift correction cannot see.
- Confidence: medium-high. The mechanism is certain; the size depends on the grid phase.

### 6. "separated from the best rival" can be claimed while a rival eliminated on one replicate was never examined
- src/explore/search.cpp:342-365 (`rank_against_rivals`).
- The rival is the eliminated candidate with the highest corrected mean. A candidate eliminated in round 1 on a single replicate (default `--replicates 1`) has no interval and a mean that is one noisy sample, so it can lose the rank to a rival with n >= 2 and then never be compared. If that n >= 2 rival has two near-equal samples, the result is kSeparated.
- Failure: four candidates, true means A=100, B=90, C=88, D=60, noise sd 15. A reads 70 in round 1 and is eliminated. B and C survive to round 2, and C is the best-ranked eliminated candidate with n=2. If B and C's two samples happen to be close, the report says "separated" and A, the true worst case, is never mentioned as uncertain.
- Confidence: medium. It is a design limitation more than a coding slip, but the wording overclaims.

### 7. The claimed interval is computed on the same samples that selected the winner
- src/explore/search.cpp:121-143 (the final top-up uses cumulative `pass_`), :334.
- `passes(final_replicates)` counts passes from zero cumulatively, so when halving already reached `final_replicates` no fresh replicate is taken. The winner's mean and interval come from samples that decided its survival (winner's curse).
- Failure (derived): 8 identical candidates, noise sd s, `--replicates 1 --final-replicates 3`. Rounds run to 1, 2 and 4 replicates, so the winner is selected on its own four samples. The expected selected mean is biased upward (for the first round about +1.4 s, the expected maximum of eight draws), and the 95% interval, centred on that mean, does not cover the true mean at the stated level.
- Confidence: medium-high that the bias is real. It follows from the algorithm; whether it counts as a defect is a design call.

### 8. The reference's degrees of freedom are overstated by one
- src/explore/search.cpp:271 (`max_degrees = references_.size() - 1`), src/explore/drift.cpp:458-477.
- `reference_variance` averages n-2 interior residuals, so the noise estimate has n-2 degrees of freedom for n >= 3 (1 for n = 2). Using n-1 gives a t that is too small when the correction term dominates.
- Failure: 5 reference readings, a winner with 10 replicates, constant candidate scores. The true df is 3 (t = 3.182), the code uses 4 (t = 2.776), so the half-width is 13% narrow. For 3 readings the code uses 2 (4.303) where the true value is 1 (12.706), which is 3 times too narrow.
- Confidence: high on the arithmetic; low impact, because the sample-spread term usually dominates.

### 9. A zero-width interval from tied or quantised samples
- src/explore/estimate.cpp:288-292, src/explore/drift.cpp:463-466, src/explore/search.cpp:364.
- With n = 2 equal samples (a quantised sensor) and two equal reference readings, stddev = 0 and the correction variance = 0, so the half-width is 0. The ranking then reports kSeparated for any gap greater than 0.
- Failure: candidates a={62,62} and b={61,61} in whole °C. The report says "separated" at 95% on 2 samples each. This is a limit of the plug-in variance, not an arithmetic error.
- Confidence: medium.

### 10. A reference trial that finds something aborts the search with exit 3, not 5, and discards every result
- src/explore/search.cpp:172-185, src/main/explore_command.cpp:581-583, README line 218.
- A reference trial with verdict kFindings (a wrong result, a hardware fault) becomes kReference. It maps to exit 3 and prints only an error line, with no report of the candidates already measured. For a candidate the same event is exit 5 with a report.
- Failure: a marginal machine whose reference load faults after three hours. Exit 3 ("a trial could not be run") and the results of all earlier trials are lost, rather than 5, "a fault".
- Confidence: high on the code path. Whether this is a defect or a deliberate design is the owner's call, but the exit code contradicts the README's "per candidate" wording.

### 11. Late, miscoded refusal: a duration at or below the sample interval
- src/main/explore_command.cpp:234-241 and src/explore/objective.cpp:111-115, src/statistics/summarizer.cpp:128-136.
- `--duration 1s` (equal to the default `--sample-interval 1s`) can leave fewer than two readings in the window: a `rate` has no span and a `stddev` or `slope` has one reading. The first reference trial then fails with kNoReading, exit 6 ("check not performed"), after a full trial has run. Nothing is checked at configuration time, though the objective is bound there.
- Confidence: medium. It is not silent, but the exit code and the timing are wrong for what is a configuration error.

### 12. Minor
- src/explore/search.cpp:289-290: `(ranked.size() + factor - 1) / factor` wraps for `--elimination` above SIZE_MAX - n. With `--elimination 18446744073709551615` and 4 candidates the sum is 2^64 + 2, which wraps to 2, giving keep = 0. Every candidate is "eliminated", there is no winner, and the report says "every candidate left the search with findings or an unfinished trial". Exit 0. The parser accepts any uint64. Confidence: high (arithmetic), low realism.
- src/explore/search.cpp:64-90 against src/main/trial_runner.cpp:122-143: the search's settle check takes the first status with the metric's name, while the runner accepts any scope that is readable. With a per-scope metric whose first scope is unreadable the search refuses a settle that the runner would run. The settle slope is read from the first series of that name in the signature (trial_runner.cpp:153-160), so other scopes are ignored.
- src/main/explore_command.cpp:530 and src/explore/trial.hpp: the "worst case: none; every candidate left the search with findings or an unfinished trial" text is also printed when the sole survivor finds something in the top-up and the others were merely eliminated (tests/unit/explore/search_test.cpp:424). The wording is false there.
- tests/unit/explore/drift_test.cpp:44-48: the test "AReadingOutOfOrderIsPassedOver" does not show the doc's claim. `baseline_at({{300,5},{100,9}}, 400)` returns 9, the out-of-order reading, because it is the last element. The behaviour is unreachable in production, because reference times are monotone, but the test and the doc describe something the code does not do.

## Checked and found correct
- t table (src/explore/estimate.cpp:246-249): all 30 values match the two-sided 95% Student t table. The steps beyond 30 (2.021, 2.000, 1.980) match df 40, 60, 120. Between entries the larger value is used. `t_critical(0)` is safe, and `n - 1` cannot underflow because it is only used for n >= 2.
- `estimate`/`moments`: the sample variance uses n-1, with no division by zero (n < 2 returns before it). `half_width_of`: `t * sqrt(sd^2/n + variance)` is the standard form.
- `reference_variance`: the residual of a middle reading against the line through its neighbours has variance sigma^2 (1 + w^2 + (1-w)^2). I derived it, and it matches the code. Division by n-2 is safe for n >= 3, the n = 2 case d^2/2 is unbiased without drift, and zero time span is guarded.
- `correction_gain`: the weights of the baseline mean less the anchor mean are right, and shared readings cancel. An empty `trial_times` produces an infinite `each` that is never used, so there is no NaN. `correction_of` passes `reference_replicates` as the anchor count, which equals the anchor readings, so the two agree. Every search has at least two reference readings (an opening one and a closing one after at least one candidate trial), so `n >= 2` always holds when the variance is used.
- `bracket_at`/`baseline_at`: for time-ordered points the span is positive when divided. Both ends are held.
- Window arithmetic: `[stop+1-D, stop+1)` is exactly D wide, holds the stop-instant round, and excludes the settle samples (the settle phase ends before the fork, so settle readings are before the window start). The mid-point used for drift is on the same running clock for every trial. A duration that is not a whole millisecond is rounded up for the run, not for the window.
- Successive halving: `keep = ceil(n/f)` strictly reduces for n >= 2 and f >= 2, so the loop terminates. Survivors have equal replicate counts at every pass boundary. Ties keep the earlier candidate. The `kLowerIsWorse` sign handling is right in both elimination and rival ranking. `close_reference` skips only when a reference has just run, so the last candidate trial is always bracketed from after.
- Settle phase: it terminates. The maximum is at most 30 days, the wait is at most `max - elapsed`, every wait is at least 1 ms, and the `int` cast of `wait_ms` cannot overflow (the interval is at most window/4 <= 30 days / 4 = 6.5e8 ms < 2^31). EINTR is tolerated. Sampler refusals and clock failures are reported as kRun.
- Overflow: durations are bounded to 30 days in the parser and in the runner, so the sums in `capacity_for` cannot overflow. `timeline_capacity` is computed in unsigned arithmetic.
- Summarizer: Neumaier sum, gauge stddev (n-1), least-squares slope (zero variance guarded), the window bound handling (start inclusive, end exclusive) and the absent-not-zero rule for Unavailable are all right. A counter reset is not counted as an advance.
- Exit codes: refusals (unknown or unavailable metric, bad statistic, bad configuration, duty-cycle mismatch) all exit 1. Parse errors exit 2. A trial that cannot run exits 3. A finding exits 5, an incomplete candidate exits 6, and 5 takes precedence over 6.
- Option parsing: duplicate options, missing values, a non-finite or negative slope, and an unknown statistic or direction are refused by name.

No outside reviewer has looked at this project yet; one is still needed.
