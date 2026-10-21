// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the stencil oracle: scipy's sweep of a generated unit is agreed with,
// a corrupted one is not, and the derived tolerance holds at the boundary the
// specification names -- breached at (1 + epsilon) times T' and held at
// (1 - epsilon) times it, in each of the four windows.

#include "verification/oracle/stencil/stencil_oracle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "oracle/verification/oracle/stencil/stencil_evidence.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::verification::oracle::stencil {
namespace {

namespace spec = workloads::stencil;
using evidence::Unit;

constexpr double kEpsilon = 1.0 / 1048576.0;  // 2^-20, the specification's

std::unique_ptr<Oracle> make_oracle(std::uint32_t tile_edge = spec::kDefaultTileEdge) {
  auto created =
      create(workloads::WorkloadConfig{},
             workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, tile_edge, 0, 0});
  return std::move(created).value();
}

contracts::OracleCase as_case(const Unit& unit) {
  return contracts::OracleCase{unit.index, unit.input, unit.output};
}

Verdict check_unit(const Oracle& oracle, const Unit& unit) {
  return oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
}

std::vector<contracts::OracleCase> real_tool_cases() {
  return {as_case(evidence::real_tool_unit())};
}

// An exponent bit of a cell in each window, and a high mantissa bit and a NaN:
// every one lies in a window whatever the index is, because the corners are
// the first two windows and the others are found from window_origins.
std::vector<contracts::OracleCase> corrupted_cases() {
  const contracts::OracleCase base = as_case(evidence::real_tool_unit());
  std::vector<contracts::OracleCase> cases;
  const auto origins = window_origins(base.index);
  for (const Origin origin : origins) {
    contracts::OracleCase unit = base;
    const std::size_t element = (origin.row + 5) * spec::kEdge + origin.column + 9;
    unit.output[element * 8 + 7] ^= std::byte{0x40};
    cases.push_back(unit);
  }
  contracts::OracleCase mantissa = base;
  mantissa.output[(origins[2].row * spec::kEdge + origins[2].column) * 8 + 6] ^= std::byte{0x10};
  cases.push_back(mantissa);
  contracts::OracleCase nan = base;
  const std::vector<std::byte> nan_bytes = evidence::bytes_of(std::vector<double>(1, std::nan("")));
  std::copy(nan_bytes.begin(), nan_bytes.end(),
            nan.output.begin() + static_cast<std::ptrdiff_t>(
                                     (origins[3].row * spec::kEdge + origins[3].column) * 8));
  cases.push_back(nan);
  return cases;
}

// A grid that is zero except one cell of magnitude 1, far from every window of
// `index`: every window's exact result is exactly zero, and M0 is exactly 1.
std::vector<double> lone_cell_grid(std::uint64_t index) {
  const auto origins = window_origins(index);
  const std::size_t reach = spec::kSteps + 1;
  for (const std::size_t candidate : {std::size_t{60}, std::size_t{128}, std::size_t{200}}) {
    bool clear = true;
    for (const Origin origin : origins) {
      const bool rows =
          candidate + reach > origin.row && candidate < origin.row + spec::kRegionEdge + reach;
      const bool columns = candidate + reach > origin.column &&
                           candidate < origin.column + spec::kRegionEdge + reach;
      clear = clear && !(rows && columns);
    }
    if (clear) {
      std::vector<double> grid(spec::kEdge * spec::kEdge, 0.0);
      grid[candidate * spec::kEdge + candidate] = 1.0;
      return grid;
    }
  }
  return {};
}

// The number of cells compared up to and including the cell (row, column):
// windows in order, each row-major, the cell counted in the first window that
// holds it (windows may overlap, and a cell in two is compared once in each).
std::uint64_t ordinal_of(const std::array<Origin, spec::kRegions>& origins, std::size_t row,
                         std::size_t column) {
  for (std::size_t window = 0; window < origins.size(); ++window) {
    const Origin origin = origins[window];
    if (row >= origin.row && row < origin.row + spec::kRegionEdge && column >= origin.column &&
        column < origin.column + spec::kRegionEdge) {
      return window * spec::kRegionEdge * spec::kRegionEdge +
             (row - origin.row) * spec::kRegionEdge + (column - origin.column) + 1;
    }
  }
  return 0;
}

double expected_limit() {
  return 2.0 * spec::gamma(4 * spec::kSteps) * (1.0 + spec::kEvaluationSlack);
}

TEST(StencilOracleTolerance, ACellOffByMoreThanTheToleranceIsCaughtAndLessIsHeldInEveryWindow) {
  const auto oracle = make_oracle();
  const double limit = expected_limit();
  for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{3}, std::uint64_t{91}}) {
    const std::vector<double> grid = lone_cell_grid(index);
    ASSERT_FALSE(grid.empty()) << "index " << index;
    const auto origins = window_origins(index);
    for (std::size_t window = 0; window < spec::kRegions; ++window) {
      // The first cell of the window, and its last: the ordinal of the failing
      // cell is the number of cells compared, windows in order, row-major.
      for (const std::size_t position :
           {std::size_t{0}, std::size_t{37}, spec::kCellsChecked / spec::kRegions - 1}) {
        const std::size_t row = origins[window].row + position / spec::kRegionEdge;
        const std::size_t column = origins[window].column + position % spec::kRegionEdge;
        const std::size_t element = row * spec::kEdge + column;
        std::vector<double> output(spec::kEdge * spec::kEdge, 0.0);
        output[element] = limit * (1.0 + kEpsilon);
        const Unit breached{index, evidence::bytes_of(grid), evidence::bytes_of(output)};
        const Verdict verdict = check_unit(*oracle, breached);
        ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict))
            << "window " << window << " position " << position;
        const auto& mismatch = std::get<BoundedMismatch>(verdict);
        EXPECT_EQ(mismatch.check, "stencil-cell");
        EXPECT_EQ(mismatch.element, std::optional<std::uint64_t>(element));
        EXPECT_EQ(mismatch.tolerance, limit);
        EXPECT_EQ(mismatch.residual, limit * (1.0 + kEpsilon));
        EXPECT_EQ(mismatch.blocks_compared, ordinal_of(origins, row, column));
        EXPECT_TRUE(is_valid(verdict, Contract::kBounded));

        output[element] = limit * (1.0 - kEpsilon);
        const Unit held{index, evidence::bytes_of(grid), evidence::bytes_of(output)};
        const Verdict agreed = check_unit(*oracle, held);
        ASSERT_TRUE(std::holds_alternative<Agreement>(agreed))
            << "window " << window << " position " << position;
        EXPECT_EQ(std::get<Agreement>(agreed).blocks_compared, spec::kCellsChecked);
      }
    }
  }
}

TEST(StencilOracleTolerance, TheToleranceScalesWithTheLargestCellOfTheWholeGrid) {
  // The largest cell sits outside every window, and still sets the bound.
  const auto oracle = make_oracle();
  std::vector<double> grid = lone_cell_grid(0);
  ASSERT_FALSE(grid.empty());
  for (double& cell : grid) {
    cell *= 0.25;
  }
  std::vector<double> output(spec::kEdge * spec::kEdge, 0.0);
  const double limit = 0.25 * expected_limit();
  output[3 * spec::kEdge + 3] = limit * (1.0 + kEpsilon);
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(
      check_unit(*oracle, Unit{0, evidence::bytes_of(grid), evidence::bytes_of(output)})));
  output[3 * spec::kEdge + 3] = limit * (1.0 - kEpsilon);
  EXPECT_TRUE(std::holds_alternative<Agreement>(
      check_unit(*oracle, Unit{0, evidence::bytes_of(grid), evidence::bytes_of(output)})));
}

TEST(StencilOracleRealTool, ScipysSweepIsAgreedWithAtEveryTileEdge) {
  for (const std::uint32_t tile_edge : spec::kTileEdges) {
    const auto oracle = make_oracle(tile_edge);
    const Verdict verdict = check_unit(*oracle, evidence::real_tool_unit());
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "tile edge " << tile_edge;
    EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kCellsChecked);
  }
}

TEST(StencilOracleRealTool, TheFixtureKeepsTheInputRingBitForBit) {
  const Unit unit = evidence::real_tool_unit();
  const std::vector<double> input = evidence::doubles_of(unit.input);
  const std::vector<double> output = evidence::doubles_of(unit.output);
  for (std::size_t i = 0; i < spec::kEdge; ++i) {
    for (std::size_t j = 0; j < spec::kEdge; ++j) {
      if (spec::is_ring(i, j)) {
        EXPECT_EQ(output[i * spec::kEdge + j], input[i * spec::kEdge + j]);
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::stencil

// The suites are instantiated where they are declared: the macros name them
// unqualified.
namespace loadforge::contracts {
namespace {

namespace under_test = verification::oracle::stencil;

struct StencilSubject {
  static constexpr const char* kWorkload = "stencil";
  static constexpr verification::Contract kContract = verification::Contract::kBounded;
  static std::unique_ptr<verification::Oracle> make() { return under_test::make_oracle(); }
  static std::vector<OracleCase> good() { return under_test::real_tool_cases(); }
  static std::vector<OracleCase> wrong() { return under_test::corrupted_cases(); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Stencil, OracleContract, ::testing::Types<StencilSubject>);

}  // namespace
}  // namespace loadforge::contracts
