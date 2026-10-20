// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test-local check, tested against hand-computed cases and against the
// specification's constants, so that the verdicts of the workload's tests mean
// something.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "oracle/workloads/particles/direct_check.hpp"
#include "unit/workloads/particles/support.hpp"

namespace loadforge::workloads::particles {
namespace {

using testing::Bytes;

TEST(ParticlesDirectCheck, TheDirectSumOfTwoParticlesAtDistanceOneIsTheHandComputedForce) {
  // Two particles at distance one, the rest together at a point far from both.
  std::vector<double> positions(3 * kParticles, 15.0);
  positions[0] = 9.0;
  positions[1] = 9.0;
  positions[2] = 9.0;
  positions[3] = 10.0;
  positions[4] = 9.0;
  positions[5] = 9.0;
  const direct::Direct first = direct::sum_directly(positions, 0);
  EXPECT_EQ(first.force[0], -0.5625) << "t = 1 - 1/4, w = 9/16, d = -1";
  EXPECT_EQ(first.force[1], 0.0);
  EXPECT_EQ(first.force[2], 0.0);
  EXPECT_EQ(first.abs_sum[0], 0.5625);
  EXPECT_EQ(first.candidates, 1U);
  const direct::Direct second = direct::sum_directly(positions, 1);
  EXPECT_EQ(second.force[0], 0.5625);
  EXPECT_EQ(second.candidates, 1U);
  const direct::Direct far = direct::sum_directly(positions, 7);
  EXPECT_EQ(far.candidates, kParticles - 3) << "the others at its own point are candidates";
  EXPECT_EQ(far.force[0], 0.0);
}

TEST(ParticlesDirectCheck, AParticleWithNoCandidateHasTheFloorAsItsTolerance) {
  direct::Direct none;
  EXPECT_EQ(direct::tolerance_of(none, 0), kMinTolerance);
  direct::Direct some;
  some.candidates = 33;
  some.abs_sum = {15.0, 15.0, 15.0};
  EXPECT_EQ(direct::tolerance_of(some, 1), force_tolerance(33, 15.0) * (1.0 + kEvaluationSlack));
}

TEST(ParticlesDirectCheck, TheVerdictIsNotCheckedForEvidenceOfTheWrongSizeOrAnInvalidInput) {
  const Bytes input(kInputBytes);
  const Bytes output(kOutputBytes);
  EXPECT_EQ(direct::check(0, Bytes(kInputBytes - 1), output).verdict, direct::Verdict::kNotChecked);
  EXPECT_EQ(direct::check(0, input, Bytes(kOutputBytes + 1)).verdict, direct::Verdict::kNotChecked);
  EXPECT_EQ(direct::check(0, Bytes{}, Bytes{}).verdict, direct::Verdict::kNotChecked);
  EXPECT_EQ(direct::check(0, input, output).verdict, direct::Verdict::kAgreement)
      << "all at the origin and no force: a valid unit";
  for (const double bad : {std::nan(""), HUGE_VAL, -HUGE_VAL, -1e-300, kBoxEdge, 100.0}) {
    std::vector<double> positions(3 * kParticles, 1.0);
    positions[3 * 100 + 1] = bad;
    EXPECT_EQ(direct::check(0, testing::as_bytes(positions), output).verdict,
              direct::Verdict::kNotChecked)
        << bad;
  }
}

}  // namespace
}  // namespace loadforge::workloads::particles
