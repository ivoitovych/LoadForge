// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/verification/oracle/dense_linear: the factory's refusals one at a
// time, the grid, the sample's placement and its dependence on the index
// alone, and the verdict's every field -- what NotChecked says, which failure
// a BoundedMismatch names when several are found, the condition number when
// it exists and its absence when it does not.

#include "verification/oracle.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "unit/verification/oracle/dense_linear/units.hpp"
#include "verification/oracle/dense_linear/dense_linear.hpp"
#include "verification/residual/residual.hpp"

namespace loadforge::verification::oracle::dense_linear {
namespace {

using units::kEntries;
using units::kN;
namespace spec = workloads::dense_linear;

std::unique_ptr<Oracle> make(std::size_t grid = kDefaultGrid) {
  core::Result<std::unique_ptr<Oracle>, OracleError> made =
      make_oracle(workloads::WorkloadConfig{}, units::plan(), grid);
  EXPECT_TRUE(made.has_value());
  return std::move(made).value();
}

Verdict check(const Oracle& oracle, const units::Unit& unit) {
  const units::Case evidence = units::to_case(unit);
  return oracle.check(UnitEvidence{evidence.index, evidence.input, evidence.output});
}

/// A unit whose only nonzero entries are small dyadic ones, so every product
/// and partial sum is exact and the oracle's c' is exact: A = 1/2 everywhere,
/// B = 1/4 everywhere, C = 32 everywhere.
units::Unit flat_unit(std::uint64_t index) {
  units::Unit unit{index, std::vector<double>(kEntries, 0.5), std::vector<double>(kEntries, 0.25),
                   std::vector<double>(kEntries, 32.0)};
  return unit;
}

double flat_tolerance() { return residual::tolerance(residual::dot_model(kN), 32.0).value(); }

TEST(DenseLinearOracleFactory, AcceptsEveryPanelDepthAndEveryGrid) {
  for (const std::uint32_t kc : spec::kPanelDepths) {
    for (const std::uint32_t configured : {0U, kc}) {
      workloads::WorkloadConfig config;
      config.decomposition = configured;
      const auto made = make_oracle(config, units::plan(kc));
      ASSERT_TRUE(made.has_value()) << kc << " " << configured;
      EXPECT_EQ(made.value()->workload(), "dense_linear");
      EXPECT_EQ(made.value()->contract(), Contract::kBounded);
    }
  }
  for (std::size_t grid = 1; grid <= kN; grid *= 2) {
    EXPECT_TRUE(make_oracle({}, units::plan(), grid).has_value()) << grid;
  }
}

TEST(DenseLinearOracleFactory, RefusesEachMismatchAlone) {
  struct Case {
    workloads::WorkloadConfig config;
    workloads::Plan plan;
    std::size_t grid;
    std::string detail;
  };
  workloads::Plan short_input = units::plan();
  short_input.input_bytes -= 8;
  workloads::Plan long_output = units::plan();
  long_output.output_bytes += 8;
  workloads::WorkloadConfig other_depth;
  other_depth.decomposition = 128;
  const std::vector<Case> cases{
      {{},
       short_input,
       kDefaultGrid,
       "the plan's unit is 1048568 bytes in and 524288 out; a dense_linear unit is 1048576 and "
       "524288"},
      {{},
       long_output,
       kDefaultGrid,
       "the plan's unit is 1048576 bytes in and 524296 out; a dense_linear unit is 1048576 and "
       "524288"},
      {{}, units::plan(48), kDefaultGrid, "panel depth 48 is not one the specification has"},
      {{}, units::plan(0), kDefaultGrid, "panel depth 0 is not one the specification has"},
      {other_depth, units::plan(64), kDefaultGrid,
       "the configuration asks for panel depth 128 and the plan resolved 64"},
      {{}, units::plan(), 0, "a sample grid of 0 does not divide the output into whole tiles"},
      {{}, units::plan(), 3, "a sample grid of 3 does not divide the output into whole tiles"},
      {{}, units::plan(), 512, "a sample grid of 512 does not divide the output into whole tiles"},
  };
  for (const Case& entry : cases) {
    const auto made = make_oracle(entry.config, entry.plan, entry.grid);
    ASSERT_FALSE(made.has_value()) << entry.detail;
    EXPECT_EQ(made.error().fault, OracleFault::kConfiguration);
    EXPECT_EQ(made.error().detail, entry.detail);
  }
}

TEST(DenseLinearOracleGrid, PowersOfTwoUpToTheOrder) {
  std::set<std::size_t> valid;
  for (std::size_t grid = 0; grid <= 2 * kN; ++grid) {
    if (is_grid(grid)) {
      valid.insert(grid);
    }
  }
  EXPECT_EQ(valid, (std::set<std::size_t>{1, 2, 4, 8, 16, 32, 64, 128, 256}));
}

TEST(DenseLinearOracleSample, OneEntryInEachTileDependingOnTheIndexAlone) {
  for (const std::size_t grid : {std::size_t{1}, std::size_t{8}, std::size_t{32}}) {
    const std::size_t edge = kN / grid;
    for (const std::uint64_t index : {std::uint64_t{0}, std::uint64_t{1}, ~std::uint64_t{0}}) {
      std::set<std::size_t> seen;
      for (std::size_t tile = 0; tile < grid * grid; ++tile) {
        const Entry entry = sample_entry(index, grid, tile);
        EXPECT_EQ(entry.row / edge, tile / grid);
        EXPECT_EQ(entry.column / edge, tile % grid);
        EXPECT_EQ(sample_entry(index, grid, tile).row, entry.row) << "a pure function";
        EXPECT_EQ(sample_entry(index, grid, tile).column, entry.column) << "a pure function";
        seen.insert(entry.row * kN + entry.column);
      }
      EXPECT_EQ(seen.size(), grid * grid);
    }
  }
}

TEST(DenseLinearOracleSample, EveryGridOfTheOrderIsEveryEntry) {
  for (std::size_t tile = 0; tile < kEntries; tile += 4099) {
    const Entry entry = sample_entry(12345, kN, tile);
    EXPECT_EQ(entry.row * kN + entry.column, tile);
  }
}

TEST(DenseLinearOracleSample, ReachesEveryOffsetWithinATileAcrossIndices) {
  // Both coordinates of the drawn offset take all 32 values within a tile of
  // the default grid over a few hundred units: the draw is not stuck on a
  // corner, and rows and columns come from different bits.
  std::set<std::size_t> rows;
  std::set<std::size_t> columns;
  std::size_t same = 0;
  for (std::uint64_t index = 0; index < 400; ++index) {
    const Entry entry = sample_entry(index, kDefaultGrid, 9);  // tile (1, 1)
    rows.insert(entry.row - 32);
    columns.insert(entry.column - 32);
    same += entry.row == entry.column ? 1 : 0;
  }
  EXPECT_EQ(rows.size(), 32U);
  EXPECT_EQ(columns.size(), 32U);
  EXPECT_LT(same, 40U) << "rows and columns drawn from the same bits";
  EXPECT_NE(sample_entry(0, kDefaultGrid, 0).row * kN + sample_entry(0, kDefaultGrid, 0).column,
            sample_entry(1, kDefaultGrid, 0).row * kN + sample_entry(1, kDefaultGrid, 0).column);
}

TEST(DenseLinearOracleVerdict, AgreementCountsEveryTile) {
  for (const std::size_t grid : {std::size_t{1}, std::size_t{8}, std::size_t{16}}) {
    const Verdict verdict = check(*make(grid), flat_unit(4));
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
    EXPECT_EQ(blocks_of(verdict), grid * grid);
  }
}

TEST(DenseLinearOracleVerdict, NotCheckedSaysWhichSizeIsWrong) {
  const auto oracle = make();
  units::Case evidence = units::to_case(flat_unit(0));
  evidence.input.resize(100);
  Verdict verdict = oracle->check(UnitEvidence{0, evidence.input, evidence.output});
  ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
  EXPECT_EQ(std::get<NotChecked>(verdict).what, "dense_linear unit");
  EXPECT_EQ(std::get<NotChecked>(verdict).why, "the input is 100 bytes, not 1048576");
  EXPECT_EQ(std::get<NotChecked>(verdict).blocks_compared, 0U);

  evidence = units::to_case(flat_unit(0));
  evidence.output.resize(7);
  verdict = oracle->check(UnitEvidence{0, evidence.input, evidence.output});
  ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
  EXPECT_EQ(std::get<NotChecked>(verdict).why, "the output is 7 bytes, not 524288");
}

TEST(DenseLinearOracleVerdict, NotCheckedNamesTheInputEntryAndItsValue) {
  const auto oracle = make();
  struct Case {
    bool in_a;
    std::size_t row;
    std::size_t column;
    double value;
    std::string why;
  };
  const double below = std::nextafter(-1.0, -2.0);
  const std::vector<Case> cases{
      {true, 3, 4, 1.0, "input A[3][4] is 1, outside [-1, 1): the tolerance cannot be derived"},
      {false, 255, 0, 1.5,
       "input B[255][0] is 1.5, outside [-1, 1): the tolerance cannot be derived"},
      {true, 0, 0, below,
       "input A[0][0] is -1.0000000000000002, outside [-1, 1): the tolerance cannot be derived"},
      {false, 7, 9, std::numeric_limits<double>::quiet_NaN(),
       "input B[7][9] is nan, outside [-1, 1): the tolerance cannot be derived"},
      {true, 255, 255, -std::numeric_limits<double>::infinity(),
       "input A[255][255] is -inf, outside [-1, 1): the tolerance cannot be derived"},
  };
  for (const Case& entry : cases) {
    units::Unit unit = flat_unit(2);
    (entry.in_a ? unit.a : unit.b)[entry.row * kN + entry.column] = entry.value;
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << entry.why;
    EXPECT_EQ(std::get<NotChecked>(verdict).why, entry.why);
    EXPECT_EQ(std::get<NotChecked>(verdict).blocks_compared, 0U);
  }
}

TEST(DenseLinearOracleVerdict, MinusOneIsInsideTheRangeTheGeneratorDraws) {
  // entry_value(0) = -1 exactly: a correctly generated entry. A unit of -1s
  // and 1/2s is checked, not refused.
  const units::Unit unit{0, std::vector<double>(kEntries, -1.0), std::vector<double>(kEntries, 0.5),
                         std::vector<double>(kEntries, -128.0)};
  EXPECT_TRUE(std::holds_alternative<Agreement>(check(*make(), unit)));
  EXPECT_EQ(spec::entry_value(0), -1.0);
}

TEST(DenseLinearOracleVerdict, AMismatchCarriesItsMeasurementAndItsEntry) {
  units::Unit unit = flat_unit(5);
  const Entry entry = sample_entry(5, kDefaultGrid, 10);
  unit.c[entry.row * kN + entry.column] = 32.5;
  const Verdict verdict = check(*make(), unit);
  ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  const auto& mismatch = std::get<BoundedMismatch>(verdict);
  EXPECT_EQ(mismatch.check, "componentwise |C - AB| on sampled entries");
  EXPECT_EQ(mismatch.residual, 0.5);
  EXPECT_EQ(mismatch.tolerance, flat_tolerance());
  EXPECT_EQ(mismatch.element, entry.row * kN + entry.column);
  ASSERT_TRUE(mismatch.condition_number.has_value());
  EXPECT_EQ(*mismatch.condition_number, 1.0) << "S^ / |c'| = 32 / 32";
  EXPECT_EQ(mismatch.blocks_compared, 64U);
  EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
}

TEST(DenseLinearOracleVerdict, NoConditionNumberWhereTheProductIsZero) {
  // Row i of A is zero: c' = 0, S^ = 0, and S^ / |c'| does not exist.
  units::Unit unit = flat_unit(6);
  const Entry entry = sample_entry(6, kDefaultGrid, 0);
  for (std::size_t k = 0; k < kN; ++k) {
    unit.a[entry.row * kN + k] = 0.0;
    unit.c[entry.row * kN + k] = 0.0;
  }
  unit.c[entry.row * kN + entry.column] = 0x1p-900;
  const Verdict verdict = check(*make(), unit);
  ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  EXPECT_FALSE(std::get<BoundedMismatch>(verdict).condition_number.has_value());
  EXPECT_EQ(std::get<BoundedMismatch>(verdict).residual, 0x1p-900);
  EXPECT_EQ(std::get<BoundedMismatch>(verdict).tolerance, (4.0 * 256.0 + 1.0) * 0x1p-1074);
}

std::size_t element_of(const Entry& entry) { return entry.row * kN + entry.column; }

TEST(DenseLinearOracleVerdict, TheWorstFailureIsNamed) {
  const auto oracle = make();
  const std::size_t first = element_of(sample_entry(7, kDefaultGrid, 3));
  const std::size_t second = element_of(sample_entry(7, kDefaultGrid, 40));
  const std::size_t third = element_of(sample_entry(7, kDefaultGrid, 63));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  struct Case {
    std::string name;
    double at_first;
    double at_second;
    double at_third;
    std::size_t expected;
  };
  const std::vector<Case> cases{
      {"greater ratio later", 33.0, 40.0, 32.0, second},
      {"greater ratio first", 40.0, 33.0, 32.0, first},
      {"a tie keeps the first", 33.0, 31.0, 32.0, first},
      {"a NaN beats any number", 1e300, nan, 32.0, second},
      {"the greatest of three", 32.0, 1.0, -1000.0, third},
      // 1e300 / T^ and 1e308 / T^ both overflow to infinity: equal ratios, as
      // ratio() of the records would print them, so the first is kept.
      {"ratios that overflow are equal", 32.0, 1e300, -1e308, second},
      {"the first NaN is kept", nan, 1e300, nan, first},
      {"a later NaN beats an earlier number", 1e300, 32.0, nan, third},
  };
  for (const Case& entry : cases) {
    units::Unit unit = flat_unit(7);
    unit.c[first] = entry.at_first;
    unit.c[second] = entry.at_second;
    unit.c[third] = entry.at_third;
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict)) << entry.name;
    EXPECT_EQ(std::get<BoundedMismatch>(verdict).element, entry.expected) << entry.name;
    EXPECT_TRUE(is_valid(verdict, Contract::kBounded)) << entry.name;
  }
}

TEST(DenseLinearOracleVerdict, AnEntryOutsideTheSampleIsNotJudged) {
  // The honest limit of sampling, stated: an entry no tile sampled is not
  // judged by this unit's check, at the default grid; grid = n judges it.
  units::Unit unit = flat_unit(8);
  std::set<std::size_t> sampled;
  for (std::size_t tile = 0; tile < kDefaultGrid * kDefaultGrid; ++tile) {
    sampled.insert(element_of(sample_entry(8, kDefaultGrid, tile)));
  }
  std::size_t unsampled = 0;
  while (sampled.contains(unsampled)) {
    ++unsampled;
  }
  unit.c[unsampled] = 1e9;
  EXPECT_TRUE(std::holds_alternative<Agreement>(check(*make(), unit)));
  const Verdict full = check(*make(kN), unit);
  ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(full));
  EXPECT_EQ(std::get<BoundedMismatch>(full).element, unsampled);
  EXPECT_EQ(std::get<BoundedMismatch>(full).blocks_compared, kEntries);
}

/// Tile 0's 4 x 8 block (rows r..r+3, columns c..c+7) that avoids unit
/// `index`'s sampled entry of tile 0, found from the sample alone. Returns the
/// block's top-left corner.
Entry block_avoiding_the_sample(std::uint64_t index) {
  const Entry sampled = sample_entry(index, kDefaultGrid, 0);
  const std::size_t tile = kN / kDefaultGrid;
  for (std::size_t row = 0; row + 4 <= tile; row += 4) {
    for (std::size_t column = 0; column + 8 <= tile; column += 8) {
      const bool inside = sampled.row >= row && sampled.row < row + 4 && sampled.column >= column &&
                          sampled.column < column + 8;
      if (!inside) {
        return Entry{row, column};
      }
    }
  }
  return Entry{};
}

void corrupt_block(units::Unit& unit, const Entry corner) {
  for (std::size_t r = 0; r < 4; ++r) {
    for (std::size_t c = 0; c < 8; ++c) {
      unit.c[(corner.row + r) * kN + corner.column + c] += 1.0;
    }
  }
}

TEST(DenseLinearOracleVerdict, DocumentedLimitationADamagedBlockAvoidingTheSampleIsAgreement) {
  // NOT a defect: review W1-02. One entry of a 32 x 32 tile is checked, so a
  // 4 x 8 block (32 of 1024 entries) wrong by 1.0, far beyond tolerance, is
  // missed by this unit when it avoids the sampled entry. Agreement says 64
  // sampled entries passed, not that the output is right.
  units::Unit unit = flat_unit(3);
  corrupt_block(unit, block_avoiding_the_sample(3));
  const Verdict verdict = check(*make(), unit);
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(blocks_of(verdict), 64U);
}

TEST(DenseLinearOracleVerdict, TheSameDamagedBlockIsCaughtAtFullGrid) {
  units::Unit unit = flat_unit(3);
  const Entry corner = block_avoiding_the_sample(3);
  corrupt_block(unit, corner);
  const Verdict verdict = check(*make(kN), unit);
  ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
  const auto& mismatch = std::get<BoundedMismatch>(verdict);
  ASSERT_TRUE(mismatch.element.has_value());
  const std::size_t row = *mismatch.element / kN;
  const std::size_t column = *mismatch.element % kN;
  EXPECT_TRUE(row >= corner.row && row < corner.row + 4 && column >= corner.column &&
              column < corner.column + 8);
}

TEST(DenseLinearOracleVerdict, AgreementSaysHowManyEntriesPassed) {
  EXPECT_EQ(blocks_of(check(*make(), flat_unit(6))), 64U) << "sampled entries only";
  EXPECT_EQ(blocks_of(check(*make(kN), flat_unit(6))), 65536U) << "every entry";
}

}  // namespace
}  // namespace loadforge::verification::oracle::dense_linear
