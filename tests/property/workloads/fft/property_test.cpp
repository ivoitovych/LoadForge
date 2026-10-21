// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: properties of the DFT the workload must keep, over many generated inputs.
// Some hold exactly because scaling by a power of two and negation commute with
// rounding; the rest hold within the specification's bound, which is the whole
// tolerance a property of an inexact transform can claim.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "oracle/workloads/fft/fft_support.hpp"
#include "workloads/fft/fft.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::workloads::fft {
namespace {

using testing::Bytes;

class FftProperty : public ::testing::TestWithParam<std::uint32_t> {
 protected:
  void SetUp() override {
    auto created = create(
        WorkloadConfig{77, 4 * (kInputBytes + kOutputBytes), GetParam(), simd::IsaPath::kScalar});
    ASSERT_TRUE(created.has_value());
    workload_ = std::move(created).value();
  }

  std::vector<double> transform(const std::vector<double>& x) {
    return testing::to_doubles(testing::compute_unit(*workload_, 0, testing::to_bytes(x), serial_));
  }

  [[nodiscard]] static std::vector<double> input(std::uint64_t unit) {
    return testing::spec_input(77, unit);
  }

  [[nodiscard]] static double bound(const std::vector<double>& x) {
    return 64.0 * testing::norm2(x) * bin_tolerance_factor(GetParam());
  }

  std::unique_ptr<Workload> workload_;
  testing::Threads serial_{1};
};

TEST_P(FftProperty, ScalingByAPowerOfTwoScalesTheOutputExactly) {
  for (std::uint64_t unit = 0; unit < 6; ++unit) {
    const std::vector<double> x = input(unit);
    std::vector<double> scaled(x.size());
    std::transform(x.begin(), x.end(), scaled.begin(), [](double v) { return v * 4.0; });
    const std::vector<double> y = transform(x);
    const std::vector<double> ys = transform(scaled);
    for (std::size_t e = 0; e < y.size(); ++e) {
      ASSERT_EQ(std::bit_cast<std::uint64_t>(ys[e]), std::bit_cast<std::uint64_t>(y[e] * 4.0)) << e;
    }
  }
}

TEST_P(FftProperty, NegatingTheInputNegatesTheOutputExactly) {
  const std::vector<double> x = input(1);
  std::vector<double> negated(x.size());
  std::transform(x.begin(), x.end(), negated.begin(), [](double v) { return -v; });
  const std::vector<double> y = transform(x);
  const std::vector<double> yn = transform(negated);
  for (std::size_t e = 0; e < y.size(); ++e) {
    ASSERT_EQ(yn[e], -y[e]) << e;
  }
}

TEST_P(FftProperty, TheTransformIsLinear) {
  const std::vector<double> a = input(2);
  const std::vector<double> b = input(3);
  std::vector<double> sum(a.size());
  for (std::size_t e = 0; e < a.size(); ++e) {
    sum[e] = 0.5 * a[e] + 0.25 * b[e];
  }
  const std::vector<double> ya = transform(a);
  const std::vector<double> yb = transform(b);
  const std::vector<double> ys = transform(sum);
  const double tolerance = 3.0 * bound(a);
  for (std::size_t e = 0; e < ys.size(); ++e) {
    ASSERT_NEAR(ys[e], 0.5 * ya[e] + 0.25 * yb[e], tolerance) << e;
  }
}

TEST_P(FftProperty, RealInputGivesAConjugateSymmetricOutput) {
  std::vector<double> x = input(4);
  for (std::size_t j = 0; j < kPoints; ++j) {
    x[2 * j + 1] = 0.0;
  }
  const std::vector<double> y = transform(x);
  const double tolerance = 2.0 * bound(x);
  for (std::size_t k = 1; k < kPoints; ++k) {
    const std::size_t mirror = kPoints - k;
    ASSERT_NEAR(y[2 * k], y[2 * mirror], tolerance) << k;
    ASSERT_NEAR(y[2 * k + 1], -y[2 * mirror + 1], tolerance) << k;
  }
  EXPECT_NEAR(y[1], 0.0, tolerance) << "bin 0 of a real input is real";
  EXPECT_NEAR(y[2 * (kPoints / 2) + 1], 0.0, tolerance) << "and so is bin n/2";
}

TEST_P(FftProperty, ACircularShiftOfTheInputMultipliesBinsByAPhase) {
  const std::vector<double> x = input(5);
  constexpr std::size_t kShift = 301;
  std::vector<double> shifted(x.size());
  for (std::size_t j = 0; j < kPoints; ++j) {
    const std::size_t to = (j + kShift) % kPoints;
    shifted[2 * to] = x[2 * j];
    shifted[2 * to + 1] = x[2 * j + 1];
  }
  const std::vector<double> y = transform(x);
  const std::vector<double> ys = transform(shifted);
  const double tolerance = 3.0 * bound(x);
  for (std::size_t k = 0; k < kPoints; ++k) {
    const Complex phase = kTwiddles[(k * kShift) % kPoints];
    const double re = y[2 * k] * phase.re - y[2 * k + 1] * phase.im;
    const double im = y[2 * k] * phase.im + y[2 * k + 1] * phase.re;
    ASSERT_NEAR(ys[2 * k], re, tolerance) << k;
    ASSERT_NEAR(ys[2 * k + 1], im, tolerance) << k;
  }
}

TEST_P(FftProperty, SwappingRealAndImaginaryPartsSwapsTheOutputsMirrored) {
  // DFT(swap(x))_k = swap(DFT(x)_{n-k}).
  const std::vector<double> x = input(6);
  std::vector<double> swapped(x.size());
  for (std::size_t j = 0; j < kPoints; ++j) {
    swapped[2 * j] = x[2 * j + 1];
    swapped[2 * j + 1] = x[2 * j];
  }
  const std::vector<double> y = transform(x);
  const std::vector<double> ys = transform(swapped);
  const double tolerance = 2.0 * bound(x);
  for (std::size_t k = 0; k < kPoints; ++k) {
    const std::size_t mirror = (kPoints - k) % kPoints;
    ASSERT_NEAR(ys[2 * k], y[2 * mirror + 1], tolerance) << k;
    ASSERT_NEAR(ys[2 * k + 1], y[2 * mirror], tolerance) << k;
  }
}

TEST_P(FftProperty, ParsevalHoldsForEveryGeneratedUnit) {
  for (std::uint64_t unit = 0; unit < 30; ++unit) {
    const std::vector<double> x = input(unit);
    const double a = testing::norm2(x);
    const double s = testing::norm2(transform(x));
    ASSERT_LE(std::fabs(s - 64.0 * a), 64.0 * a * parseval_tolerance_factor(GetParam())) << unit;
  }
}

TEST_P(FftProperty, UnitsDoNotInteract) {
  // The transform of a unit does not depend on what the workload computed
  // before: the same input after a different one gives the same bits.
  const std::vector<double> x = input(7);
  const std::vector<double> first = transform(x);
  (void)transform(input(8));
  EXPECT_EQ(transform(x), first);
}

TEST_P(FftProperty, TheOutputOfTheCheckerIsAgreementForEveryProperty) {
  for (std::uint64_t unit = 0; unit < 6; ++unit) {
    const std::vector<double> x = input(unit);
    const testing::Verdict verdict = testing::check_unit(GetParam(), unit, testing::to_bytes(x),
                                                         testing::to_bytes(transform(x)));
    EXPECT_EQ(verdict.kind, testing::Kind::kAgreement) << unit;
  }
}

INSTANTIATE_TEST_SUITE_P(Radix, FftProperty, ::testing::Values(2U, 4U),
                         [](const ::testing::TestParamInfo<std::uint32_t>& info) {
                           return "Radix" + std::to_string(info.param);
                         });

}  // namespace
}  // namespace loadforge::workloads::fft
