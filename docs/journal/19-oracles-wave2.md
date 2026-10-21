# Stream 19 — oracles for sort and Monte Carlo

Written 2026-10-20 15:57 UTC.

## Established by experiment

- **The replay is within an ulp of an exact sum.** For a Monte Carlo batch the
  oracle's compensated sum equals Python 3.13.16 `math.fsum` over the same
  16384 values bit for bit for all six keys in
  `tests/fixtures/oracle/monte_carlo/exact_sums.txt` (command and machine in
  its header, x86-64, GCC 13.3). A second test sums the values in 128-bit
  integers (every value is a multiple of 2^-79) and bounds the distance to the
  oracle's double by 2^-52 relative.
- **What the bound lets through.** Sums are near 12867, so one ulp is 2^-39
  and the tolerance about 2^-24.35: flipping mantissa bit k of a sampled sum
  moves it by 2^(k-39), caught from k = 15 up and passed for k <= 14. Tested
  bit by bit. This is the specification's choice (a rounding bound, not a
  statistical one), recorded here so no one expects a low-bit flip in a sum to
  be seen.
- **Chunked orders sit inside the bound.** Sums by every chunk length (64 ..
  16384), built in the test the way the specification's decomposition says, are
  within `tolerance(r)` of the replay, and within 2^-39 relative of the exact
  integer sum.
- **gcov and a std::string temporary.** Returning `NotChecked{"...", "...", 0}`
  directly from a function that can throw left an uncoverable branch in the
  cleanup of the two strings (JOURNAL §1.13 again). Building the verdict inside
  a small `noexcept` helper removes it. Likewise `new (std::nothrow) T[n]`
  emits a null-check branch no test can take; scratch comes from `malloc` in a
  `unique_ptr` with a deleter instead.
- **-Wdangling-reference caught a real bug in a test helper** that returned a
  reference into a temporary Verdict; the helper now returns by value.

## Decisions

- **Fixtures.** The right output of a sort unit is built in the tests with
  `std::sort` over (key, position) pairs, the standard library's sort, not the
  stress implementation; keys come from the specification's generator or are
  chosen at the ends of the order. T3 compares the oracle with a naive second
  reading of the specification that builds the whole expected output.
  Option rejected: committing 1.5 MiB sort units as fixtures; they would be
  produced by the same call.
- **Monte Carlo summation.** Neumaier compensated summation, not a long double
  accumulator (differs by architecture) and not a pairwise tree (error grows
  with log n). Its error is far inside the allowance, and it is the same bits
  on x86-64 and ARM64.
- **Scratch seam for sort.** `make_sort_oracle(config, plan, allocator)` takes
  the allocator of the 16 KiB seen-set, so T5b can make it fail and show
  NotChecked (blocks 0) for right and wrong output alike, as the specification
  requires. Monte Carlo has no scratch and no second NotChecked, as specified.
- **Creation.** Both oracles refuse (kConfiguration) a configured decomposition
  that is neither 0 nor in the specification's set, a plan decomposition not in
  the set (0 included), and plan sizes that are not the unit's. Public
  constructors are free functions in
  `src/verification/oracle/<workload>/<workload>_oracle.hpp`; the registry is
  the integration stream's.
- **Monte Carlo exposes `replay_batch_sum`** so a test can check the summation
  against an outside tool.
- **Ledger.** Classes P1, P3, P4 and tiers T1, T3, T5, T5b kept as the brief
  listed; no P2 (the one fallible call is the scratch allocation, covered as
  T5b) and no consumes.
- **Git identity.** The container's global config sets an SSH signing key and
  `gpg.format`; the rule's two `--unset-all` commands were refused by the
  sandbox. `commit.gpgsign=false` is set locally, so commits are unsigned;
  verified with `git log --show-signature`.

## Contract questions

- **Config and plan decomposition disagree.** The specification refuses a bad
  configured decomposition and a bad plan decomposition separately, but does
  not say whether a valid configured value that differs from the plan's
  resolved one is refused. Read as: not refused; the plan is what the units
  were made under and the oracle does not use the decomposition to judge.
- **Sort step 2, window past the region.** The window is min(64, region -
  window_offset); read as stopping at the end of the keys' region, as the
  indices region is a different Domain span.
- **Sort NotChecked order.** Wrong sizes are checked before the scratch is
  obtained, so sizes win when both fail; the specification lists both as
  NotChecked, blocks 0, and does not order them.
