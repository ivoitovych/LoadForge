// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the FFT oracle: the real tool's transform of generated units is
// agreed with, a corrupted one is not, and the derived tolerances hold at the
// boundary the specification names -- breached at (1 + epsilon) times T' and
// held at (1 - epsilon) times it, for both radices and both checks.

#include "verification/oracle/fft/fft_oracle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "oracle/verification/oracle/fft/fft_evidence.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::verification::oracle::fft {
namespace {

namespace spec = workloads::fft;
using evidence::Unit;

constexpr double kEpsilon = 1.0 / 1048576.0;  // 2^-20, the specification's

workloads::Plan plan_for(std::uint32_t radix) {
  return workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, radix, 0, 0};
}

std::unique_ptr<Oracle> make_oracle(std::uint32_t radix) {
  auto created = create(workloads::WorkloadConfig{}, plan_for(radix));
  return std::move(created).value();
}

contracts::OracleCase as_case(const Unit& unit) {
  return contracts::OracleCase{unit.index, unit.input, unit.output};
}

Verdict check_unit(const Oracle& oracle, const Unit& unit) {
  return oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
}

// The real tool's units, and corruptions of them that every check would see.
std::vector<contracts::OracleCase> real_tool_cases() {
  std::vector<contracts::OracleCase> cases;
  for (const std::uint64_t index : evidence::kFixtureIndices) {
    cases.push_back(as_case(evidence::real_tool_unit(index)));
  }
  return cases;
}

std::vector<contracts::OracleCase> corrupted_cases() {
  std::vector<contracts::OracleCase> cases;
  const std::vector<std::byte> base = as_case(evidence::real_tool_unit(5)).output;
  const auto input = as_case(evidence::real_tool_unit(5)).input;
  // An exponent bit of one value, wherever it falls: a sampled bin sees it
  // directly and every other bin through Parseval.
  for (const std::size_t element : {std::size_t{0}, std::size_t{37}, std::size_t{4097}}) {
    contracts::OracleCase unit{5, input, base};
    unit.output[element * 8 + 7] ^= std::byte{0x40};
    cases.push_back(unit);
  }
  // A high mantissa bit of one value.
  contracts::OracleCase mantissa{5, input, base};
  mantissa.output[1234 * 8 + 6] ^= std::byte{0x08};
  cases.push_back(mantissa);
  // A NaN where a number belongs.
  contracts::OracleCase nan{5, input, base};
  const std::vector<double> quiet(1, std::nan(""));
  const std::vector<std::byte> nan_bytes = evidence::bytes_of(quiet);
  std::copy(nan_bytes.begin(), nan_bytes.end(), nan.output.begin() + 777 * 8);
  cases.push_back(nan);
  return cases;
}

class FftOracleTolerance : public ::testing::TestWithParam<std::uint32_t> {};

INSTANTIATE_TEST_SUITE_P(BothRadices, FftOracleTolerance, ::testing::Values(2U, 4U));

// A unit whose only nonzero input is x_0 = 1: its transform is (1, 0) at every
// bin, exactly, so the oracle's own sum at every bin is exactly (1, 0) and the
// 2-norm of the input is exactly 1.
std::vector<double> impulse_input() {
  std::vector<double> x(2 * spec::kPoints, 0.0);
  x[0] = 1.0;
  return x;
}

std::vector<double> impulse_spectrum() {
  std::vector<double> spectrum(2 * spec::kPoints, 0.0);
  for (std::size_t k = 0; k < spec::kPoints; ++k) {
    spectrum[2 * k] = 1.0;
  }
  return spectrum;
}

double expected_bin_tolerance(std::uint32_t radix) {
  return 64.0 * spec::bin_tolerance_factor(radix) * (1.0 + spec::kEvaluationSlack);
}

double expected_parseval_tolerance(std::uint32_t radix) {
  return 64.0 * spec::parseval_tolerance_factor(radix) * (1.0 + spec::kEvaluationSlack);
}

TEST_P(FftOracleTolerance, ABinOffByMoreThanTheToleranceIsCaughtAndLessIsHeld) {
  const std::uint32_t radix = GetParam();
  const auto oracle = make_oracle(radix);
  const double limit = expected_bin_tolerance(radix);
  const std::vector<double> x = impulse_input();
  const auto bins = sampled_bins(11);
  // The first sampled bin, and the last: the ordinal of the failing check is
  // the number of checks made. The imaginary part is exactly zero in the
  // exact transform, so the residual is the offset itself, to the last bit.
  for (const std::size_t which : {std::size_t{0}, spec::kSampledBins - 1}) {
    const std::size_t k = bins[which];
    std::vector<double> spectrum = impulse_spectrum();
    spectrum[2 * k + 1] = limit * (1.0 + kEpsilon);
    const Verdict breached = check_unit(*oracle, evidence::unit_of(11, x, spectrum));
    ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(breached)) << "bin " << k;
    const auto& mismatch = std::get<BoundedMismatch>(breached);
    EXPECT_EQ(mismatch.check, "dft-bin");
    EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>(k));
    EXPECT_EQ(mismatch.blocks_compared, which + 1);
    EXPECT_EQ(mismatch.tolerance, limit);
    EXPECT_EQ(mismatch.residual, limit * (1.0 + kEpsilon));
    EXPECT_TRUE(is_valid(breached, Contract::kBounded));

    spectrum[2 * k + 1] = limit * (1.0 - kEpsilon);
    const Verdict held = check_unit(*oracle, evidence::unit_of(11, x, spectrum));
    ASSERT_TRUE(std::holds_alternative<Agreement>(held)) << "bin " << k;
    EXPECT_EQ(std::get<Agreement>(held).blocks_compared, spec::kChecks);
  }
}

// The sum of squares of a spectrum that is (1, 0) everywhere but one bin, whose
// real part is 1 + delta, and the residual the Parseval check measures.
double parseval_residual(std::size_t bin, double delta) {
  std::vector<double> spectrum = impulse_spectrum();
  spectrum[2 * bin] = 1.0 + delta;
  double squares = 0.0;
  for (const double value : spectrum) {
    squares += value * value;
  }
  return std::fabs(std::sqrt(squares) - 64.0);
}

TEST_P(FftOracleTolerance, ParsevalIsHeldJustInsideItsToleranceAndBreachedJustOutside) {
  const std::uint32_t radix = GetParam();
  const auto oracle = make_oracle(radix);
  const double limit = expected_parseval_tolerance(radix);
  const std::vector<double> x = impulse_input();
  const auto bins = sampled_bins(0);
  std::size_t bin = 1;
  while (std::find(bins.begin(), bins.end(), bin) != bins.end()) {
    ++bin;
  }

  // The residual is a difference of two doubles near 64, so it moves in steps
  // of 2^-47, which are about 2^-13 of the tolerance: epsilon = 2^-20 cannot be
  // landed on. Find the offsets whose residual is the nearest the limit on each
  // side, and require both sides of epsilon.
  auto smallest_offset_with = [&](double target) {
    double low = 0.0;
    double high = 1.0;
    for (int step = 0; step < 200; ++step) {
      const double middle = 0.5 * (low + high);
      (parseval_residual(bin, middle) >= target ? high : low) = middle;
    }
    return high;
  };
  const double breached_offset = smallest_offset_with(limit * (1.0 + kEpsilon));
  const double held_offset = std::nextafter(smallest_offset_with(limit * (1.0 - kEpsilon)), 0.0);
  ASSERT_GE(parseval_residual(bin, breached_offset), limit * (1.0 + kEpsilon));
  ASSERT_LE(parseval_residual(bin, held_offset), limit * (1.0 - kEpsilon));
  EXPECT_GE(parseval_residual(bin, held_offset), limit * (1.0 - 1.0 / 2048.0));
  EXPECT_LE(parseval_residual(bin, breached_offset), limit * (1.0 + 1.0 / 2048.0));

  std::vector<double> spectrum = impulse_spectrum();
  spectrum[2 * bin] = 1.0 + breached_offset;
  const Verdict breached = check_unit(*oracle, evidence::unit_of(0, x, spectrum));
  ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(breached));
  const auto& mismatch = std::get<BoundedMismatch>(breached);
  EXPECT_EQ(mismatch.check, "parseval");
  EXPECT_EQ(mismatch.element, std::nullopt);
  EXPECT_EQ(mismatch.blocks_compared, spec::kChecks);
  EXPECT_EQ(mismatch.tolerance, limit);
  EXPECT_EQ(mismatch.residual, parseval_residual(bin, breached_offset));

  spectrum[2 * bin] = 1.0 + held_offset;
  const Verdict held = check_unit(*oracle, evidence::unit_of(0, x, spectrum));
  ASSERT_TRUE(std::holds_alternative<Agreement>(held));
  EXPECT_EQ(std::get<Agreement>(held).blocks_compared, spec::kChecks);
}

TEST(FftOracleRealTool, EveryFixtureIsAgreedWithAtBothRadicesAfterAllChecks) {
  for (const std::uint32_t radix : spec::kRadices) {
    const auto oracle = make_oracle(radix);
    for (const std::uint64_t index : evidence::kFixtureIndices) {
      const Verdict verdict = check_unit(*oracle, evidence::real_tool_unit(index));
      ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "radix " << radix;
      EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kChecks);
    }
  }
}

TEST(FftOracleRealTool, TheFixtureInputIsTheSpecificationsGeneration) {
  // The fixture's output is numpy's transform of the input the Python
  // generator built. That the oracle agrees with it (above) already says the
  // two inputs match to the tolerance; this says they match to the bit, by
  // a different route: the transform of the regenerated input at bin 0 is the
  // sum of its components, which numpy's X_0 reproduces to rounding.
  for (const std::uint64_t index : evidence::kFixtureIndices) {
    const Unit unit = evidence::real_tool_unit(index);
    const std::vector<double> x = evidence::doubles_of(unit.input);
    const std::vector<double> spectrum = evidence::doubles_of(unit.output);
    long double re = 0.0L;
    long double im = 0.0L;
    for (std::size_t j = 0; j < spec::kPoints; ++j) {
      re += static_cast<long double>(x[2 * j]);
      im += static_cast<long double>(x[2 * j + 1]);
    }
    EXPECT_NEAR(static_cast<double>(re), spectrum[0], 1e-12) << "unit " << index;
    EXPECT_NEAR(static_cast<double>(im), spectrum[1], 1e-12) << "unit " << index;
  }
}

}  // namespace

}  // namespace loadforge::verification::oracle::fft

// The suites are instantiated where they are declared: the macros name them
// unqualified.
namespace loadforge::contracts {
namespace {

namespace under_test = verification::oracle::fft;

// This binary instantiates the oracle suite only.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);

struct RadixFour {
  static constexpr const char* kWorkload = "fft";
  static constexpr verification::Contract kContract = verification::Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() { return under_test::make_oracle(4); }
  static std::vector<OracleCase> good() { return under_test::real_tool_cases(); }
  static std::vector<OracleCase> wrong() { return under_test::corrupted_cases(); }
};

struct RadixTwo {
  static constexpr const char* kWorkload = "fft";
  static constexpr verification::Contract kContract = verification::Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() { return under_test::make_oracle(2); }
  static std::vector<OracleCase> good() { return under_test::real_tool_cases(); }
  static std::vector<OracleCase> wrong() { return under_test::corrupted_cases(); }
};

using Radices = ::testing::Types<RadixFour, RadixTwo>;
INSTANTIATE_TYPED_TEST_SUITE_P(Fft, OracleContract, Radices);

}  // namespace
}  // namespace loadforge::contracts
