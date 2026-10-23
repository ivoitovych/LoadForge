# Journal fragment r3 — fixes to the worst-case exploration, from the review of 2026-10-22

**Written:** 2026-10-23 03:45 UTC
**Change:** `src/explore` (search, drift), `src/main` (`explore_command`, `trial_runner`); `tests/unit/explore`, `tests/unit/main`. Work item R3 of `reviews/reaction-plan-2026-10-23-0030-UTC.md`. Findings and evidence: `reviews/internal-review-verdicts-2026-10-23-0030-UTC-b.md` (the finding numbers below are its numbers). Contract headers and `tests/contracts/` are untouched; no contract question was needed.

Not fixed here, on purpose: finding 4 (the window starts at the last fork; it needs the controller to record a start instant, and R2 is editing the controller) and finding 9 (a zero-width interval from tied samples; a design limitation, the miscalibration measured 6.1% against 5%).

## Decisions

### Finding 1. Corrected hardware errors: scored, flagged per trial, exit 5

Options: (a) score the trial and flag it; (b) treat it as a finding like `kFindings` (drop the trial, the candidate leaves the search). Chosen: **(a)**. A corrected error means the hardware reported something and fixed it; the computation was verified right, so the objective's reading is a true measurement of that load. Dropping it (b) would remove exactly the candidate that stresses the hardware most, which is what explore searches for, and would leave the search with no number where `stress` prints a verdict. What `stress` does is the exit code, not the score: `outcome.cpp` maps `kCorrectedErrors` to 5, and README line 225 says explore uses 5 "for the same things". So each scored trial (candidate or reference) carries a `TrialNote`, the report prints one headline line and the replicate (or reference reading) numbers concerned, and `exit_code_for(report)` is 5.

### Finding 2. A trial that did not settle: marked, counted, exit code unchanged

Options: (a) mark in the report only; (b) also exit 6 or a warning exit. Chosen: **(a)**. JOURNAL 2.54 decided that a settle timeout is not an error (a machine that never flattens still has a worst case, and an exit 6 would make `--settle` unusable on a noisy one). The defect was the silence. Each scored trial carries the mark, the search counts trials that asked for a settle phase and those that did not meet it (unscored trials included), and the report says "N of M trial(s) did not settle ..." and, when it is every one, that no trial started from a controlled state. The owner may still choose a non-zero exit; it is one line in `exit_code_for`.

### Finding 3. A settle window longer than the settle maximum: refused in three places

The slope is judged only once a full window has been read, and the phase ends at its maximum, so such a phase cannot settle. Options for `--settle-max 20s` alone (the default window is 30 s): (a) refuse; (b) clamp the window to the maximum. Chosen: **(a)**, because (b) changes what the user asked for without saying so. Where: the command line refuses it in `settle_of`, **exit 2**, because it is a combination of options like "`--settle-slope` needs `--settle`" which already is exit 2, and the message names both values; `Search::configure` refuses it, **exit 1** (`kConfiguration`), for a library user; the trial runner refuses the candidate (`kCandidate`) as a backstop. Equal is allowed. One unit test (`TheWaitIsAQuarterOfTheWindow...`) had a case with a window above the maximum to reach the "never past the end of the phase" arm; it now reaches it with window equal to maximum and a later wait.

### Finding 5. `delta`: the advance from the last reading before the window

Options: (a) include the last reading before the window as the baseline; (b) scale the window's delta to the whole window by its rate. Chosen: **(a)**: (b) assumes a constant rate, and for an error counter that is exactly what is not true. The summarizer is the frozen statistics contract, so the trial runner summarises once more over a window that opens at that earlier reading and takes the counters' `delta` from it; gauges and the rate are those of the load window. Measured with the shipped binary, `frequency.busy_ticks`, `--duration 3s --sample-interval 1s`: before, delta 801.9 and 788.3 against a rate of about 400/s (two of three seconds); after, 921 and 926 against a rate of about 298/s (delta/3 is 3% above the rate). The 3% is the gap between the reading before the first fork and the window start plus the lateness of the deadline tick: delta covers the load window **and up to one sample interval before it**, the part that is idle or starting. Where `docs` says what delta is, it must say so.

### Finding 6. "Separated": the ranking is kept, its words are exact

The task said wording only. The ranking logic is unchanged (the verdict showed a one-reading rival can only block a "separated", never create one). `describe(kSeparated)` now reads "separated from the rival with the worst corrected mean"; the report prints the rival's id after it and a line "not compared by an interval (eliminated with fewer than two replicates): ..." listing the eliminated candidates that had no interval. `SearchReport` gained `rival` and `uncompared`. Option not taken: make `kIndistinguishable` whenever any eliminated candidate has one replicate. At `--replicates 1` that would make "separated" unreachable in the default configuration (the first round eliminates on one replicate, always), which would make the word useless rather than exact.

### Finding 7. The winner's claim comes from fresh replicates

Options: (a) `--final-replicates` fresh trials of the survivor after it is chosen, mean and interval from those alone; (b) keep the pooled samples and label the interval as conditional on selection. Chosen: **(a)**, as the owner preferred. Measured (eight identical candidates, one replicate in the first round, three at the end, 20000 seeded searches in the test, 40000 in the review): the pooled claim had a bias of +0.61 sd and 87.3% coverage; fresh replicates give a bias near 0 and 95%. The cost: a search now runs `--final-replicates` more trials after its rounds, always. Before, the survivor was topped up *to* that count, and nothing was added when halving had already reached it (for example 1, 2, 4 replicates with `--final-replicates 3`). The README's example search (two candidates, `--replicates 1 --final-replicates 2`) now runs one more trial than before (the survivor had one replicate and was topped up to two; it now gets two fresh ones). A single candidate is unchanged (nothing chose it; all its replicates are fresh). The eliminated candidates' estimates are still from the samples that eliminated them, which makes their means biased *low* in badness, so a "separated" against them is slightly easier than the interval says; not fixed, noted. The winner's earlier replicates are listed in the report, marked by a bar, and are not in its estimates.

### Finding 8. Reference degrees of freedom: `max(1, (n - 2) / 2)`

Options: (a) `n - 2` floored at 1 (the review); (b) the measured relation; (c) a closed form below it. The verdict measured effective degrees of freedom 1.0, 1.4, 1.9, 2.3, 3.4 for 3, 4, 5, 6, 8 readings, so `n - 2` (1, 2, 3, 4, 6) is also too high from 4 readings on. The derivation (in `drift.cpp`): for equally spaced readings the interior residuals have correlation -2/3 with a neighbour and 1/6 two places away, which gives `m^2 / (m + 8(m - 1)/9 + (m - 2)/18)` for `m = n - 2` residuals, about 0.51 m. Chosen: **(c) `max(1, (n - 2) / 2)`**, integer division, which is below the measured value for every n (1, 1, 1, 2, 2, 3 for 3 to 8). A test measures the effective degrees of freedom by simulation as a ceiling, and the coverage with only the reference noisy (≥ 94%; it was 86% at three readings). **Cost, to be decided by the owner:** the interval still takes its t at the *smaller* of the replicates' degrees and this one, so with the few reference readings of a short search (three to five) the t is 12.7 (one degree) rather than 4.3, even when the reference is perfectly steady. That is conservative and was already so for two readings. The proper combination of the two variances is Welch-Satterthwaite (the t at the degrees of freedom of the sum, so a steady reference costs nothing); it changes `Correction` (`estimate.hpp`) and its tests, and is the follow-up if the width matters. The README's sample intervals and the T11 tables in fragment 33 were measured before this fix and will be wider when re-measured.

### Finding 10. A reference trial that finds something: exit 5 or 6, with the report

Options: (a) keep the error (exit 3), print the reference's finding; (b) end the search with a report of what was measured. Chosen: **(b)**: a candidate's finding is 5 with a report, the README says the same codes apply, and a marginal machine whose reference faults after three hours should not lose three hours of results. The search ends at the reference trial; `SearchReport::reference_stop` holds its verdict; the candidates still in the search are `Fate::kStopped` ("stopped with the search"), never ranked; there is no worst case; every trial measured so far is listed. Findings exit 5; an unfinished reference exits 6, as an unfinished candidate does. A reference with no reading of the objective is still an error, exit 6 (`kNoReading`). `ExploreFault::kReference` is now only the internal signal and is never returned from `explore()`; it is kept in the enum and still maps to 3.

### Finding 11. A duration that cannot hold two readings: refused at parse, exit 2

For rate, delta, stddev and slope, `--duration` under twice `--sample-interval` is refused, naming both, **exit 2** (a combination of options, as for the settle window). Twice the interval always holds two rounds; the deadline's own round adds one more. `Search::configure` cannot check it: the sample interval is a runner option, not in `SearchConfig`. A statistic of one reading (mean, max, min, last) needs no more than one.

### Finding 12a. `--elimination`

`keep` is `n / factor` plus one when there is a remainder, which cannot wrap, and the command line refuses a factor above 1024 (exit 2). A factor above the number of candidates keeps one candidate, so nothing real is lost. `--elimination 1` is still refused by `Search::configure` (exit 1), as before.

### Finding 12b. Settle scope

The runner and the search both take **the first series of the settle metric's name** in the capability report (the search already did). The runner now refuses it with the source's reason and remedy if it cannot be read, instead of running a phase on a later scope that its slope code then skipped. Option not taken: settle on the first *readable* scope in both; on a two-package machine that would settle on package 1's sensor when the objective is package 0's.

### Finding 12c, 12d

The report line is "worst case: none; no candidate survived the search (each candidate's fate follows)". `baseline_at`'s doc no longer says an out-of-order reading is passed over; the points must be in time order, nothing reorders them, and the test says what the code does.

## Sentences R4 must change

README.md:

- Line 225, "`explore` uses 0, 1, 2, 3, 5 and 6 for the same things, per candidate; a trial the ceiling ends is 3." Add: 5 also covers a scored trial the hardware reported a corrected error in (the trial is scored and flagged, and the exit is 5 as for `stress`), and a reference trial that finds something (the search ends with a report of what was measured, no worst case); 6 covers an unfinished reference trial as it does a candidate's. The words "per candidate" then miss the reference.
- The explore example (lines 163-165) is still valid (`rate` with 4 s and the default 1 s interval). Where the README lists explore's options, if it does: `--elimination` is 1 to 1024; the settle window may not exceed `--settle-max`; for `rate`, `delta`, `stddev` and `slope` the duration must be at least twice `--sample-interval`.
- Anywhere the README says what the report holds: it now also says which trials did not settle, which had corrected hardware errors, and which rival "separated" was against.

docs/journal/33-explore-integration.md and docs/JOURNAL.md:

- JOURNAL 2.56 and fragment 33 "Exit codes keep `stress`'s vocabulary": "3 a trial could not be run or the reference was unusable; 5 a candidate's trial found a fault" becomes: 3 a trial could not be run; 5 a finding in a candidate's or the reference's trial, or a corrected hardware error; 6 a candidate's or the reference's trial that did not finish.
- Fragment 33 "Contract questions", "Stream 28's question about corrected errors ... the report does not yet flag it": it is flagged now, per trial, and exits 5.
- JOURNAL 2.54 / fragment 33 on the settle phase: "Unsettled reports `settled = false` ... not an error" stays true; add that the search now says which trials were unsettled. "`ASettlePhaseIsMeasuredAndBounded` uses a 1 ms window" is unchanged (window 1 ms, max 50 ms).
- Fragment 33 "The load window": add that a counter's `delta` covers the load window plus up to one sample interval before it (finding 5), and that the window itself still starts at the last launch (finding 4, open).
- `docs/journal/28-explore.md` lines 66-69 ("the survivor is topped up to `final_replicates`"): the survivor is run `final_replicates` more times after it is chosen, and its estimate is from those alone. Line 75: "the reference's own degrees of freedom" is `max(1, (n - 2) / 2)`, not `n - 1`. The "Ranking" bullet: kSeparated is against the eliminated rival with the worst mean, named in the report, and the candidates with one replicate are listed as not compared.
- JOURNAL line 1200 ("the best rival in a halving round has one or two replicates") stands.
- `docs/hardware-validation.md` 6.1.1, "A trial that started warm is reported as such", and 6.1.3, "whether the settle phase settled (`settled`/`not settled` per trial in the report)": the report now has the line "settle: N of M trial(s) did not settle ..." and the per-candidate list "replicate(s) that did not settle"; the words `settled`/`not settled` appear only in the `--verbose` log. 6.1.3 "the ranking against the best rival" becomes "against the rival named in the ranking line". "Plan hours": a search is `--final-replicates` trials longer than before.
- `docs/architecture.md` Explore paragraph: "every claim carries its replicate count, standard deviation and 95% interval" add: the winner's claim is from replicates run after it was chosen. `docs/architecture.md` section 7 repeats the explore exit-code sentence.
- `docs/ROADMAP.md`: R3 done; findings 4 and 9 remain open.
