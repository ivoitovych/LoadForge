// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b. The workload owns no verification path; its output is what one
// verifies. These tests corrupt that output, and the input it was computed
// from, and assert which classification the check of the specification
// produces -- and that a check that could not be run is never an agreement.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "oracle/workloads/particles/direct_check.hpp"
#include "unit/workloads/particles/support.hpp"

namespace loadforge::workloads::particles {
namespace {

using direct::Verdict;
using testing::Bytes;

constexpr std::uint64_t kUnit = 1;

struct Evidence {
  Bytes input;
  std::vector<double> positions;
  std::vector<double> forces;
  direct::Reference reference;
};

Evidence correct_evidence(std::uint32_t cells, simd::IsaPath path) {
  const auto workload = testing::make(testing::config_for(cells, path, 41));
  EXPECT_NE(workload, nullptr);
  testing::PoolExecutor pool(2);
  Evidence evidence;
  evidence.input = testing::generate(*workload, kUnit);
  evidence.positions = testing::as_doubles(evidence.input);
  evidence.forces = testing::as_doubles(testing::compute(*workload, kUnit, evidence.input, pool));
  evidence.reference = direct::make_reference(evidence.positions, direct::sample(kUnit));
  return evidence;
}

double flipped(double value, int bit) {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << bit));
}

TEST(ParticlesFault, ACorrectOutputIsAnAgreementOnAllFortyEightEntries) {
  for (const std::uint32_t cells : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const Evidence evidence = correct_evidence(cells, path);
      const direct::Result result = direct::judge(evidence.reference, evidence.forces);
      EXPECT_EQ(result.verdict, Verdict::kAgreement);
      EXPECT_EQ(result.entries_compared, kEntriesChecked);
    }
  }
}

TEST(ParticlesFault, AFlippedBitIsCaughtWhenItMovesTheForceBeyondTheToleranceAndNamesTheEntry) {
  std::size_t caught = 0;
  std::size_t unseen = 0;
  for (const simd::IsaPath path : testing::runnable_paths()) {
    Evidence evidence = correct_evidence(2, path);
    for (std::size_t k = 0; k < direct::sample(kUnit).size(); ++k) {
      const std::size_t p = evidence.reference.chosen[k];
      for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t element = 3 * p + c;
        const double original = evidence.forces[element];
        const double tolerance = direct::tolerance_of(evidence.reference.sums[k], c);
        for (int bit = 0; bit < 64; ++bit) {
          const double corrupted = flipped(original, bit);
          evidence.forces[element] = corrupted;
          const direct::Result result = direct::judge(evidence.reference, evidence.forces);
          evidence.forces[element] = original;
          // The residual is the change of the value plus the output's own
          // distance from the direct sum, which is a fraction of the tolerance.
          const double change = std::fabs(corrupted - original);
          if (original == 0.0 && bit == 63) {
            EXPECT_EQ(result.verdict, Verdict::kAgreement) << "-0 is the same force";
          } else if (!(change <= 2.0 * tolerance)) {
            ASSERT_EQ(result.verdict, Verdict::kBoundedMismatch)
                << "p " << p << " c " << c << " bit " << bit;
            EXPECT_EQ(result.element, element);
            EXPECT_EQ(result.entries_compared, 3 * k + c + 1);
            EXPECT_FALSE(result.residual <= result.tolerance) << "NaN included";
            EXPECT_EQ(result.tolerance, tolerance);
            ++caught;
          } else if (change < 0.25 * tolerance) {
            EXPECT_EQ(result.verdict, Verdict::kAgreement)
                << "p " << p << " c " << c << " bit " << bit << " change " << change;
            ++unseen;
          }
        }
      }
    }
  }
  // Not vacuous: the sign, the exponent and the upper mantissa are caught in
  // every entry, the low mantissa below the bound is not.
  EXPECT_GT(caught, testing::runnable_paths().size() * 48 * 40);
  EXPECT_GT(unseen, testing::runnable_paths().size() * 48 * 4);
}

TEST(ParticlesFault, OfSeveralCorruptEntriesTheFirstInCheckingOrderIsNamed) {
  Evidence evidence = correct_evidence(2, simd::IsaPath::kScalar);
  const std::vector<std::size_t>& chosen = evidence.reference.chosen;
  // The last sampled particle's y and an early one's z, written in the
  // reverse of the order they are checked in.
  evidence.forces[3 * chosen[15] + 1] += 1.0;
  evidence.forces[3 * chosen[3] + 2] += 1.0;
  direct::Result result = direct::judge(evidence.reference, evidence.forces);
  EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch);
  EXPECT_EQ(result.element, 3 * chosen[3] + 2);
  EXPECT_EQ(result.entries_compared, 3 * 3 + 2 + 1);
  // Within one particle x comes before y before z.
  evidence.forces[3 * chosen[3] + 2] = evidence.reference.sums[3].force[2];
  evidence.forces[3 * chosen[3] + 1] += 1.0;
  evidence.forces[3 * chosen[3]] += 1.0;
  result = direct::judge(evidence.reference, evidence.forces);
  EXPECT_EQ(result.element, 3 * chosen[3]);
  EXPECT_EQ(result.entries_compared, 3 * 3 + 1);
}

TEST(ParticlesFault, WrongOutputsOfEveryKindAreBoundedMismatches) {
  const Evidence evidence = correct_evidence(2, simd::IsaPath::kScalar);
  const std::size_t first = 3 * evidence.reference.chosen[0];  // particle 0
  ASSERT_GT(std::fabs(evidence.reference.sums[0].force[0]), 1e-3) << "particle 0 feels a force";

  std::vector<double> zeros(evidence.forces.size(), 0.0);
  direct::Result result = direct::judge(evidence.reference, zeros);
  EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch) << "an output left as zeros";
  EXPECT_EQ(result.element, first);

  std::vector<double> not_a_number = evidence.forces;
  not_a_number[first] = std::numeric_limits<double>::quiet_NaN();
  result = direct::judge(evidence.reference, not_a_number);
  EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch) << "NaN fails, it is not skipped";
  EXPECT_EQ(result.entries_compared, 1U);

  std::vector<double> infinite = evidence.forces;
  infinite[first + 1] = std::numeric_limits<double>::infinity();
  result = direct::judge(evidence.reference, infinite);
  EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch);
  EXPECT_EQ(result.element, first + 1);

  std::vector<double> input_as_output = evidence.positions;
  result = direct::judge(evidence.reference, input_as_output);
  EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch) << "the input where the output belongs";

  std::vector<double> shifted(evidence.forces.size(), 0.0);
  for (std::size_t k = 3; k < shifted.size(); ++k) {
    shifted[k] = evidence.forces[k - 3];
  }
  result = direct::judge(evidence.reference, shifted);
  EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch) << "forces one particle late";

  const auto other = testing::make(testing::config_for(2, simd::IsaPath::kScalar, 41));
  testing::SerialExecutor serial;
  const std::vector<double> another_unit = testing::as_doubles(
      testing::compute(*other, kUnit + 1, testing::generate(*other, kUnit + 1), serial));
  result = direct::judge(evidence.reference, another_unit);
  EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch) << "the output of another unit";
}

/// A particle within 1 of sampled particle 0, other than it.
std::size_t neighbour_of_first(const Evidence& evidence) {
  for (std::size_t j = 1; j < kParticles; ++j) {
    const double dx = evidence.positions[0] - evidence.positions[3 * j];
    const double dy = evidence.positions[1] - evidence.positions[3 * j + 1];
    const double dz = evidence.positions[2] - evidence.positions[3 * j + 2];
    if (dx * dx + dy * dy + dz * dz < 1.0) {
      return j;
    }
  }
  return 0;
}

TEST(ParticlesFault, AnInputCorruptedBeforeComputeShowsInTheForcesOfItsNeighbours) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    Evidence evidence = correct_evidence(2, path);
    const std::size_t neighbour = neighbour_of_first(evidence);
    ASSERT_GT(neighbour, 0U);
    const auto workload = testing::make(testing::config_for(2, path, 41));
    testing::SerialExecutor serial;
    // The neighbour's x has a bit of its mantissa flipped: a move of a
    // fraction of a unit, in the tool's own memory between generate and compute.
    std::vector<double> corrupted = evidence.positions;
    corrupted[3 * neighbour] = flipped(corrupted[3 * neighbour], 50);
    const std::vector<double> forces = testing::as_doubles(
        testing::compute(*workload, kUnit, testing::as_bytes(corrupted), serial));
    const direct::Result result = direct::judge(evidence.reference, forces);
    EXPECT_EQ(result.verdict, Verdict::kBoundedMismatch) << simd::describe(path);
    EXPECT_EQ(result.element / 3, 0U) << "the first sampled particle is the first to disagree";
  }
}

TEST(ParticlesFault, AnInputCorruptedBelowTheToleranceOrOutsideTheSampleGoesUnseen) {
  // What the check does not claim (spec.hpp, "What the bound does not claim"):
  // the lowest bit of a coordinate moves a force by about 1e-16, and a
  // particle none of the sampled ones has in its cutoff is not examined.
  Evidence evidence = correct_evidence(2, simd::IsaPath::kScalar);
  const auto workload = testing::make(testing::config_for(2, simd::IsaPath::kScalar, 41));
  testing::SerialExecutor serial;
  const std::size_t neighbour = neighbour_of_first(evidence);
  std::vector<double> tiny = evidence.positions;
  tiny[3 * neighbour + 1] = flipped(tiny[3 * neighbour + 1], 0);
  EXPECT_EQ(direct::judge(evidence.reference,
                          testing::as_doubles(testing::compute(*workload, kUnit,
                                                               testing::as_bytes(tiny), serial)))
                .verdict,
            Verdict::kAgreement);

  // A particle at least 2 from every sampled particle: move it by 3.
  std::vector<double> far = evidence.positions;
  for (std::size_t j = 1; j < kParticles; ++j) {
    bool clear = true;
    for (const std::size_t p : evidence.reference.chosen) {
      const double dx = far[3 * p] - far[3 * j];
      const double dy = far[3 * p + 1] - far[3 * j + 1];
      const double dz = far[3 * p + 2] - far[3 * j + 2];
      clear = clear && dx * dx + dy * dy + dz * dz > 4.0 * 4.0;
    }
    if (clear) {
      far[3 * j] = std::fmod(far[3 * j] + 3.0, kBoxEdge);
      break;
    }
  }
  EXPECT_EQ(direct::judge(evidence.reference,
                          testing::as_doubles(testing::compute(*workload, kUnit,
                                                               testing::as_bytes(far), serial)))
                .verdict,
            Verdict::kAgreement)
      << "unexamined particles are not examined";
}

// ---- T5b: a check that could not be run is never an agreement -------------

TEST(ParticlesFaultUnrun, EvidenceOfTheWrongSizeIsNotChecked) {
  const Evidence evidence = correct_evidence(2, simd::IsaPath::kScalar);
  const Bytes output = testing::as_bytes(evidence.forces);
  for (const std::size_t input_size : {std::size_t{0}, kInputBytes - 8, kInputBytes + 8,
                                       kInputBytes - 1}) {
    const direct::Result result = direct::check(kUnit, Bytes(input_size), output);
    EXPECT_EQ(result.verdict, Verdict::kNotChecked) << input_size;
    EXPECT_NE(result.verdict, Verdict::kAgreement);
    EXPECT_EQ(result.entries_compared, 0U);
  }
  for (const std::size_t output_size : {std::size_t{0}, kOutputBytes - 8, kOutputBytes + 8}) {
    const direct::Result result = direct::check(kUnit, evidence.input, Bytes(output_size));
    EXPECT_EQ(result.verdict, Verdict::kNotChecked) << output_size;
    EXPECT_EQ(result.entries_compared, 0U);
  }
  EXPECT_EQ(direct::check(kUnit, evidence.input, output).verdict, Verdict::kAgreement)
      << "and the right size is checked";
}

TEST(ParticlesFaultUnrun, AnInputThatCannotBearATolerancePreventsTheCheckWhateverTheOutputSays) {
  const Evidence evidence = correct_evidence(2, simd::IsaPath::kScalar);
  const Bytes output = testing::as_bytes(evidence.forces);
  const Bytes garbage_output(kOutputBytes, std::byte{0xFF});  // NaN everywhere
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), -1.0, -std::numeric_limits<double>::denorm_min(),
        kBoxEdge, 1e300}) {
    std::vector<double> positions = evidence.positions;
    positions[3 * 700 + 2] = bad;
    const Bytes input = testing::as_bytes(positions);
    for (const Bytes* shown : {&output, &garbage_output}) {
      const direct::Result result = direct::check(kUnit, input, *shown);
      EXPECT_EQ(result.verdict, Verdict::kNotChecked) << bad;
      EXPECT_EQ(result.entries_compared, 0U);
    }
  }
  // The same wrong output on a valid input is a mismatch, not NotChecked: a
  // NaN in the OUTPUT is a finding.
  EXPECT_EQ(direct::check(kUnit, evidence.input, garbage_output).verdict, Verdict::kBoundedMismatch);
}

TEST(ParticlesFaultUnrun, AnAgreementAlwaysCountsTheFullSample) {
  const Evidence evidence = correct_evidence(1, simd::IsaPath::kScalar);
  const direct::Result result =
      direct::check(kUnit, evidence.input, testing::as_bytes(evidence.forces));
  EXPECT_EQ(result.verdict, Verdict::kAgreement);
  EXPECT_EQ(result.entries_compared, kEntriesChecked) << "an agreement over nothing is not one";
  EXPECT_EQ(direct::sample(kUnit).size(), kSampledParticles);
}

}  // namespace
}  // namespace loadforge::workloads::particles
