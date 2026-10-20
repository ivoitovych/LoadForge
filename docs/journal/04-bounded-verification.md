# Journal fragment 04 — bounded verification

**Written:** 2026-10-20 11:33 UTC
**Change:** stream 04 (`docs/streams/04-bounded-verification.md`): derived tolerances
(`src/verification/residual`), invariant and conservation checks
(`src/verification/invariant`), and the independent oracle for the dense-linear workload
(`src/verification/oracle/dense_linear`).

## Established by experiment

### The derived tolerance against the rounding real evaluations commit

The tolerance (residual.hpp, steps 1 to 5) was measured three ways.

- **Against bound (3) of its own derivation**, in `long double` (64- and 113-bit
  significands on the two architectures), over 10 error models and 2,008 magnitudes: never
  below it, and at most **17.9 u** above it for magnitudes where the product K S^ is
  normal. The header admits 24 u. Where K S^ falls among the subnormals, the excess is one
  subnormal step, which the test allows explicitly.
- **Against exact arithmetic.** Every dense-linear entry is a multiple of 2^-52, so a dot
  product of 256 of them is exact in a 128-bit integer. For 43 evaluation orders (forward,
  reverse, pairwise, and every panel depth of the specification with 1, 2, 4 and 8 lanes,
  plain and fused) the worst error seen against Higham's gamma_n S was **0.0049** on the
  specification's inputs, **0.019** on deliberately cancelling sums, and **0.55** on short
  vectors (n from 1 to 33, where one rounding is a large share of a small bound). The
  worst residual against the derived tolerance was **0.0030**, **0.012** and **0.23**
  respectively. Sums of up to 2,048 values (the invariant module) reached **0.021** of
  gamma_(n-1) S and **0.0098** of the tolerance.
- **Against corruption.** A single-bit flip of a correct result is caught for every sign
  and exponent bit; of the significand, the low bits below the bound are not, and how many
  depends on |c| against S: up to **16 of 52** on the specification's inputs, where a
  result that cancels to a small |c| has a small ulp. Of 1,184 single-bit flips of sampled
  output entries (all 64 bits of eight entries, sign and exponent of 56 more), **1,068 were
  caught and 116 fell within the bound**, every one of the 116 a low significand bit.

### The round-up steps are a proof margin, and a test cannot tell 8 from 2

Removing the eight `nextafter` steps (K = G as computed) makes the bound test fail (first at
`depth 2, S^ = 1`: T^ one ulp below B). With two steps it passes
every case tried. The proof needs seven ((1 + u)^s (1 - u)^6 >= 1 + u), because it must
hold for the worst rounding of every operation at once, which no sample of inputs reaches.
A surviving "8 to 2" mutant is therefore expected, and the argument in the header, not a
test, is what discharges it.

### gcov counters race under the oracle contract's concurrency test

`OracleContract.ChecksRunConcurrently` runs `check()` on one thread per case. Under the
coverage preset the input scan's loop counters were incremented from up to fourteen
threads at once; gcov's counters are plain increments, increments were lost, and gcovr read
`branch 1 taken -5761` at `dense_linear.cpp:102` and refused the report (it fails closed on
negative hits, as tools/coverage.sh intends). `-fprofile-update=atomic` on the three
targets makes the counters exact; the gate then measured 100%. The option is added in this
stream's own build fragments because `cmake/Coverage.cmake` is not this stream's — see
"Contract questions".

### Two allocating members in an aggregate return, met again

`return NotChecked{std::string(kWhat), "the input is " + std::to_string(n) + ..., 0};` left
one uncovered branch per such line (journal §1.13's mechanism): the second member's
temporaries are built while the first member already exists. Building the text in a
statement of its own and moving it in removed both; the input-range return, already
written that way, had been covered from the start.

### clang-tidy's De Morgan rewrite would let a NaN through

`readability-simplify-boolean-expr` flagged `!(value >= -1.0 && value < 1.0)` and proposes
`value < -1.0 || value >= 1.0`, which is false for a NaN — the one value the expression is
written to refuse. The condition is now a named `in_range` and negated; the rewrite is not
taken.

### Three toolchain facts that cost a build each

- A `requires` expression outside a template is not a SFINAE context: `static_assert(!requires(T r) { r.difference; })`
  is a hard error when the member is missing. A named concept makes it a predicate; each
  concept is also asserted true of the type that has the member, so an unsatisfiable
  concept cannot pass for an absence.
- `INSTANTIATE_TYPED_TEST_SUITE_P` must be written in the suite's own namespace
  (`loadforge::contracts`); the macro pastes the name and cannot take a qualified one, and
  the ledger gate's pattern expects the bare name too.
- Including a contract-suite header registers every suite in it. A test binary that
  instantiates `OracleContract` but not `OracleRegistryContract` fails GoogleTest's
  uninstantiated-suite check; `GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST` marks the
  second as deliberately not run here, and the shared test helper defines its own byte-case
  type so that binaries not running the contract do not include the suite header at all.

## Decisions

### The tolerance: two evaluations, each rounding accounted for

Options: (a) Higham's gamma_n S as the spec states it, compared directly; (b) a multiple of
it chosen to "leave room"; (c) the bound on |kernel - oracle| when both are evaluations of
the same sum, with the oracle's magnitude sum, the residual's own rounding and the
tolerance's own evaluation each covered. (a) false-alarms: the oracle's value is not
exact, so the difference can reach twice gamma_n S. (b) is a chosen constant, which
verification.md §5 forbids. **(c) is taken**: T^ = fl(fl(K S^) + V), K = 2 gamma / (1 -
gamma) raised by eight representable steps, V = (4m + 1) 2^-1074, derived in full in
residual.hpp. The result is a sound bound (no correct evaluation exceeds it, proved and
measured) within 24 u of the sharpest one for two evaluations.

### Underflow is in the bound, though correct inputs never underflow

Correctly generated entries are multiples of 2^-52, whose products cannot underflow. But
the oracle accepts any entry in the generator's range, and a subnormal is in range; a
tolerance derived without the underflow term would be wrong for such an input. The term
U = m 2^-1074 costs nothing measurable (below 2^-1040) and makes the derivation hold for
every input the range check admits. It also gives every tolerance a strictly positive
floor, which NumericalVerificationFailure requires.

### The input range is the generator's, and the whole input is scanned

`[-1, 1)`, from `entry_value`, rather than the spec's "magnitude below 1" (see "Contract
questions"). The whole input is scanned before anything is compared, not only the rows and
columns the sample uses: the verdict on a corrupt input then does not depend on which
entries the unit index happened to sample, and the scan of 131,072 doubles is cheap beside
the dot products.

### The sample: one entry per tile, 8 x 8 tiles by default

Options: a fixed set of entries (the same for every unit: a fault elsewhere is never seen);
uniform random entries (clusters and gaps); whole rows or columns (one tile row only); one
entry per tile of a grid, drawn per unit. **One per tile** is taken: a fault confined to one
region of C — a microkernel's register block, a cache line's rows — meets a check whatever
the region, and the drawn offset varies with the unit, so over units every entry is
checked. Drawn from Philox at a fixed seed with the unit index as the stream, so the
sample is a function of the index alone (oracle.hpp). The grid is a parameter of the
factory, validated (a power of two up to 256), so the tests can check every entry; the
default of 8 is 64 dot products, about 0.2% of a unit's arithmetic before the duty cycle.

### Every sampled entry is compared, and the worst failure is named

Options: stop at the first failure (cheaper, names whichever came first); compare all and
name the worst. **All, worst named**: blocks_compared then always means the whole sample,
and the record shows the most severe entry — a NaN before any number, then the greatest
ratio, the first in tile order on a tie. Two ratios that overflow to infinity are a tie,
as the records' `ratio()` would print them both as infinity.

### The condition number is S^ / |c'|

The dot product's relative condition number, reported where c' is nonzero and absent where
it is zero (absence reported as absence, not as infinity or zero). It tells a reader how
much cancellation the entry had, which is what makes a residual large relative to |c|
without being large relative to S.

### Loads are `memcpy` on a little-endian build

The first version assembled each double byte by byte. On the platforms LoadForge builds
for, both little-endian, a copy of eight bytes is the value; the byte loop was the oracle's
largest cost at -O0. `static_assert(std::endian::native == std::endian::little)` makes a
big-endian port fail to compile until it adds a swap, rather than read wrong values.

### Invariant checks: what they are, and the one place a bound is twice as wide as needed

Sum, squared norm, conservation (bounded), first descent and a multiset fingerprint
(exact). The conservation check evaluates sum(after) - sum(before) as one tree whose exact
value is zero; residual's tolerance is derived for two evaluations and so is twice what one
needs. Kept, rather than a second derivation: one derivation to trust, at the cost of one
bit of sensitivity, which the fault-injection tests measure. The fingerprint is the count
and two 64-bit sums of Philox4x32-10 under a fixed key: order-independent without sorting
(verification.md §3: never a second sort), and every single-bit flip of 256 keys moved both
sums in the test. Keys are `std::uint64_t` only; a later workload with other keys widens
them or asks for an overload.

### Tiers and classes, against the brief's table

The three modules have P1, P3 and P4 as the table says. No P5: nothing here holds mutable
state, and the concurrency OracleContract exercises is concurrency of pure functions.
`src/verification/invariant` gains **T5b**, because its not-derivable arm is a verification
outcome and owes its unrun-check test like the others.

### Exact arithmetic in tests: `__int128` and `long double`

The truth the bound is checked against is computed in 128-bit integers (`__extension__`,
GCC and Clang on both architectures), and bound (3) in `long double`. determinism.md
prohibits `long double` on architecture-neutral bit-exact paths; a test's check of a bound
is not one, and both formats are wider than double, which is all the check needs.

### No pull request from this stream's session

The brief's last section says to open one; the instructions the stream ran under say the
manager opens and merges pull requests. The second is followed; everything the pull request
would carry is on the branch and in this fragment.

## Contract questions

1. **The input range** (`workloads/dense_linear/spec.hpp`). The header says "Every
   correctly generated entry is finite with magnitude below 1", but
   `entry_value(0) == -1.0` exactly (and `tests/contracts/workloads/` asserts it). Read as:
   the range is `[-1, 1)`, the generator's. The derivation needs only |entry| <= 1, so
   nothing else changes. Suggested correction: "in [-1, 1)" in place of "magnitude below
   1".
2. **The workload's name.** The spec does not name the workload; the oracle needs it for
   `workload()`. Read as `dense_linear` (the directory, the namespace, and stream 11's
   `--test dense_linear`). Suggested correction: a `kName` in spec.hpp beside `kVersion`.
3. **Which input entries make a unit NotChecked.** "A non-finite or out-of-range entry in
   the INPUT" could mean any entry or only those the sampled checks read. Read as any
   entry (see "Decisions").
4. **Which failure a BoundedMismatch names when several entries fail, and what
   blocks_compared counts then.** oracle.hpp and spec.hpp are silent. Read as the worst,
   with blocks_compared the whole sample (see "Decisions"). A contract sentence would let
   the workload stream's T5 tests and this oracle's agree on it.
5. **Underflow.** The spec's derivation is Higham's without underflow, which holds for
   correctly generated inputs; inputs the range check admits can underflow. The oracle
   includes the underflow term (see "Decisions"). Suggested addition to the spec: "with
   gradual underflow, an absolute term of n 2^-1074 per evaluation".
6. **Suites sharing a header.** `oracle_contract.hpp` holds both `OracleContract` and
   `OracleRegistryContract`, so every provider of one meets GoogleTest's
   uninstantiated-suite failure for the other and must silence it. Suggested: one suite
   per header.
7. **Coverage under concurrency** (not a contract, a shared file). `cmake/Coverage.cmake`
   compiles with plain gcov counters; any test that runs instrumented code on several
   threads can lose counts and produce negative branch counts, which the gate refuses.
   This stream sets `-fprofile-update=atomic` on its own targets. Suggested: add it in
   `loadforge_enable_coverage` for every target, so the next stream with a concurrency
   test does not meet this again.
