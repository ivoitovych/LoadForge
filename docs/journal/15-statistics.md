# Journal fragment 15 — statistics

**Written:** 2026-10-20 12:56 UTC
**Change:** the Summarizer (src/statistics), its tests (T1, T4) and ledger fragment.

## Established by experiment

### A variant built on the first line of a function reads as half-covered

`MetricSummary summary{metric, nothing_in_window(), 0};` left two of four branches
uncovered under gcovr's exclusions: the branches are the variant's converting
constructor, not a decision of the code. The default state of the `value` member is now
assigned only in the arm that means it ("no reading at all"), and the line carries no
branch. Coverage went from 99.9% to 100.0% branch with no other change.

### Fixture

`tests/fixtures/timeline/statistics/proc-sample.csv` is 12 real samples of this
kernel's /proc/stat context-switch counter and /proc/meminfo MemAvailable (command and
machine in its header). Expected figures were computed in exact rational arithmetic with
python3 `fractions`, not by the code under test; the test compares within 1e-8..1e-9.

## Decisions

- **Lookup by binary search.** A series' points are in timestamp order (the timeline
  refuses otherwise), so the window is found with two `lower_bound` calls and only its
  points are read.
- **Series kind comes from the metric.** A Gauge reading in a counter series, or a Counter
  in a gauge series, is ignored rather than counted: the timeline refuses such a point
  (kKindMismatch), so it only arrives from a hand-built view.
- **Slope with no variance of time** (two readings at one instant, only possible from a
  view that breaks the ordering rule) is absent, not NaN.
- **Formulas are the reference's:** compensated sums in timestamp order for the mean,
  squared deviations from the mean, the mean time, and the covariance and variance of
  the slope; stddev is sqrt(sum/(n-1)).
- **The "nothing in the window" remedy** reads "sample for longer, or more often, so that
  the window holds a reading".
- **Ledger.** P1 and P3 as the brief listed; no P7 (no capability state, absence is a
  value the readings already carry) and no P2 (no fallible call besides the window check,
  covered by T1). T4 added as owed.
- Nothing here meets ADR 0001's test, so no proposed ADR.

## Contract questions

- The contract header registers one suite (SummarizerContract); the T4 binary includes it
  without instantiating it, so it carries `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST`.
- signature.hpp says rate is "absent when no such pair exists" and defines it over
  span_ns; I read "a pair exists" as span_ns > 0, which holds for any strictly increasing
  timeline.
- The contract fixes which sums are compensated but not the form of the stddev and slope
  (two-pass about the mean here); the suite's values are exact either way.
