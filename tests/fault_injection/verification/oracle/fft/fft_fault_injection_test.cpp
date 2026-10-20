// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for the FFT oracle. T5: corruption is introduced into the output
// of a unit the real tool computed, and the verdict is asserted for which check
// fired, which bin it named and how many checks had been made. T5b: the check
// is made impossible, and the verdict must be NotChecked with nothing compared,
// never an agreement.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "oracle/verification/oracle/fft/fft_evidence.hpp"
#include "verification/oracle/fft/fft_oracle.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::verification::oracle::fft {
namespace {

namespace spec = workloads::fft;
using evidence::Unit;

constexpr std::uint64_t kIndex = 5;

const Unit& good_unit() {
  static const Unit unit = evidence::real_tool_unit(kIndex);
  return unit;
}

std::unique_ptr<Oracle> make_oracle(std::uint32_t radix = spec::kDefaultRadix) {
  auto created = create(workloads::WorkloadConfig{},
                        workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, radix, 0, 0});
  return std::move(created).value();
}

Verdict check_unit(const Unit& unit, std::uint32_t radix = spec::kDefaultRadix) {
  const auto oracle = make_oracle(radix);
  return oracle->check(UnitEvidence{unit.index, unit.input, unit.output});
}

/// Flips bit `bit` (0 = least significant) of double `element` of `bytes`.
void flip(std::vector<std::byte>& bytes, std::size_t element, unsigned bit) {
  bytes[element * 8 + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
}

void put(std::vector<std::byte>& bytes, std::size_t element, double value) {
  const std::vector<double> one(1, value);
  const std::vector<std::byte> encoded = evidence::bytes_of(one);
  std::copy(encoded.begin(), encoded.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(element * 8));
}

std::size_t unsampled_bin() {
  const auto bins = sampled_bins(kIndex);
  std::size_t k = 100;
  while (std::find(bins.begin(), bins.end(), k) != bins.end()) {
    ++k;
  }
  return k;
}

BoundedMismatch mismatch_of(const Verdict& verdict) {
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
  return std::get<BoundedMismatch>(verdict);
}

TEST(FftFaultInjection, TheUnflippedUnitIsAgreedWith) {
  const Verdict verdict = check_unit(good_unit());
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kChecks);
}

TEST(FftFaultInjection, AnExponentFlipInASampledBinIsNamedWithItsOrdinal) {
  const auto bins = sampled_bins(kIndex);
  for (const std::size_t which : {std::size_t{0}, std::size_t{3}, spec::kSampledBins - 1}) {
    Unit unit = good_unit();
    flip(unit.output, 2 * std::size_t{bins[which]}, 62);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.check, "dft-bin");
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(bins[which]));
    EXPECT_EQ(found.blocks_compared, which + 1);
    EXPECT_GT(found.residual, found.tolerance);
    EXPECT_EQ(found.condition_number, std::nullopt);
  }
}

TEST(FftFaultInjection, AHighMantissaFlipOfTheImaginaryPartIsCaught) {
  const auto bins = sampled_bins(kIndex);
  Unit unit = good_unit();
  flip(unit.output, 2 * std::size_t{bins[0]} + 1, 51);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "dft-bin");
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(bins[0]));
  EXPECT_EQ(found.blocks_compared, 1U);
}

TEST(FftFaultInjection, OfSeveralFailingBinsTheSmallestKIsNamed) {
  const auto bins = sampled_bins(kIndex);
  Unit unit = good_unit();
  flip(unit.output, 2 * std::size_t{bins[11]}, 62);
  flip(unit.output, 2 * std::size_t{bins[4]}, 62);
  flip(unit.output, 2 * std::size_t{bins[7]} + 1, 62);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(bins[4]));
  EXPECT_EQ(found.blocks_compared, 5U);
}

TEST(FftFaultInjection, AnErrorInAnUnsampledBinIsCaughtByParseval) {
  Unit unit = good_unit();
  flip(unit.output, 2 * unsampled_bin(), 62);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "parseval");
  EXPECT_EQ(found.element, std::nullopt);
  EXPECT_EQ(found.blocks_compared, spec::kChecks);
}

TEST(FftFaultInjection, AFailingSampledBinTakesPrecedenceOverParseval) {
  const auto bins = sampled_bins(kIndex);
  Unit unit = good_unit();
  flip(unit.output, 2 * unsampled_bin(), 62);
  flip(unit.output, 2 * std::size_t{bins[8]}, 62);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "dft-bin");
  EXPECT_EQ(found.blocks_compared, 9U);
}

TEST(FftFaultInjection, ANaNOrInfinityInASampledBinFailsWhichevierPartHoldsIt) {
  const auto bins = sampled_bins(kIndex);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const std::size_t part : {std::size_t{0}, std::size_t{1}}) {
    for (const double bad : {nan, inf, -inf}) {
      Unit unit = good_unit();
      put(unit.output, 2 * std::size_t{bins[6]} + part, bad);
      const BoundedMismatch found = mismatch_of(check_unit(unit));
      EXPECT_EQ(found.check, "dft-bin");
      EXPECT_EQ(found.element, std::optional<std::uint64_t>(bins[6]));
      EXPECT_EQ(found.blocks_compared, 7U);
      EXPECT_TRUE(std::isnan(found.residual) || std::isinf(found.residual))
          << "part " << part << " value " << bad;
      EXPECT_TRUE(std::isfinite(found.tolerance));
    }
  }
}

TEST(FftFaultInjection, ANaNOrInfinityInAnUnsampledBinIsCaughtByParseval) {
  const std::size_t k = unsampled_bin();
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    Unit unit = good_unit();
    put(unit.output, 2 * k + 1, bad);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.check, "parseval");
    EXPECT_EQ(found.blocks_compared, spec::kChecks);
  }
}

TEST(FftFaultInjection, AnOutputWhoseSquaresOverflowFailsParsevalWithAnInfiniteResidual) {
  Unit unit = good_unit();
  put(unit.output, 2 * unsampled_bin(), 1.0e300);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "parseval");
  EXPECT_TRUE(std::isinf(found.residual));
}

TEST(FftFaultInjection, BothRadicesCatchTheSameCorruption) {
  const auto bins = sampled_bins(kIndex);
  Unit unit = good_unit();
  flip(unit.output, 2 * std::size_t{bins[2]}, 60);
  for (const std::uint32_t radix : spec::kRadices) {
    const BoundedMismatch found = mismatch_of(check_unit(unit, radix));
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(bins[2]));
  }
}

TEST(FftFaultInjection, TheLowestMantissaBitOfAValueIsBelowWhatTheBoundClaims) {
  // "An error below T goes unseen" (spec.hpp): the oracle must not raise a
  // false alarm on the least significant bit of a value of this size.
  const auto bins = sampled_bins(kIndex);
  Unit unit = good_unit();
  flip(unit.output, 2 * std::size_t{bins[5]}, 0);
  flip(unit.output, 2 * unsampled_bin() + 1, 0);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(unit)));
}

TEST(FftFaultInjection, AnOutputRightForAnotherInputIsWrongForThisOne) {
  // The oracle judges the output against the input it is given (oracle.hpp).
  // One mantissa bit of the INPUT moves, the output stays: the verdict is a
  // mismatch of the output, and it is the runner's bracket that says the input
  // was the side that changed.
  Unit unit = good_unit();
  flip(unit.input, 100, 51);
  const Verdict verdict = check_unit(unit);
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
}

// T5b ------------------------------------------------------------------------

void expect_not_checked(const Verdict& verdict) {
  ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict))
      << "an unrun check must never be reported as anything else";
  const auto& unchecked = std::get<NotChecked>(verdict);
  EXPECT_EQ(unchecked.blocks_compared, 0U);
  EXPECT_FALSE(unchecked.what.empty());
  EXPECT_FALSE(unchecked.why.empty());
  EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
}

TEST(FftNotPerformed, EvidenceOfTheWrongSizeIsNotChecked) {
  std::vector<Unit> units(8, good_unit());
  units[0].input.pop_back();
  units[1].input.push_back(std::byte{0});
  units[2].input.clear();
  units[3].output.pop_back();
  units[4].output.push_back(std::byte{0});
  units[5].output.clear();
  units[6].input.resize(spec::kInputBytes / 2);
  units[7].output.resize(spec::kOutputBytes * 2);
  for (const Unit& unit : units) {
    expect_not_checked(check_unit(unit));
  }
}

TEST(FftNotPerformed, AnInputComponentThatIsNotFiniteIsNotChecked) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    for (const std::size_t element : {std::size_t{0}, std::size_t{4095}, std::size_t{8191}}) {
      Unit unit = good_unit();
      put(unit.input, element, bad);
      expect_not_checked(check_unit(unit));
    }
  }
}

TEST(FftNotPerformed, AnInputComponentAboveOneInMagnitudeIsNotChecked) {
  for (const double bad : {1.0000000000000002, -1.0000000000000002, 2.0, -1.0e10}) {
    Unit unit = good_unit();
    put(unit.input, 1234, bad);
    expect_not_checked(check_unit(unit));
  }
  // One is the largest magnitude the tolerance covers: -1 is generated.
  Unit edge = good_unit();
  put(edge.input, 1234, -1.0);
  EXPECT_FALSE(std::holds_alternative<NotChecked>(check_unit(edge)));
}

TEST(FftNotPerformed, AnAllZeroInputIsNotCheckedEvenWithAnAllZeroOutput) {
  Unit unit = good_unit();
  std::fill(unit.input.begin(), unit.input.end(), std::byte{0});
  expect_not_checked(check_unit(unit));
  std::fill(unit.output.begin(), unit.output.end(), std::byte{0});
  expect_not_checked(check_unit(unit));
}

TEST(FftNotPerformed, ANaNInTheOutputIsAMismatchNotAnUnrunCheck) {
  Unit unit = good_unit();
  put(unit.output, 0, std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check_unit(unit)));
}

}  // namespace
}  // namespace loadforge::verification::oracle::fft
