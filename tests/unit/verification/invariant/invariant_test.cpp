// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 and T5b for src/verification/invariant: each check on small inputs whose
// answers are known, the empty and single-element cases, the tolerance each
// bounded check is judged by, and each refusal -- values from which no
// tolerance can be derived give the Underivable arm, never a measurement.

#include "verification/invariant/invariant.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/residual/residual.hpp"

namespace loadforge::verification::invariant {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

double tolerance_of(residual::ErrorModel model, double magnitude) {
  return residual::tolerance(model, magnitude).value();
}

TEST(InvariantSum, MeasuresTheClaimAgainstTheSumInIncreasingOrder) {
  const std::vector<double> values{0.5, -0.25, 2.0};
  const Bounded held = check_sum(2.25, values);
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(held.value().residual, 0.0);
  EXPECT_EQ(held.value().tolerance, tolerance_of(residual::sum_model(3), 2.75));
  EXPECT_TRUE(residual::within(held.value()));

  const Bounded breached = check_sum(2.5, values);
  ASSERT_TRUE(breached.has_value());
  EXPECT_EQ(breached.value().residual, 0.25);
  EXPECT_FALSE(residual::within(breached.value()));
}

TEST(InvariantSum, EmptyAndSingle) {
  // Nothing to sum: the claim must be zero, to within the floor alone.
  const Bounded empty = check_sum(0.0, {});
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty.value().tolerance, 0x1p-1074);
  EXPECT_TRUE(residual::within(empty.value()));
  EXPECT_FALSE(residual::within(check_sum(0x1p-1073, {}).value()));
  // One value: no addition happens, so the claim must be that value.
  const std::vector<double> one{0.1};
  EXPECT_TRUE(residual::within(check_sum(0.1, one).value()));
  EXPECT_FALSE(residual::within(check_sum(std::nextafter(0.1, 1.0), one).value()));
}

TEST(InvariantSum, NoToleranceFromValuesThatHaveNone) {
  const std::vector<double> nan{1.0, kNaN};
  const std::vector<double> inf{kInf, 1.0};
  const std::vector<double> huge{0x1p1001};
  EXPECT_EQ(check_sum(1.0, nan).error(), residual::Underivable::kNonFiniteMagnitude);
  EXPECT_EQ(check_sum(1.0, inf).error(), residual::Underivable::kNonFiniteMagnitude);
  EXPECT_EQ(check_sum(1.0, huge).error(), residual::Underivable::kMagnitudeTooLarge);
}

TEST(InvariantSum, ANaNClaimIsAMeasurementThatFails) {
  const std::vector<double> values{0.5, 0.25};
  const Bounded result = check_sum(kNaN, values);
  ASSERT_TRUE(result.has_value()) << "the values have a tolerance; the claim is what is wrong";
  EXPECT_FALSE(residual::within(result.value()));
}

TEST(InvariantSquaredNorm, MeasuresTheClaimAgainstTheDotOfTheValuesWithThemselves) {
  const std::vector<double> values{0.5, -0.25, 2.0};
  const Bounded held = check_squared_norm(0.25 + 0.0625 + 4.0, values);
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(held.value().residual, 0.0);
  EXPECT_EQ(held.value().tolerance, tolerance_of(residual::dot_model(3), 4.3125));
  EXPECT_FALSE(residual::within(check_squared_norm(4.0, values).value()));
  EXPECT_TRUE(residual::within(check_squared_norm(0.0, {}).value()));
  const std::vector<double> nan{kNaN};
  EXPECT_EQ(check_squared_norm(0.0, nan).error(), residual::Underivable::kNonFiniteMagnitude);
  const std::vector<double> overflowing{0x1p600};
  EXPECT_EQ(check_squared_norm(0.0, overflowing).error(),
            residual::Underivable::kNonFiniteMagnitude);
}

TEST(InvariantSquaredNorm, SquaresThatUnderflowAreCoveredByTheUnderflowTerm) {
  // (2^-600)^2 = 2^-1200 rounds to zero: the computed norm is 0 and the
  // exact one is not. Both claims are within the bound.
  const std::vector<double> tiny(16, 0x1p-600);
  EXPECT_TRUE(residual::within(check_squared_norm(0.0, tiny).value()));
  EXPECT_TRUE(residual::within(check_squared_norm(0x1p-1074, tiny).value()));
}

TEST(InvariantConserved, TotalsThatAgreeAndTotalsThatDoNot) {
  const std::vector<double> before{1.0, 2.0, 3.0};
  const std::vector<double> after{3.0, 2.0, 1.0, 0.0};
  const Bounded held = check_conserved(before, after);
  ASSERT_TRUE(held.has_value());
  EXPECT_EQ(held.value().residual, 0.0);
  EXPECT_EQ(held.value().tolerance, tolerance_of(residual::ErrorModel{4, 0}, 12.0));
  EXPECT_TRUE(residual::within(held.value()));

  const std::vector<double> lost{3.0, 2.0};
  const Bounded breached = check_conserved(before, lost);
  ASSERT_TRUE(breached.has_value());
  EXPECT_EQ(breached.value().residual, 1.0);
  EXPECT_FALSE(residual::within(breached.value()));

  // The deeper side sets the depth, whichever side it is.
  EXPECT_EQ(check_conserved(after, before).value().tolerance, held.value().tolerance);
  EXPECT_TRUE(residual::within(check_conserved({}, {}).value()));
}

TEST(InvariantConserved, NoToleranceFromValuesThatHaveNone) {
  const std::vector<double> good{1.0};
  const std::vector<double> nan{kNaN};
  EXPECT_EQ(check_conserved(nan, good).error(), residual::Underivable::kNonFiniteMagnitude);
  EXPECT_EQ(check_conserved(good, nan).error(), residual::Underivable::kNonFiniteMagnitude);
}

TEST(InvariantOrder, TheFirstDescentIsNamed) {
  EXPECT_EQ(first_descent({}), std::nullopt);
  const std::vector<std::uint64_t> one{5};
  EXPECT_EQ(first_descent(one), std::nullopt);
  const std::vector<std::uint64_t> sorted{0, 0, 1, 7, 7, ~std::uint64_t{0}};
  EXPECT_EQ(first_descent(sorted), std::nullopt) << "equal neighbours are in order";
  const std::vector<std::uint64_t> at_one{2, 1, 3};
  EXPECT_EQ(first_descent(at_one), 1U);
  const std::vector<std::uint64_t> at_last{1, 2, 3, 0};
  EXPECT_EQ(first_descent(at_last), 3U);
  const std::vector<std::uint64_t> two{1, 5, 4, 3};
  EXPECT_EQ(first_descent(two), 2U) << "the first, not the last";
}

TEST(InvariantPermutation, AFingerprintCountsAndIgnoresOrder) {
  EXPECT_EQ(fingerprint({}), (Fingerprint{0, 0, 0}));
  const std::vector<std::uint64_t> values{3, 1, 4, 1, 5};
  const std::vector<std::uint64_t> shuffled{1, 5, 3, 1, 4};
  EXPECT_EQ(fingerprint(values).count, 5U);
  EXPECT_EQ(fingerprint(values), fingerprint(shuffled));
  EXPECT_TRUE(is_permutation(values, shuffled));
  const std::vector<std::uint64_t> other{3, 1, 4, 1, 6};
  EXPECT_FALSE(is_permutation(values, other));
  const std::vector<std::uint64_t> shorter{3, 1, 4, 1};
  EXPECT_FALSE(is_permutation(values, shorter));
  // A zero is a value like any other: it moves the sums.
  const std::vector<std::uint64_t> zero{0};
  EXPECT_NE(fingerprint(zero), fingerprint({}));
  EXPECT_NE(fingerprint(zero).low, 0U);
}

TEST(InvariantPermutation, TheSumsAreOfPhiloxUnderTheFixedKey) {
  // One value's fingerprint is its Philox block under kFingerprintSeed:
  // a second reading of the definition in the header, computed here.
  const std::uint64_t value = 0x0123456789ABCDEFULL;
  const rng::Block block =
      rng::philox4x32_10(rng::Block{0x89ABCDEFU, 0x01234567U, 0, 0}, rng::key_of(kFingerprintSeed));
  const std::vector<std::uint64_t> one{value};
  const Fingerprint print = fingerprint(one);
  EXPECT_EQ(print.low, std::uint64_t{block[0]} | (std::uint64_t{block[1]} << 32U));
  EXPECT_EQ(print.high, std::uint64_t{block[2]} | (std::uint64_t{block[3]} << 32U));
  const std::vector<std::uint64_t> twice{value, value};
  EXPECT_EQ(fingerprint(twice).low, 2 * print.low);
}

TEST(InvariantPermutation, FingerprintsCompareMemberByMember) {
  const Fingerprint base{3, 5, 7};
  EXPECT_EQ(base, (Fingerprint{3, 5, 7}));
  EXPECT_NE(base, (Fingerprint{4, 5, 7}));
  EXPECT_NE(base, (Fingerprint{3, 6, 7}));
  EXPECT_NE(base, (Fingerprint{3, 5, 8}));
}

}  // namespace
}  // namespace loadforge::verification::invariant
