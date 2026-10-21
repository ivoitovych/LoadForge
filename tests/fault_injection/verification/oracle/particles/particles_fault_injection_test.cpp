// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for the particles oracle. T5: corruption is introduced into the
// output of a unit the real tool computed, and the verdict is asserted for
// which entry it named and how many entries had been compared. T5b: the check
// is made impossible, and the verdict must be NotChecked with nothing
// compared, never an agreement.

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

#include "oracle/verification/oracle/particles/particles_evidence.hpp"
#include "verification/oracle/particles/particles_oracle.hpp"
#include "workloads/particles/spec.hpp"

namespace loadforge::verification::oracle::particles {
namespace {

namespace spec = workloads::particles;
using evidence::Point;
using evidence::Rig;
using evidence::Unit;

const Unit& good_unit() {
  static const Unit unit = evidence::real_tool_unit();
  return unit;
}

std::unique_ptr<Oracle> make_oracle() {
  auto created =
      create(workloads::WorkloadConfig{}, workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0,
                                                          1, spec::kDefaultCellsPerCutoff, 0, 0});
  return std::move(created).value();
}

Verdict check_unit(const Unit& unit) {
  const auto oracle = make_oracle();
  return oracle->check(UnitEvidence{unit.index, unit.input, unit.output});
}

void flip(std::vector<std::byte>& bytes, std::size_t element, unsigned bit) {
  bytes[element * 8 + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
}

void put(std::vector<std::byte>& bytes, std::size_t element, double value) {
  const std::vector<std::byte> encoded = evidence::bytes_of(std::vector<double>(1, value));
  std::copy(encoded.begin(), encoded.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(element * 8));
}

BoundedMismatch mismatch_of(const Verdict& verdict) {
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
  return std::get<BoundedMismatch>(verdict);
}

std::uint32_t unsampled_particle() {
  const auto sampled = sampled_particles(good_unit().index);
  std::uint32_t p = 1000;
  while (std::find(sampled.begin(), sampled.end(), p) != sampled.end()) {
    ++p;
  }
  return p;
}

TEST(ParticlesFaultInjection, TheUncorruptedUnitIsAgreedWith) {
  const Verdict verdict = check_unit(good_unit());
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kEntriesChecked);
}

TEST(ParticlesFaultInjection, AnExponentFlipInASampledEntryIsNamedWithItsOrdinal) {
  const auto sampled = sampled_particles(good_unit().index);
  for (const std::size_t which : {std::size_t{0}, std::size_t{6}, spec::kSampledParticles - 1}) {
    for (std::size_t component = 0; component < 3; ++component) {
      Unit unit = good_unit();
      const std::size_t element = 3 * std::size_t{sampled[which]} + component;
      flip(unit.output, element, 62);
      const BoundedMismatch found = mismatch_of(check_unit(unit));
      EXPECT_EQ(found.check, "force-component");
      EXPECT_EQ(found.element, std::optional<std::uint64_t>(element));
      EXPECT_EQ(found.blocks_compared, 3 * which + component + 1);
      EXPECT_GT(found.residual, found.tolerance);
      EXPECT_EQ(found.condition_number, std::nullopt);
    }
  }
}

TEST(ParticlesFaultInjection, ASignFlipAndAHighMantissaFlipAreCaught) {
  const auto sampled = sampled_particles(good_unit().index);
  for (const unsigned bit : {63U, 51U, 40U}) {
    Unit unit = good_unit();
    flip(unit.output, 3 * std::size_t{sampled[2]} + 1, bit);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(3 * std::size_t{sampled[2]} + 1))
        << "bit " << bit;
  }
}

TEST(ParticlesFaultInjection, OfSeveralFailingEntriesTheFirstInTheOrderIsNamed) {
  const auto sampled = sampled_particles(good_unit().index);
  Unit unit = good_unit();
  flip(unit.output, 3 * std::size_t{sampled[9]} + 2, 62);
  flip(unit.output, 3 * std::size_t{sampled[4]} + 1, 62);
  flip(unit.output, 3 * std::size_t{sampled[4]} + 2, 62);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(3 * std::size_t{sampled[4]} + 1));
  EXPECT_EQ(found.blocks_compared, 3 * 4 + 1 + 1);
}

TEST(ParticlesFaultInjection, AnUnsampledParticleIsNotExamined) {
  // The bound does not claim the other particles are checked.
  Unit unit = good_unit();
  flip(unit.output, 3 * std::size_t{unsampled_particle()}, 62);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(unit)));
}

TEST(ParticlesFaultInjection, ANaNOrInfinityInASampledEntryFailsWithANonFiniteResidual) {
  const auto sampled = sampled_particles(good_unit().index);
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    Unit unit = good_unit();
    const std::size_t element = 3 * std::size_t{sampled[11]} + 2;
    put(unit.output, element, bad);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(element));
    EXPECT_TRUE(std::isnan(found.residual) || std::isinf(found.residual));
    EXPECT_TRUE(std::isfinite(found.tolerance));
    EXPECT_GT(found.tolerance, 0.0);
  }
}

TEST(ParticlesFaultInjection, TheLowestMantissaBitOfAForceIsBelowWhatTheBoundClaims) {
  // "An error below T goes unseen" (spec.hpp): no false alarm on the least
  // significant bit of a force of order one.
  const auto sampled = sampled_particles(good_unit().index);
  Unit unit = good_unit();
  flip(unit.output, 3 * std::size_t{sampled[1]}, 0);
  flip(unit.output, 3 * std::size_t{sampled[8]} + 1, 1);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(unit)));
}

TEST(ParticlesFaultInjection, AParticleWithNoCandidateGetsTheSmallestToleranceSoAnyForceFails) {
  const Rig rig(5);
  std::vector<double> forces = rig.agreeing_forces();
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(rig.unit_with(forces))));
  const std::size_t element = 3 * std::size_t{rig.sampled[2]} + 1;
  forces[element] = 1.0e-300;
  const BoundedMismatch found = mismatch_of(check_unit(rig.unit_with(forces)));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(element));
  EXPECT_EQ(found.tolerance, spec::kMinTolerance);
  EXPECT_EQ(found.residual, 1.0e-300);
  EXPECT_EQ(found.blocks_compared, 3 * 2 + 1 + 1);
  forces[element] = spec::kMinTolerance * 0.5;  // below the floor: held
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(rig.unit_with(forces))));
}

TEST(ParticlesFaultInjection, ACorruptedPositionIsJudgedByTheOutputItDidNotProduce) {
  // The oracle judges the output against the input it is given (oracle.hpp).
  // One coordinate of a neighbour of the first sampled particle moves, the
  // output stays: a mismatch of the output; the runner's bracket says the input
  // was the side that changed.
  Unit unit = good_unit();
  const std::vector<double> positions = evidence::doubles_of(unit.input);
  const Force before = direct_force(positions, 0);
  ASSERT_GT(before.candidates, 0U);
  std::size_t neighbour = 1;
  while (true) {
    const double dx = positions[0] - positions[3 * neighbour];
    const double dy = positions[1] - positions[3 * neighbour + 1];
    const double dz = positions[2] - positions[3 * neighbour + 2];
    if (dx * dx + dy * dy + dz * dz < 3.0) {
      break;
    }
    ++neighbour;
  }
  flip(unit.input, 3 * neighbour, 40);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.element.value() / 3, 0U) << "the force on particle 0 moves first";
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

TEST(ParticlesNotPerformed, EvidenceOfTheWrongSizeIsNotChecked) {
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

TEST(ParticlesNotPerformed, ACoordinateThatIsNotFiniteIsNotChecked) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    for (const std::size_t element :
         {std::size_t{0}, std::size_t{6000}, 3 * spec::kParticles - 1}) {
      Unit unit = good_unit();
      put(unit.input, element, bad);
      expect_not_checked(check_unit(unit));
    }
  }
}

TEST(ParticlesNotPerformed, ACoordinateOutsideTheBoxIsNotChecked) {
  for (const double bad : {spec::kBoxEdge, 16.000000000000004, 100.0, -1.0e-300,
                           -std::numeric_limits<double>::denorm_min(), -3.0}) {
    Unit unit = good_unit();
    put(unit.input, 1234, bad);
    expect_not_checked(check_unit(unit));
  }
  // The edges the generator can reach are covered: 0 and the largest below 16.
  for (const double edge : {0.0, -0.0, std::nextafter(spec::kBoxEdge, 0.0)}) {
    Unit unit = good_unit();
    put(unit.input, 1234, edge);
    EXPECT_FALSE(std::holds_alternative<NotChecked>(check_unit(unit)));
  }
}

TEST(ParticlesNotPerformed, ANaNInTheOutputIsAMismatchNotAnUnrunCheck) {
  Unit unit = good_unit();
  put(unit.output, 0, std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check_unit(unit)));
}

}  // namespace
}  // namespace loadforge::verification::oracle::particles
