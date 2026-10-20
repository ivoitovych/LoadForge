// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b: what the specification's check (verification.md §8's three
// outcomes, in the terms of spec.hpp) makes of the workload's output when it
// is corrupted in each way, asserting WHICH classification it produces, and
// of the cases in which the check cannot be performed. The checker is
// tests/oracle/workloads/fft/fft_support.hpp, written from spec.hpp alone.
//
// A workload has no failing call of its own: its faults are wrong bits, and
// this is where the ones the specification claims to catch are caught, and the
// one it says it cannot (a low bit of a bin nobody sampled) is shown unseen.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "oracle/workloads/fft/fft_support.hpp"
#include "workloads/fft/fft.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::workloads::fft {
namespace {

using testing::Bytes;
using testing::Kind;
using testing::Verdict;

constexpr std::uint64_t kUnit = 5;
constexpr std::uint64_t kSeed = 31337;

class FftFault : public ::testing::TestWithParam<std::uint32_t> {
 protected:
  void SetUp() override {
    auto created = create(WorkloadConfig{kSeed, 4 * (kInputBytes + kOutputBytes), GetParam(),
                                         simd::IsaPath::kScalar});
    ASSERT_TRUE(created.has_value());
    workload_ = std::move(created).value();
    input_.assign(kInputBytes, std::byte{0});
    workload_->generate(kUnit, input_);
    output_ = testing::compute_unit(*workload_, kUnit, input_, serial_);
    bins_ = testing::sampled_bins(kUnit);
  }

  [[nodiscard]] Verdict check(const Bytes& output) const {
    return testing::check_unit(GetParam(), kUnit, input_, output);
  }

  /// A sampled bin and a bin the check does not sample.
  [[nodiscard]] std::size_t unsampled_bin() const {
    std::size_t k = 1;
    while (std::find(bins_.begin(), bins_.end(), k) != bins_.end()) {
      ++k;
    }
    return k;
  }

  static Bytes with_bit_flipped(Bytes bytes, std::size_t element, unsigned bit) {
    // Little-endian: bit b of the double is bit b % 8 of byte b / 8.
    bytes[element * 8 + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    return bytes;
  }

  std::unique_ptr<Workload> workload_;
  testing::Threads serial_{1};
  Bytes input_;
  Bytes output_;
  std::array<std::size_t, kSampledBins> bins_{};
};

TEST_P(FftFault, AnUncorruptedOutputIsAgreementOverAllSeventeenChecks) {
  const Verdict verdict = check(output_);
  EXPECT_EQ(verdict.kind, Kind::kAgreement);
  EXPECT_EQ(verdict.blocks_compared, kChecks);
  EXPECT_GT(verdict.tolerance, 0.0);
}

TEST_P(FftFault, AnExponentBitFlipInASampledBinIsADftBinMismatchNamingThatBin) {
  for (std::size_t ordinal = 0; ordinal < bins_.size(); ++ordinal) {
    const std::size_t k = bins_[ordinal];
    for (const std::size_t part : {std::size_t{0}, std::size_t{1}}) {
      const Verdict verdict = check(with_bit_flipped(output_, 2 * k + part, 55));
      ASSERT_EQ(verdict.kind, Kind::kBoundedMismatch) << "bin " << k;
      EXPECT_EQ(verdict.check, "dft-bin");
      EXPECT_EQ(verdict.element, k);
      EXPECT_EQ(verdict.blocks_compared, ordinal + 1) << "the ordinal of the failing check";
      EXPECT_GT(verdict.residual, verdict.tolerance);
    }
  }
}

TEST_P(FftFault, AHighMantissaBitFlipInASampledBinIsCaughtAndANearLowOneIsNot) {
  const std::size_t k = bins_[3];
  const Verdict high = check(with_bit_flipped(output_, 2 * k, 51));
  EXPECT_EQ(high.kind, Kind::kBoundedMismatch);
  EXPECT_EQ(high.check, "dft-bin");
  EXPECT_EQ(high.element, k);
  // Bit 0 is far below the derived tolerance: "an error below T goes unseen".
  EXPECT_EQ(check(with_bit_flipped(output_, 2 * k, 0)).kind, Kind::kAgreement);
}

TEST_P(FftFault, AnExponentBitFlipInAnUnsampledBinIsAParsevalMismatchAfterAllBins) {
  const std::size_t k = unsampled_bin();
  const Verdict verdict = check(with_bit_flipped(output_, 2 * k, 62));
  ASSERT_EQ(verdict.kind, Kind::kBoundedMismatch);
  EXPECT_EQ(verdict.check, "parseval");
  EXPECT_EQ(verdict.blocks_compared, kChecks);
  EXPECT_GT(verdict.residual, verdict.tolerance);
}

TEST_P(FftFault, ALowBitFlipInAnUnsampledBinIsUnseenAndTheSpecificationSaysSo) {
  const std::size_t k = unsampled_bin();
  EXPECT_EQ(check(with_bit_flipped(output_, 2 * k, 3)).kind, Kind::kAgreement);
}

TEST_P(FftFault, ANanOrInfinityIsAMismatchNeverAnAgreement) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const double poison : {nan, inf, -inf}) {
    std::vector<double> y = testing::to_doubles(output_);
    y[2 * bins_[5]] = poison;
    const Verdict sampled = check(testing::to_bytes(y));
    ASSERT_EQ(sampled.kind, Kind::kBoundedMismatch);
    EXPECT_EQ(sampled.check, "dft-bin");
    EXPECT_EQ(sampled.element, bins_[5]);
    EXPECT_FALSE(sampled.residual <= sampled.tolerance);

    y = testing::to_doubles(output_);
    y[2 * unsampled_bin() + 1] = poison;
    const Verdict unsampled = check(testing::to_bytes(y));
    ASSERT_EQ(unsampled.kind, Kind::kBoundedMismatch);
    EXPECT_EQ(unsampled.check, "parseval");
  }
}

TEST_P(FftFault, OfSeveralFailingBinsTheSmallestKIsNamedAndABinBeatsParseval) {
  std::vector<double> y = testing::to_doubles(output_);
  y[2 * bins_[9]] += 1.0;
  y[2 * bins_[4]] += 1.0;
  y[2 * unsampled_bin()] += 1.0;
  const Verdict verdict = check(testing::to_bytes(y));
  ASSERT_EQ(verdict.kind, Kind::kBoundedMismatch);
  EXPECT_EQ(verdict.check, "dft-bin");
  EXPECT_EQ(verdict.element, bins_[4]);
  EXPECT_EQ(verdict.blocks_compared, 5U) << "stops at the first failure";
}

TEST_P(FftFault, TheBinToleranceHoldsJustInsideAndBreaksJustOutsideEpsilonTwoToMinus20) {
  constexpr double kEpsilon = 1.0 / 1048576.0;
  const std::size_t k = bins_[7];
  const std::vector<double> x = testing::to_doubles(input_);
  const Complex direct = testing::direct_bin(x, k);
  const double tolerance = 64.0 * testing::norm2(x) * bin_tolerance_factor(GetParam()) *
                           (1.0 + kEvaluationSlack);  // T' of the bin checks
  for (const double scale : {1.0 - kEpsilon, 1.0 + kEpsilon}) {
    std::vector<double> y = testing::to_doubles(output_);
    y[2 * k] = direct.re + tolerance * scale;
    y[2 * k + 1] = direct.im;
    const Verdict verdict = check(testing::to_bytes(y));
    if (scale < 1.0) {
      EXPECT_EQ(verdict.kind, Kind::kAgreement) << "held at (1 - epsilon) T'";
    } else {
      ASSERT_EQ(verdict.kind, Kind::kBoundedMismatch) << "breached at (1 + epsilon) T'";
      EXPECT_EQ(verdict.check, "dft-bin");
      EXPECT_EQ(verdict.element, k);
    }
  }
}

TEST_P(FftFault, TheParsevalToleranceHoldsJustInsideAndBreaksJustOutsideEpsilon) {
  // Scale the whole output by 1 + d: every sampled bin stays far inside its
  // own tolerance, and s^ - 64 a^ moves by about d 64 a. Find the d at which
  // the verdict flips, to the last bit, and read the residual either side.
  const std::vector<double> y0 = testing::to_doubles(output_);
  const auto verdict_at = [&](double d) {
    std::vector<double> y = y0;
    for (double& value : y) {
      value *= 1.0 + d;
    }
    return check(testing::to_bytes(y));
  };
  double passing = 0.0;
  double failing = 1e-11;
  ASSERT_EQ(verdict_at(passing).kind, Kind::kAgreement);
  ASSERT_EQ(verdict_at(failing).check, "parseval") << "bins hold while Parseval breaks";
  for (int step = 0; step < 200; ++step) {
    const double middle = 0.5 * (passing + failing);
    if (middle == passing || middle == failing) {
      break;
    }
    (verdict_at(middle).kind == Kind::kAgreement ? passing : failing) = middle;
  }
  const Verdict inside = verdict_at(passing);
  const Verdict outside = verdict_at(failing);
  ASSERT_EQ(inside.kind, Kind::kAgreement);
  ASSERT_EQ(outside.kind, Kind::kBoundedMismatch);
  EXPECT_EQ(outside.check, "parseval");
  EXPECT_EQ(outside.blocks_compared, kChecks);
  EXPECT_LE(inside.residual, inside.tolerance);
  EXPECT_GT(outside.residual, outside.tolerance);
  // s^ is a double near 64 a, so the residual moves in steps of one ulp of it,
  // about 7e-5 of the tolerance: coarser than epsilon = 2^-20, so the flip
  // cannot be placed closer than a step by any output. The two sides of the
  // flip are within a few steps of each other, the rounding of the sum of squares included.
  const double ulp = std::nextafter(64.0 * testing::norm2(testing::to_doubles(input_)), 1e9) -
                     64.0 * testing::norm2(testing::to_doubles(input_));
  EXPECT_LE(outside.residual - inside.residual, 4.0 * ulp);
  EXPECT_GE(inside.residual, inside.tolerance - 4.0 * ulp);
}

TEST_P(FftFault, ACorruptedInputIsCaughtByRegenerationNotByTheTransformThatFollowsIt) {
  // F18: flip an exponent bit of the stored input, compute on it, and check the
  // result against the input REGENERATED from (seed, unit). The transform of
  // the corrupt input is a perfectly good transform of it; only the comparison
  // with the truth shows the damage.
  const Bytes corrupt = with_bit_flipped(input_, 40, 61);
  ASSERT_NE(corrupt, input_);
  const Bytes output = testing::compute_unit(*workload_, kUnit, corrupt, serial_);
  Bytes regenerated(kInputBytes);
  workload_->generate(kUnit, regenerated);
  EXPECT_EQ(regenerated, input_);
  EXPECT_EQ(testing::check_unit(GetParam(), kUnit, corrupt, output).kind, Kind::kAgreement)
      << "against the corrupt input the output is right";
  // 2^61 flips the exponent, the entry becomes huge (outside [-1, 1]): not checkable;
  // a mantissa flip stays checkable and is a dft-bin mismatch against the truth.
  const Bytes mantissa = with_bit_flipped(input_, 40, 51);
  const Verdict verdict = testing::check_unit(
      GetParam(), kUnit, regenerated, testing::compute_unit(*workload_, kUnit, mantissa, serial_));
  ASSERT_EQ(verdict.kind, Kind::kBoundedMismatch);
  EXPECT_TRUE(verdict.check == "dft-bin" || verdict.check == "parseval");
}

// ---- T5b: the checks that cannot be performed ----------------------------

TEST_P(FftFault, EvidenceOfTheWrongSizeIsNotCheckedNotMismatched) {
  for (const std::size_t size :
       {std::size_t{0}, kOutputBytes - 1, kOutputBytes + 1, kInputBytes * 2}) {
    const Bytes bad(size);
    const Verdict short_output = check(bad);
    EXPECT_EQ(short_output.kind, Kind::kNotChecked) << size;
    EXPECT_EQ(short_output.blocks_compared, 0U);
    const Verdict short_input = testing::check_unit(GetParam(), kUnit, bad, output_);
    EXPECT_EQ(short_input.kind, Kind::kNotChecked) << size;
    EXPECT_EQ(short_input.blocks_compared, 0U);
  }
}

TEST_P(FftFault, AnInputComponentThatIsNotFiniteOrAboveOneInMagnitudeIsNotChecked) {
  for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), 1.0000000000000002, -2.0}) {
    std::vector<double> x = testing::to_doubles(input_);
    x[17] = bad;
    const Verdict verdict = testing::check_unit(GetParam(), kUnit, testing::to_bytes(x), output_);
    EXPECT_EQ(verdict.kind, Kind::kNotChecked);
    EXPECT_EQ(verdict.blocks_compared, 0U);
  }
  std::vector<double> edge = testing::to_doubles(input_);
  edge[17] = 1.0;
  EXPECT_NE(testing::check_unit(GetParam(), kUnit, testing::to_bytes(edge), output_).kind,
            Kind::kNotChecked)
      << "magnitude exactly 1 is within the derivation";
}

TEST_P(FftFault, AnAllZeroInputIsNotCheckedBecauseItsToleranceWouldBeZero) {
  const Bytes zeros(kInputBytes);
  const Verdict verdict = testing::check_unit(GetParam(), kUnit, zeros, Bytes(kOutputBytes));
  EXPECT_EQ(verdict.kind, Kind::kNotChecked);
  EXPECT_EQ(verdict.blocks_compared, 0U);
  // One nonzero component and the check is performed.
  std::vector<double> x(2 * kPoints, 0.0);
  x[5] = 0.5;
  EXPECT_NE(testing::check_unit(GetParam(), kUnit, testing::to_bytes(x), Bytes(kOutputBytes)).kind,
            Kind::kNotChecked);
}

TEST_P(FftFault, TheSampledBinsAreSixteenDistinctIncreasingAndIncludeTheTwinAxes) {
  for (std::uint64_t unit = 0; unit < 50; ++unit) {
    const auto bins = testing::sampled_bins(unit);
    EXPECT_EQ(bins.front(), 0U);
    EXPECT_TRUE(std::binary_search(bins.begin(), bins.end(), kPoints / 2));
    EXPECT_TRUE(std::is_sorted(bins.begin(), bins.end()));
    EXPECT_EQ(std::adjacent_find(bins.begin(), bins.end()), bins.end());
    EXPECT_LT(bins.back(), kPoints);
    EXPECT_EQ(testing::sampled_bins(unit), bins) << "a function of the unit alone";
  }
}

INSTANTIATE_TEST_SUITE_P(Radix, FftFault, ::testing::Values(2U, 4U),
                         [](const ::testing::TestParamInfo<std::uint32_t>& info) {
                           return "Radix" + std::to_string(info.param);
                         });

}  // namespace
}  // namespace loadforge::workloads::fft
