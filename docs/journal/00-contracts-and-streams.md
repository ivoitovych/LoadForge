# Journal fragment 00 — contracts and streams

**Written:** 2026-10-20 05:17 UTC
**Change:** the contracts, their suites, the fragment mechanism and the stream plan
(ADR 0004).

## Established by experiment

### An unqualified call named `fill` finds `std::fill`

The generator's byte-filling function was first called `fill(seed, stream, out)`. A test
calling it with a `std::vector<std::byte>` failed to compile inside `stl_algobase.h`:
argument-dependent lookup found `std::fill(first, last, value)` through the vector's
namespace, and as a template taking the arguments by their exact types it beat the
project's function, which needed a conversion to `std::span`. Renamed `fill_stream`. Any
contract function taking a span is exposed to the same lookup when a caller passes a
standard container; a name no standard algorithm uses is the defence.

### `= default` comparisons, and comparisons that can throw, in frozen headers

Measured on the first three contract headers under the coverage preset. A defaulted
`operator==` used by two test translation units left member lines at zero (§1.22's
mechanism, now in a header no stream may edit). Replacing it with an explicit `std::tie`
comparison moved the problem rather than removing it: a comparison that is not `noexcept`
calls functions the compiler must assume can throw, and the cleanup block for that
exception was attributed to the first line of the `return` expression, reported
`#####` while the function itself was called 78 times. Declaring the comparison
`noexcept` removed the block, and with it the uncovered lines and branches (§1.13's rule,
met from a different direction). Every contract value type now compares this way.

### Contract suites count towards `src/core/result.hpp`

The suites are instrumented, so every `core::Result<T, E>` they instantiate is measured
as `src/core/result.hpp` (§1.23). A suite that unwrapped results with `value_or()` on the
success path only, and a reference whose error return no test reached, left two lines and
two branches of `result.hpp` uncovered. Streams meet the same rule with their own
instantiations; their briefs say so.

### `std::variant`'s `operator==` and clang-tidy

clang-tidy's `bugprone-exception-escape` refuses a `noexcept` function that compares two
variants with `==`, since the analyser cannot see that the comparison does not throw. The
comparison is written out: same index, then the held alternatives compared through
`std::get_if` (`telemetry::same_reading`, `worker::ControlMessage`).

### Two GoogleTest details that cost a build each

`INSTANTIATE_TYPED_TEST_SUITE_P(Prefix, Suite, ::testing::Types<A, B>)` is a macro call
with a comma inside a template argument list; it has to be given a type alias. And a test
helper template whose second parameter is a list of lambdas cannot deduce `T` from both
arguments; `std::type_identity_t<T>` makes the second a non-deduced context.

## Decisions

All recorded in ADR 0004: the units model of a workload, checks in the computing slice,
the oracle judging the given input, the two-stage record, an exact corruption without bits,
the caller-driven sampler, the framed control pipe, the three contracts that carry their
definitions, the glob over a list, one ledger fragment per module, and the thirty-four
streams in six waves.

## Contract questions

None yet; this is where streams record theirs.
