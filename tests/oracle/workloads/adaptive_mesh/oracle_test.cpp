// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the adaptive mesh workload: a scalar recomputation, written in the
// tests (tests/unit/workloads/adaptive_mesh/support.hpp) from the
// specification's prose, of the cells the workload computed. The mesh is
// rebuilt by plain recursion, each leaf valued by a formula of its own, and
// the leaves of a coarse cell added one by one in leaf order from +0.0, which
// is c'. The workload's integral c must be within cell_tolerance(m, c') of c'
// and its leaf count exactly m. Every cell of every unit is checked, the
// sampled cells the oracle will check among them, on executors that group the
// leaves differently. The checks are shown able to fail: a stored value just
// outside the tolerance is refused and one just inside is accepted, and an
// input that crowds a cell to hundreds of leaves is where that margin is
// narrow enough to be a test (spec.hpp, step 5).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "unit/workloads/adaptive_mesh/support.hpp"

namespace loadforge::workloads::adaptive_mesh::testing {
namespace {

/// The samples of the specification: sample j is a cell in [8 j, 8 j + 8),
/// chosen by a function of the unit index alone. Any such function serves
/// here: we check all of them.
std::size_t sampled_cell(std::size_t j, std::uint64_t unit) { return (8 * j) + (unit % 8); }

TEST(AdaptiveMeshOracle, EveryCellOfGeneratedUnitsAgreesOnEveryScheduleAndTheSamplesAreAmongThem) {
  const auto workload = make(config_for(5, 16));
  ASSERT_NE(workload, nullptr);
  Serial serial;
  Dealt dealt(3, 1);
  Shuffled shuffled(5, 2);
  Claiming claiming(4);
  worker::Executor* const executors[] = {&serial, &dealt, &shuffled, &claiming};
  for (std::uint64_t unit = 0; unit < 6; ++unit) {
    const Bytes input = generated(*workload, unit);
    const Hotspots spots = hotspots_of(input);
    for (worker::Executor* executor : executors) {
      const Bytes output = computed(*workload, unit, input, *executor);
      EXPECT_EQ(first_disagreement(output, spots), "")
          << "unit " << unit << ", " << executor->threads() << " threads";
    }
    // The sampled cells are distinct, spread over the whole square, and cells.
    for (std::size_t j = 0; j < kSampledCells; ++j) {
      EXPECT_EQ(sampled_cell(j, unit) / 8, j);
      EXPECT_LT(sampled_cell(j, unit), kCoarseCells);
    }
  }
}

TEST(AdaptiveMeshOracle, ACrowdedCellIsWithinItsToleranceOnEverySchedule) {
  // 256 distinct finest cells of coarse cell 0: hundreds of leaves in one cell.
  Hotspots spots;
  for (std::uint32_t y = 0; y < 32; y += 2) {
    for (std::uint32_t x = 0; x < 32; x += 2) {
      spots.push_back({x, y});
    }
  }
  const auto workload = make(config_for());
  ASSERT_NE(workload, nullptr);
  const Bytes input = input_of(spots);
  const CellReference r = reference_cell(0, spots);
  ASSERT_GT(r.leaves, 700U);
  Dealt dealt(7, 3);
  Claiming claiming(4);
  Serial serial;
  for (worker::Executor* executor :
       {static_cast<worker::Executor*>(&serial), static_cast<worker::Executor*>(&dealt),
        static_cast<worker::Executor*>(&claiming)}) {
    const Bytes output = computed(*workload, 0, input, *executor);
    EXPECT_EQ(count_of(output, 0), r.leaves);
    EXPECT_TRUE(integral_within(output, 0, r));
  }
}

TEST(AdaptiveMeshOracle, TheToleranceBoundsWhatTheWorkloadDoesAndRefusesWhatItDoesNot) {
  const Hotspots spots = crowd({3, 3}, 1, {kLattice, kLattice});
  const auto workload = make(config_for());
  ASSERT_NE(workload, nullptr);
  Serial serial;
  Bytes output = computed(*workload, 0, input_of(spots), serial);
  const CellReference r = reference_cell(0, spots);
  ASSERT_EQ(r.leaves, 16U);
  const double tau = cell_tolerance(r.leaves, r.value);
  EXPECT_GT(tau, 0.0);
  EXPECT_LT(tau, r.value * 1e-14) << "a tolerance of a few units in the last place";
  // The residual the workload actually has, against the tolerance.
  EXPECT_LE(std::fabs(integral_of(output, 0) - r.value), tau);
  // Just inside and just outside, by the specification's own margin: a stored
  // value moves in steps of ulp(c'), so the cases are built from whole ulps.
  const double ulp = std::nextafter(r.value, std::numeric_limits<double>::infinity()) - r.value;
  const auto steps_inside = static_cast<std::size_t>(tau / ulp);
  ASSERT_GE(steps_inside, 1U);
  put_double(output, integral_offset(0), r.value + (static_cast<double>(steps_inside) * ulp));
  EXPECT_TRUE(integral_within(output, 0, r));
  put_double(output, integral_offset(0), r.value + (static_cast<double>(steps_inside + 1) * ulp));
  EXPECT_FALSE(integral_within(output, 0, r));
  put_double(output, integral_offset(0), r.value - (static_cast<double>(steps_inside + 1) * ulp));
  EXPECT_FALSE(integral_within(output, 0, r));
}

TEST(AdaptiveMeshOracle, TheResidualsOfRealRunsStayBelowTheToleranceWithRoomToSpare) {
  const auto workload = make(config_for(9, 8));
  ASSERT_NE(workload, nullptr);
  double worst = 0.0;
  Dealt dealt(7, 5);
  for (std::uint64_t unit = 0; unit < 3; ++unit) {
    const Bytes input = generated(*workload, unit);
    const Hotspots spots = hotspots_of(input);
    const Bytes output = computed(*workload, unit, input, dealt);
    for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
      const CellReference r = reference_cell(cell, spots);
      worst = std::max(worst, std::fabs(integral_of(output, cell) - r.value) /
                                  cell_tolerance(r.leaves, r.value));
    }
  }
  EXPECT_LE(worst, 1.0);
  RecordProperty("worst_residual_over_tolerance", std::to_string(worst));
}

}  // namespace
}  // namespace loadforge::workloads::adaptive_mesh::testing
