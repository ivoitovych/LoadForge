// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the stencil oracle: its windows, its sweep against a whole-grid sweep,
// its tolerance and the plans it refuses, each on its own.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "oracle/verification/oracle/stencil/stencil_evidence.hpp"
#include "verification/oracle/stencil/stencil_oracle.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::verification::oracle::stencil {
namespace {

namespace spec = workloads::stencil;

TEST(StencilLoadDouble, ReadsLittleEndianWhateverTheHostIs) {
  const std::vector<std::byte> bytes = {
      std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0xF0}, std::byte{0x3F}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{0},    std::byte{0},    std::byte{0}, std::byte{0}};
  EXPECT_EQ(load_double(bytes, 0), 1.0);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(load_double(bytes, 1)), 1U);
}

TEST(StencilWindows, AreTheTwoCornersThenTwoThatDependOnTheIndexAlone) {
  std::set<std::pair<std::size_t, std::size_t>> seen;
  for (std::uint64_t index = 0; index < 3000; ++index) {
    const auto origins = window_origins(index);
    EXPECT_EQ(origins[0].row, 0U);
    EXPECT_EQ(origins[0].column, 0U);
    EXPECT_EQ(origins[1].row, spec::kEdge - spec::kRegionEdge);
    EXPECT_EQ(origins[1].column, spec::kEdge - spec::kRegionEdge);
    for (std::size_t window = 2; window < spec::kRegions; ++window) {
      EXPECT_LE(origins[window].row, spec::kEdge - spec::kRegionEdge);
      EXPECT_LE(origins[window].column, spec::kEdge - spec::kRegionEdge);
      seen.emplace(origins[window].row, origins[window].column);
    }
    const auto again = window_origins(index);
    for (std::size_t window = 0; window < spec::kRegions; ++window) {
      EXPECT_EQ(origins[window].row, again[window].row) << "a re-run checks the same cells";
      EXPECT_EQ(origins[window].column, again[window].column);
    }
  }
  EXPECT_GT(seen.size(), 2000U) << "units are spread over the grid";
  // The extremes of the range are reached by some index.
  bool reached_low = false;
  bool reached_high = false;
  for (std::uint64_t index = 0; index < 200000 && !(reached_low && reached_high); ++index) {
    const auto origins = window_origins(index);
    reached_low = reached_low || origins[2].row == 0;
    reached_high = reached_high || origins[2].row == spec::kEdge - spec::kRegionEdge;
  }
  EXPECT_TRUE(reached_low);
  EXPECT_TRUE(reached_high);
}

// The oracle's windowed sweep, against the plainest reading of the
// specification -- the whole grid swept eight times, no window and no clipping
// -- at origins that exercise every clip: each corner, each edge, the middle,
// and windows 8 cells from an edge, where the margin just fits.
TEST(StencilExpectedWindow, IsTheWholeGridSweptEightTimesAtEveryKindOfOrigin) {
  const std::vector<double> input = evidence::generated_grid(evidence::kSeed, 3);
  const std::vector<std::byte> bytes = evidence::bytes_of(input);
  const std::vector<double> swept = evidence::swept_grid(input);
  const std::size_t far = spec::kEdge - spec::kRegionEdge;
  const std::vector<Origin> origins = {
      {0, 0},     {0, far}, {far, 0},           {far, far},         {0, 100},
      {far, 100}, {100, 0}, {100, far},         {100, 100},         {7, 7},
      {8, 8},     {9, 9},   {far - 7, far - 7}, {far - 8, far - 8}, {far - 9, far - 9},
      {1, 200},   {200, 1}};
  for (const Origin origin : origins) {
    const Window window = expected_window(bytes, origin);
    for (std::size_t r = 0; r < spec::kRegionEdge; ++r) {
      for (std::size_t c = 0; c < spec::kRegionEdge; ++c) {
        EXPECT_EQ(window[r * spec::kRegionEdge + c],
                  swept[(origin.row + r) * spec::kEdge + origin.column + c])
            << "origin (" << origin.row << ", " << origin.column << ") cell (" << r << ", " << c
            << ")";
      }
    }
  }
}

TEST(StencilExpectedWindow, KeepsAConstantGridAndTheRing) {
  std::vector<double> grid(spec::kEdge * spec::kEdge, 0.5);
  const std::vector<std::byte> bytes = evidence::bytes_of(grid);
  for (const Origin origin : {Origin{0, 0}, Origin{100, 100}, Origin{240, 240}}) {
    for (const double cell : expected_window(bytes, origin)) {
      EXPECT_EQ(cell, 0.5);
    }
  }
}

TEST(StencilExpectedWindow, SpreadsAnImpulseByTheStatedWeights) {
  // One sweep of a lone cell u = 1 at the middle: the cell becomes 1/2 and its
  // four neighbours 1/8 each. The weights sum to 1 and the impulse is far from
  // the ring, so the mass stays 1 and the window around it holds nearly all.
  std::vector<double> grid(spec::kEdge * spec::kEdge, 0.0);
  grid[128 * spec::kEdge + 128] = 1.0;
  const std::vector<std::byte> bytes = evidence::bytes_of(grid);
  const Window window = expected_window(bytes, Origin{120, 120});
  double mass = 0.0;
  for (const double cell : window) {
    EXPECT_GE(cell, 0.0);
    mass += cell;
  }
  // Nothing reaches the ring and no clipping loses any; the window is 16 cells
  // wide and the impulse's reach after eight sweeps is 17, so the one cell of
  // the reach it leaves out holds what is missing, about 6e-8 of the mass.
  EXPECT_NEAR(mass, 1.0, 2e-7);
  EXPECT_LT(mass, 1.0);
  EXPECT_GT(window[8 * spec::kRegionEdge + 8], window[8 * spec::kRegionEdge + 11])
      << "the peak is the impulse's own cell";
}

TEST(StencilCellTolerance, IsTwoGammaTimesTheLargestCellWithTheSlack) {
  EXPECT_EQ(cell_tolerance(1.0),
            2.0 * spec::gamma(4 * spec::kSteps) * (1.0 + spec::kEvaluationSlack));
  EXPECT_EQ(cell_tolerance(0.25), 0.25 * cell_tolerance(1.0));
  EXPECT_GT(cell_tolerance(0x1p-52), 0.0);
  EXPECT_GT(cell_tolerance(1.0), 6e-15);
  EXPECT_LT(cell_tolerance(1.0), 8e-15);
}

TEST(StencilCreate, GivesTheOracleForEveryTileEdge) {
  for (const std::uint32_t edge : spec::kTileEdges) {
    const workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 5, edge, 0, 0};
    auto created = create(workloads::WorkloadConfig{}, plan);
    ASSERT_TRUE(created.has_value());
    const auto oracle = std::move(created).value();
    EXPECT_EQ(oracle->workload(), "stencil");
    EXPECT_EQ(oracle->contract(), Contract::kBounded);
  }
}

TEST(StencilCreate, RefusesAPlanThatIsNotAUnitOfTheSpecification) {
  const auto refused = [](const workloads::Plan& plan) {
    auto created = create(workloads::WorkloadConfig{}, plan);
    EXPECT_FALSE(created.has_value());
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_FALSE(created.error().detail.empty());
  };
  const workloads::Plan good{spec::kInputBytes, spec::kOutputBytes, 0, 1, 64, 0, 0};
  workloads::Plan plan = good;
  plan.input_bytes = spec::kInputBytes - 1;
  refused(plan);
  plan = good;
  plan.input_bytes = spec::kInputBytes * 2;
  refused(plan);
  plan = good;
  plan.output_bytes = spec::kOutputBytes + 8;
  refused(plan);
  plan = good;
  plan.output_bytes = 0;
  refused(plan);
  for (const std::uint32_t edge : {0U, 1U, 8U, 48U, 256U}) {
    plan = good;
    plan.decomposition = edge;
    refused(plan);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::stencil
