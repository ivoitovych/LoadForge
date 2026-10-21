# Review request

- **Date and time:** 2026-10-20 18:41 UTC.
- **Series:** review requests are dated and kept, one file each, in `reviews/`; the newest is
  the current one. The introduction in
  [review-request-2026-10-20-0352-UTC.md](review-request-2026-10-20-0352-UTC.md) (what LoadForge
  is, how to build it, how to report) still applies.

## Why this request

Two independent reviews today found eight real defects, every one of which had passed the
project's automated gates. Three were major: a kill taken as proof of a death's cause, a
verification record that could describe a passing check as a failure, and a CI identity gate
resting on forgeable fields. All eight are accepted; most are fixed
([reaction plan 1](reaction-plan-external-review-2026-10-20-1230-UTC.md),
[reaction plan 2](reaction-plan-wave1-review-2026-10-20-1547-UTC.md)).

Since then, wave 2 has been written against frozen contracts, with each component's tests and
mocks its own (ADR 0003), and most of it has merged. **None of it has had an outside reader.** Nor
have the frozen specifications that waves 3 and 4 are being built from right now: a
specification wrong today becomes a workload and an oracle that agree on the wrong answer.

## What to review, most valuable first

### 1. The specifications: can a workload and its oracle agree on a wrong answer?

Each `src/workloads/<name>/spec.hpp` is the single document a workload and its independent
oracle are both written from. Questions for each: is every range, rounding and tie-break stated
exactly? Is the tolerance derived correctly, and tight enough to catch a real fault? Could two
correct readings of the text disagree?

- `sort/spec.hpp`, `monte_carlo/spec.hpp` (on main, PR #50).
- FFT, stencil, sparse, graph, particles (PR #65); ray tracing, adaptive mesh (PR #66; its
  journal lists three open questions, including the ray-tracing determinism class).

### 2. The oracles built from them

- `src/verification/oracle/sort/` and `monte_carlo/` (on main, PR #62): can a wrong sort or a
  wrong sum be accepted? The sort oracle proves the result without a second sort (permutation,
  then key equality, then order); is that sound?

### 3. Telemetry, results and the journal

- Telemetry sources (`src/telemetry/`, PRs #51 on main, #57): thermal, fan, frequency, memory,
  power, performance counters, EDAC. Does any source report a wrong reading as valid, or misname
  a sensor? Counter wrap-around handling.
- The timeline and sampler (PR #58, on main): ordering and loss under load.
- Results and the JSON and CSV reporters (PR #56): does a report ever claim more than was
  checked? A sampled verification must read "N sampled entries agreed", never "correct"
  (reaction plan 2, W1-02).
- The crash journal (PR #59): does it survive a crash at any point of a write?
- Statistics, the Load Signature (`src/statistics/`, on main, PR #52).

### 4. Workloads and primitives

- Sort and Monte Carlo workloads (on main, PRs #60, #61); calibration primitives (PR #63);
  explore (`src/explore/`, on main, PR #64).

## Already known, so you need not report them

The eight findings of the two earlier reviews; each PR's "Contract questions" in its journal
fragment (`docs/journal/NN-*.md`); the wave-1 contract corrections in progress (one suite per
contract header, specification wording).

## How to report

As before: a GitHub issue, inline comments on a pull request, or a file
`reviews/review-response-2026-10-20-1841-UTC.md` on a branch. For each finding: the file and
line, what goes wrong, and how to make it happen. Every finding is answered; each that holds is
fixed with a test that fails without the fix.
