# Journal fragment 28 — explore

**Written:** 2026-10-20 16:04 UTC
**Change:** `src/explore` — objectives, successive halving, drift correction, confidence (F5, M6).

## Established by experiment

### Correcting against a noisy reference makes the plain interval too narrow

The first version reported the interval from the replicates' own spread, with the
drift-corrected scores. A coverage property (600 seeded searches, 5 replicates, noise on
the candidate *and* on the reference) measured the 95% interval covering the true mean
81% of the time. The cause: every corrected score subtracts a baseline built from the same
few reference readings, so the error is shared between replicates and the sample spread
cannot see it. The interval now adds the propagated reference noise
(`correction_gain` x `reference_variance`, `src/explore/drift.hpp`) and the same test
measures 95-98%.

### A successive-difference estimate of the reference's noise is wrong under drift

First attempt: `sum (r[k+1]-r[k])^2 / 2(R-1)`. Under an injected drift of a few degrees per
trial it was dominated by the drift, the interval grew to tens of degrees and a clearly
separated winner (gap > 30) was reported as indistinguishable. Replaced by the residual of
each interior reading from the line through its neighbours in time, scaled by
`1 + w^2 + (1-w)^2`; a drift that is linear over three readings leaves no residual. With
two readings only the difference exists, so that case keeps the first-difference form and
errs high.

### An unreachable guard in the interpolation

`baseline_at` first had a `span <= 0` arm for two readings at one time. Reasoning from the
loop's invariant (the previous reading is strictly before `at`) shows the span is always
positive, so the arm could never run; it was removed rather than excluded.

### Two coverage traps from the journal, met again

- `return ExploreError{k, id + ": " + detail, fault}` left one branch no test can take
  (§1.13). Building the string and the error in separate statements removed it.
- `std::set<std::string> seen{config.reference.id}` (an initializer list) did the same;
  inserting after construction removed it.

## Decisions

- **Worst case is a type that cannot lack its evidence.** `WorstCase` is built only by
  `worst_case_of`, which answers nothing below two replicates; `stddev` and `half_width` are
  plain doubles. `final_replicates >= 2` is refused at configuration. Rejected: optional
  spread fields on the claim, which would let a caller print a bare number.
- **Objectives bind to the capability report before the first trial** (`bind`), by name and
  scope index; unknown, unavailable (with the source's own detail and remedy) and
  statistic-of-the-wrong-kind are three distinct faults. Settle metrics of every candidate and
  the reference are checked the same way at configuration. Statistic is a variant of a gauge
  and a counter enum, so a mismatch is a type-level question answered once.
- **Scoring a missing reading is an error**, never zero: a sensor that dropped out in a
  window ends the search (kNoReading). Rejected: dropping the candidate, which would let a
  flaky sensor silently remove the worst case.
- **A trial with findings (or incomplete) is not a score.** The candidate leaves the search
  with that fate and is listed. A reference with either ends the search, since drift can no
  longer be measured.
- **Drift correction is additive**, linear interpolation in time between the neighbouring
  reference readings (held at the ends), anchored on the mean of the first
  `reference_replicates` readings. Rejected: multiplicative (misbehaves near zero), a fitted
  line (biased for non-linear drift).
- **Replicates run pass by pass** (every survivor once per pass) so drift meets candidates
  equally; the reference runs before the first trial, every `interleave_every` candidate
  trials, and after each round, so every trial is bracketed.
- **Halving**: round r runs survivors to `base * factor^r` replicates, keeps
  `ceil(n / factor)` by corrected mean (ties to the earlier candidate), and the survivor is
  topped up to `final_replicates`.
- **Ranking** says whether the winner's interval is clear of the best eliminated rival's
  (kSeparated), overlaps or the rival has too few replicates for an interval
  (kIndistinguishable), or there is no rival (kSole). A constant objective therefore ranks
  nothing.
- **Time of a trial** is the midpoint of its load window on the timeline's clock.
- **Intervals use Student's t** from a 30-row table, stepped conservatively beyond; the reference
  noise term uses the smaller of n-1 and the reference's own degrees of freedom.
- **T8 is not owed here**; the search over the real controller belongs to stream 33. The ledger
  row says so. P2 is not owed: no fallible call is made, only runner results propagated.

## Contract questions

- `TrialRunner::run` says nothing about the order of `signature.window` across trials. Explore
  uses window midpoints as the time axis and tolerates equal times, but a runner whose windows
  are not non-decreasing across calls would make the interpolation meaningless. Reading
  chosen: the timeline is monotonic (as `telemetry/timeline.hpp` says); worth stating in
  `trial.hpp`.
- Whether a trial with verdict `kCorrectedErrors` is a score or a finding is unstated. Read as
  a score (the verdict says the hardware corrected it), reported in nothing but the candidate
  samples; a reporter may want to flag it.
- `Candidate.duty` equality across a search is enforced here (F17) although the contract only
  says it is "held constant"; no contract change needed, noted for the integration stream.
