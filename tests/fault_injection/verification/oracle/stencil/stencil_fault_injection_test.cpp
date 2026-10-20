// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for the stencil oracle. T5: corruption is introduced into the
// output of a unit scipy swept, and the verdict is asserted for which cell it
// named and how many cells had been compared. T5b: the check is made
// impossible, and the verdict must be NotChecked with nothing compared, never
// an agreement.

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

#include "oracle/verification/oracle/stencil/stencil_evidence.hpp"
#include "verification/oracle/stencil/stencil_oracle.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::verification::oracle::stencil {
namespace {

namespace spec = workloads::stencil;
using evidence::Unit;

const Unit& good_unit() {
  static const Unit unit = evidence::real_tool_unit();
  return unit;
}

std::unique_ptr<Oracle> make_oracle() {
  auto created = create(
      workloads::WorkloadConfig{},
      workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, spec::kDefaultTileEdge, 0, 0});
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

std::size_t element_in(const Origin& origin, std::size_t r, std::size_t c) {
  return (origin.row + r) * spec::kEdge + origin.column + c;
}

BoundedMismatch mismatch_of(const Verdict& verdict) {
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
  return std::get<BoundedMismatch>(verdict);
}

TEST(StencilFaultInjection, TheUncorruptedUnitIsAgreedWith) {
  const Verdict verdict = check_unit(good_unit());
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kCellsChecked);
}

TEST(StencilFaultInjection, AnExponentFlipInAnyWindowNamesTheCellAndCountsTheCellsBefore) {
  const auto origins = window_origins(good_unit().index);
  for (std::size_t window = 0; window < spec::kRegions; ++window) {
    Unit unit = good_unit();
    const std::size_t element = element_in(origins[window], 4, 11);
    flip(unit.output, element, 62);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.check, "stencil-cell");
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(element));
    // The earliest window holding the cell: a window can overlap an earlier one.
    EXPECT_LE(found.blocks_compared, static_cast<std::uint64_t>(window) * 256 + 76);
    EXPECT_GT(found.residual, found.tolerance);
    EXPECT_EQ(found.condition_number, std::nullopt);
  }
}

TEST(StencilFaultInjection, TheFirstFailingCellInTheOrderIsTheOneNamed) {
  const auto origins = window_origins(good_unit().index);
  Unit unit = good_unit();
  // A later window, then an earlier one, then a later cell of the same window.
  flip(unit.output, element_in(origins[1], 0, 0), 62);
  flip(unit.output, element_in(origins[0], 3, 3), 62);
  flip(unit.output, element_in(origins[0], 9, 2), 62);
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(element_in(origins[0], 3, 3)));
  EXPECT_EQ(found.blocks_compared, 3U * 16 + 3 + 1);
}

TEST(StencilFaultInjection, TheRingIsCheckedWhereAWindowCoversIt) {
  Unit unit = good_unit();
  flip(unit.output, 0, 62);  // cell (0, 0), on the ring
  const BoundedMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.element, std::optional<std::uint64_t>(0));
  EXPECT_EQ(found.blocks_compared, 1U);
  unit = good_unit();
  flip(unit.output, spec::kEdge * spec::kEdge - 1, 62);  // cell (n-1, n-1)
  EXPECT_EQ(mismatch_of(check_unit(unit)).blocks_compared, 2U * 256) << "the last cell of window 1";
}

TEST(StencilFaultInjection, AnErrorBelowTheBoundIsUnseenAndAboveItIsCaught) {
  // "An error below T_cell goes unseen" (spec.hpp): about the 48th mantissa
  // bit of a cell of magnitude 1. The lowest bits raise no false alarm; a flip
  // eight bits higher is above the bound for any cell of magnitude 1/4 or more.
  const auto origins = window_origins(good_unit().index);
  const std::vector<double> cells = evidence::doubles_of(good_unit().output);
  std::size_t element = element_in(origins[0], 0, 0);
  for (std::size_t at = 0; at < 256 && std::fabs(cells[element]) <= 0.25; ++at) {
    element = element_in(origins[0], at / 16, at % 16);
  }
  ASSERT_GT(std::fabs(cells[element]), 0.25) << "a cell of window 0 large enough to flip";
  for (const unsigned bit : {0U, 1U, 2U}) {
    Unit unit = good_unit();
    flip(unit.output, element, bit);
    EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(unit))) << "bit " << bit;
  }
  for (const unsigned bit : {10U, 30U, 51U}) {
    Unit unit = good_unit();
    flip(unit.output, element, bit);
    EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check_unit(unit))) << "bit " << bit;
  }
}

TEST(StencilFaultInjection, ACellOutsideEveryWindowIsNotExamined) {
  // The bound does not claim cells outside the four windows are checked.
  const auto origins = window_origins(good_unit().index);
  const auto inside_a_window = [&](std::size_t row, std::size_t column) {
    return std::any_of(origins.begin(), origins.end(), [&](const Origin& origin) {
      return row >= origin.row && row < origin.row + spec::kRegionEdge && column >= origin.column &&
             column < origin.column + spec::kRegionEdge;
    });
  };
  std::size_t row = 100;
  while (inside_a_window(row, 100)) {
    ++row;
  }
  Unit unit = good_unit();
  flip(unit.output, row * spec::kEdge + 100, 62);
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(unit)));
}

TEST(StencilFaultInjection, ANaNOrInfinityInACellFailsWithANonFiniteResidual) {
  const auto origins = window_origins(good_unit().index);
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    Unit unit = good_unit();
    const std::size_t element = element_in(origins[1], 2, 2);
    put(unit.output, element, bad);
    const BoundedMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.element, std::optional<std::uint64_t>(element));
    EXPECT_TRUE(std::isnan(found.residual) || std::isinf(found.residual));
    EXPECT_TRUE(std::isfinite(found.tolerance));
    EXPECT_GT(found.tolerance, 0.0);
  }
}

TEST(StencilFaultInjection, ACorruptedInputIsJudgedByTheOutputItDidNotProduce) {
  // The oracle judges the output against the input it is given (oracle.hpp).
  // One cell of the INPUT inside a window's reach moves, the output stays: the
  // verdict is a mismatch of the output, and it is the runner's bracket that
  // says the input was the side that changed.
  const auto origins = window_origins(good_unit().index);
  Unit unit = good_unit();
  flip(unit.input, element_in(origins[0], 4, 4), 51);
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check_unit(unit)));
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

TEST(StencilNotPerformed, EvidenceOfTheWrongSizeIsNotChecked) {
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

TEST(StencilNotPerformed, AnInputCellThatIsNotFiniteIsNotChecked) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()}) {
    for (const std::size_t cell :
         {std::size_t{0}, std::size_t{30000}, spec::kEdge * spec::kEdge - 1}) {
      Unit unit = good_unit();
      put(unit.input, cell, bad);
      expect_not_checked(check_unit(unit));
    }
  }
}

TEST(StencilNotPerformed, AnInputCellAboveOneInMagnitudeIsNotChecked) {
  for (const double bad : {1.0000000000000002, -1.0000000000000002, 2.0, -1.0e10}) {
    Unit unit = good_unit();
    put(unit.input, 4321, bad);
    expect_not_checked(check_unit(unit));
  }
  // One is the largest magnitude the tolerance covers; -1 is generated.
  Unit edge = good_unit();
  put(edge.input, 4321, -1.0);
  EXPECT_FALSE(std::holds_alternative<NotChecked>(check_unit(edge)));
}

TEST(StencilNotPerformed, AnAllZeroInputIsNotCheckedEvenWithAnAllZeroOutput) {
  Unit unit = good_unit();
  std::fill(unit.input.begin(), unit.input.end(), std::byte{0});
  expect_not_checked(check_unit(unit));
  std::fill(unit.output.begin(), unit.output.end(), std::byte{0});
  expect_not_checked(check_unit(unit));
}

TEST(StencilNotPerformed, ANaNInTheOutputIsAMismatchNotAnUnrunCheck) {
  Unit unit = good_unit();
  put(unit.output, 0, std::numeric_limits<double>::quiet_NaN());
  EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(check_unit(unit)));
}

}  // namespace
}  // namespace loadforge::verification::oracle::stencil
