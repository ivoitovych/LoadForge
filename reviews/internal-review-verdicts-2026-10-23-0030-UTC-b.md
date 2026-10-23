# Verdicts on the review of the worst-case exploration code

Written 2026-10-23 00:29 UTC. Source: origin/main at 720fdbbe46ebcc89f3bb0312e71dd46a41bfa6c8, fresh copy built with the debug preset.
Tests: verify-b-tests/verify_b_test.cpp (suite loadforge_vb_tests, added to the copy's tests/CMakeLists.txt only; 19 tests, 17 fail on current code, each failure is a reproduction; the 18th "failure" is a harness note about an uninstantiated contract suite pulled in by an include, not a finding).
Run log: vb-run.log (exit code 1 in vb-run.exit; build exit codes in vb-build.exit, vb-build2.exit, both 0). Real-binary runs: vb-r11.log, vb-r5-delta.log, vb-r5-rate.log, vb-strace.log.
Severity scale as briefed: high = wrong worst case named, or wrong verdict or exit code against the README; medium = overstated interval or silently lost condition; low = unreachable or absurd input.

## 1. Corrected hardware errors scored, ranked, exit 0
Verdict: CONFIRMED. Severity: high. Kind: defect against the README (the gap is admitted in docs/journal/33-explore-integration.md "the report does not yet flag it", but the exit code is not discussed there; JOURNAL 2.56 says 5 is for "a finding", and README line 218 says explore uses 5 "for the same things" as stress, where 5 includes "an error the hardware reported, corrected or not").
Evidence: test VB01 (ModelRunner returning Verdict::kCorrectedErrors for every trial): the report names a worst case ("b: 60 +/- 0"), ranking "separated", no word "corrected" anywhere; cli::exit_code_for(report) = 0 while cli::exit_code_for(Verdict::kCorrectedErrors, ...) = 5 (the stress mapping). The unit test search_test.cpp:396 pins the current behaviour.
Smallest fix: in render_report print a per-candidate "corrected errors" line, and in exit_code_for(report) return 5 when any trial verdict was kCorrectedErrors (carry the verdict, or a flag, in CandidateReport); change the pinned test.

## 2. A trial that did not settle is ranked like a settled one, with no trace
Verdict: CONFIRMED. Severity: medium (silently lost condition). Kind: defect in the report; "unsettled is not an error" itself is a documented decision (JOURNAL 2.54), silence in the report is not.
Evidence: test VB02 (runner returning settled=false, settle_ns=max for every trial): report text contains no "settle"; exit code 0. Code: Search::measure drops TrialOutcome.settled; only a --verbose line in the runner mentions it (seen in the real run: "trial reference: clean, not settled after 0 ns").
Smallest fix: keep settled per observation, print "N of M trials did not settle" in the report, and (owner's choice) exit 6 or warn when any did not.

## 3. --settle-window larger than --settle-max can never settle and is not refused
Verdict: CONFIRMED. Severity: medium (together with 2 the lost condition is silent); alone low. Kind: defect (missing validation).
Evidence: tests VB03: (a) the command line "--settle thermal.package --settle-max 20s" is accepted with window 30 s > max 20 s; (b) the trial runner given window 30 ms and max 20 ms over a perfectly flat gauge returns settled=false, settle_ns=20 ms, while the control (window 30 ms, max 500 ms) settles. It does terminate, as the review says.
Smallest fix: refuse window_ns > max_ns in check_candidate (kCandidate) and in the argument parser, or clamp the default window to max.

## 4. The load window starts at the last fork, so it holds worker start-up
Verdict: PARTLY. Severity: low to medium (size unmeasured for large footprints). Kind: documented design limitation in part: JOURNAL 2.55 and the integration journal say the window is anchored on the deadline and "the start is therefore the last launch to within a controller tick"; they do not mention exec or footprint set-up.
Evidence: orchestration.cpp:441-444 takes the baseline round before the forks, line 532 overwrites started_ at each fork, nothing gates the start on the workers' kStarted record, so the window [stop+1-D, stop+1) begins at the last fork plus the deadline tick's lateness. Measured with strace on the shipped (debug) binary, 1 GiB footprint: fork to exec 1 ms, to the workload thread 11 ms; the footprint set-up time itself was not separable (the worker exited 1.497 s after the fork for a 1 s duration, so up to about 0.5 s of set-up or wind-down exists at 1 GiB). Also a correction to the review: a late deadline tick (observed 64 to 76 ms late on this machine) moves the window start later, so it removes start-up from the window rather than adding it; the bias depends on tick lateness, not only on start-up.
Not claimed: the "about 7% idle samples" figure; 150 ms start-up was not shown.
Smallest fix: have the controller record a start instant after all workers report kStarted (or after set-up) and anchor the window on it, or state the limitation in trial.hpp.

## 5. delta loses the part of the window before the first in-window reading
Verdict: CONFIRMED (mechanism and loss); the review's size estimate PARTLY wrong. Severity: medium. Kind: defect (objective does not measure the window it names).
Evidence: test VB05 (rounds at 0, 1 s, 2 s, 3.005 s, counter 100/s, window built as trial_runner builds it): delta = 200 where the load advanced 300. Real binary, frequency.busy_ticks, duration 3 s, sample interval 1 s: rate 400.5, 400.6 ticks/s but delta 801.9, 788.3, i.e. about 2.0 s of the 3 s (a 33% loss). The round at the last fork is always outside the window (window start is at least fork+1 ns), so the first counted pair starts one sample interval in. The loss is therefore about one sample interval, not a random 0 to 1 interval as the review said; the run-to-run variation seen was about 1.7% (delta sd 6.4 against rate sd 0.2). delta is a bias that is nearly equal across candidates, but it breaks comparison across different durations or intervals and inflates relative noise.
Smallest fix: take one reading just before the window start into the summarizer's window (include the last pre-window reading as the baseline for the first pair), or divide delta by its counted span and scale to the window.

## 6. "separated from the best rival" claimed while a rival eliminated on one replicate was never examined
Verdict: PARTLY. Severity: low. Kind: design limitation of successive halving with one replicate (not documented as such).
Evidence: Monte Carlo VB06, 20000 seeded searches (seed 20261022), true means A=100 B=90 C=88 D=60, sd 15, --replicates 1, final 2: wrong worst case named 7598 times (38%, expected for one noisy sample at this noise level); "separated" printed 68 times, 14 of those with the winner not A, and in 4 of the 14 A had been dropped on a single sample (exactly the review's mechanism). Code: rank_against_rivals picks the highest-mean eliminated candidate; a one-sample rival has no interval and yields kIndistinguishable, so a one-sample rival can only block a "separated", never create a false one; the false claim needs a lower-mean rival with n>=2 to outrank it. Rate is 0.02% of searches here; the substantive problem (38% wrong winner at replicates 1) is shown by the report's width only if the user reads n.
Smallest fix: when any eliminated candidate has fewer samples than the winner's interval needs (n<2), report kIndistinguishable ("an eliminated rival has no interval").

## 7. Interval computed on the samples that selected the winner
Verdict: CONFIRMED. Severity: medium. Kind: defect/undocumented design limitation (no mention in the journals or ADRs; grep found nothing).
Evidence: Monte Carlo VB07, 40000 seeded searches (seed 7): eight identical candidates (true mean 50, sd 1), --replicates 1, --final-replicates 3. Mean bias of the reported worst-case mean +0.608 (sd units); coverage of the 95% interval 87.3%; control with the same number of fresh samples: 95.1%. The final top-up adds no sample when halving has already reached final_replicates (passes() counts cumulatively), as the review said. Review's +1.4 is the first-round bias; the final reported bias is 0.61 here.
Smallest fix: after elimination, take final_replicates fresh trials of the winner that were not used for selection, and compute mean and interval from those alone.

## 8. Reference degrees of freedom overstated
Verdict: CONFIRMED in direction, review's numbers wrong. Severity: medium when the correction term dominates, low otherwise. Kind: defect (max_degrees = readings-1 is not the variance's df).
Evidence: VB08 Monte Carlo (400000 draws per n, seed 88): effective (Satterthwaite) df of reference_variance: n=3 0.997, n=4 1.38, n=5 1.87, n=6 2.34, n=8 3.38; code uses n-1 = 2,3,4,5,7. So the review's "n-2" (1,2,3,4) is also too high for n>=4 (n=5 is about 2, not 3; t about 4.3, not 3.18). Coverage test (200000 draws, constant candidate samples, only the reference noisy): 3 readings: 86.3% with the code's df=2 against 95.8% with df=1; 5 readings: 93.5% against 98.4% with df=2.
Smallest fix: use max_degrees = (n-1)/2 rounded down but at least 1 (a conservative bound near the measured values), or compute the Satterthwaite df of the gain-weighted variance.

## 9. Zero-width interval from tied or quantised samples
Verdict: PARTLY. Severity: low. Kind: design limitation (plug-in variance of a t interval; no mention in the journals).
Evidence: VB09 (a): readings a=62 b=61 constant: "a: 62 +/- 0 (95% CI, n=3, sd 0, correction sd 0)", ranking "separated", confirming the zero width (n=3 here because of the final top-up). (b) 20000 seeded searches of two identical candidates with whole-degree readings (truth 61.5, noise sd 0.3): "separated" in 6.1% against a nominal 5%, so the miscalibration is mild, not gross.
Smallest fix: when the sample sd is zero, report the quantisation step as a floor for the half-width (or print "no spread seen") and do not claim "separated".

## 10. A reference trial that finds something aborts with exit 3 and discards the results
Verdict: PARTLY. Severity: medium. Kind: documented design (JOURNAL 2.56: "3 a trial could not run or the reference was unusable"), but it does not match the README's "per candidate" 5 and the loss of earlier results is not discussed.
Evidence: test VB10: reference's third trial returns kFindings after 7 trials: the search returns an error "reference unusable: the reference ref ended findings", exit_code_for = 3 (expected 5), no report of the candidates already measured. Code path confirmed as described.
Smallest fix: map a reference kFindings/kCorrectedErrors to exit 5 and print the error plus the trials' results so far (or at least the reference's finding).

## 11. Late, miscoded refusal when duration equals the sample interval
Verdict: CONFIRMED. Severity: low (message is clear, exit code and timing wrong for a configuration error). Kind: defect.
Evidence: test VB11: the parser accepts "--duration 1s --sample-interval 1s" for a rate objective. Real binary: "explore --objective frequency.busy_ticks:rate:higher --candidate a:sort:1 --duration 1s --sample-interval 1s" ran one full 1.19 s trial then ended "no reading for the objective: frequency.busy_ticks has too few readings for rate per second", exit 6 (log: vb-r11.log).
Smallest fix: in the argument parser refuse duration < 2 * sample interval when the statistic is rate, slope, stddev or delta.

## 12. Minor points
12a (elimination factor wrap): CONFIRMED. Severity: low (absurd input). Kind: defect. Test VB12a: --elimination 18446744073709551615 is accepted; with 4 candidates keep wraps to 0, the report says "worst case: none; every candidate left the search with findings or an unfinished trial", exit code 0. Fix: compute keep as n/f + (n%f != 0), or bound the factor at parse.
12b (settle scope): PARTLY. Severity: low. Kind: design inconsistency. Test VB12b: with two scopes, the first unreadable, the search refuses ("settle metric level.reading of ref is unavailable: dead") while the runner accepts and runs, but the runner reads the first series only and ends settled=false at max (settle_ns 500 ms). So the search refusal prevents a settle that could not succeed; the refusal is the right side, the runner is the inconsistent one, and it fails silently (see 2). Fix: make the runner refuse the same way, or settle on the same scope the search checked.
12c (false "none" sentence): CONFIRMED. Severity: low. Kind: defect in wording. Test VB12c: candidate a ends with findings in the final top-up, b was merely eliminated with a score of 50; the report prints "every candidate left the search with findings or an unfinished trial" (exit would be 5 from the finding). Fix: say "no candidate survived the search" and list eliminated ones as such.
12d (AReadingOutOfOrderIsPassedOver): CONFIRMED. Severity: low. Kind: test/doc mismatch. Test VB12d passes: baseline_at({{300,5},{100,9}}, 400) returns 9, the last element, so the test does not show a reading being passed over. Fix: reword the doc and test, or drop the claim.

## Summary
| # | Verdict | Severity | Kind |
|---|---|---|---|
| 1 | CONFIRMED | high | defect vs README (report gap acknowledged) |
| 2 | CONFIRMED | medium | defect (report silent) |
| 3 | CONFIRMED | medium | defect (no validation) |
| 4 | PARTLY | low to medium | design limitation, partly documented |
| 5 | CONFIRMED (size corrected) | medium | defect |
| 6 | PARTLY | low | design limitation |
| 7 | CONFIRMED | medium | defect, undocumented |
| 8 | CONFIRMED (numbers wrong) | medium/low | defect |
| 9 | PARTLY | low | design limitation |
| 10 | PARTLY | medium | documented design, README mismatch |
| 11 | CONFIRMED | low | defect |
| 12a | CONFIRMED | low | defect |
| 12b | PARTLY | low | inconsistency, refusal side is right |
| 12c | CONFIRMED | low | defect (wording) |
| 12d | CONFIRMED | low | test/doc mismatch |

No outside reviewer has looked at this project yet; one is still needed.
