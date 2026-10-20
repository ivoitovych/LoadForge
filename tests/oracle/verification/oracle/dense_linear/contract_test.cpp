// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for src/verification/oracle/dense_linear: OracleContract against the
// oracle (ADR 0003: the provider runs its contract's suite), and the oracle
// against outputs known right by exact arithmetic and against every order a
// correct kernel may sum in.
//
// The good units are products whose exact value is known: the specification's
// own inputs (dyadic: multiples of 2^-52) with C = AB computed in 128-bit
// integers and rounded once, and two units of integer-valued and small
// dyadic entries whose products are exact in doubles. Never kernel output
// (verification.md §2). The wrong units corrupt entries the index samples --
// or every entry -- so each is caught whatever the default grid draws.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "unit/verification/oracle/dense_linear/units.hpp"
#include "unit/verification/residual/orders.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/dense_linear/dense_linear.hpp"

namespace loadforge::verification::oracle::dense_linear {
namespace {

using units::kEntries;
using units::kN;

std::unique_ptr<Oracle> make(std::size_t grid = kDefaultGrid) {
  core::Result<std::unique_ptr<Oracle>, OracleError> made =
      make_oracle(workloads::WorkloadConfig{}, units::plan(), grid);
  EXPECT_TRUE(made.has_value());
  return std::move(made).value();
}

contracts::OracleCase case_of(const units::Unit& unit) {
  units::Case bytes = units::to_case(unit);
  return contracts::OracleCase{bytes.index, std::move(bytes.input), std::move(bytes.output)};
}

/// The spec units, built once: an exact 256^3 product is not free.
const std::vector<units::Unit>& spec_units() {
  static const std::vector<units::Unit> built{units::spec_unit(0), units::spec_unit(1),
                                              units::spec_unit(1000003)};
  return built;
}

/// Integer-valued: entries -1 and 0 (the only integers in [-1, 1)), so C is
/// integer-valued, and exact in any order.
units::Unit integer_unit() {
  units::Unit unit{17, std::vector<double>(kEntries), std::vector<double>(kEntries), {}};
  for (std::size_t e = 0; e < kEntries; ++e) {
    unit.a[e] = (e * 7 + 3) % 5 == 0 ? -1.0 : 0.0;
    unit.b[e] = (e * 11 + 1) % 3 == 0 ? 0.0 : -1.0;
  }
  unit.c = units::exact_product(unit.a, unit.b);
  return unit;
}

/// Small dyadic entries, k / 8 for k in [-8, 8), so every product is a
/// multiple of 2^-6 and every partial sum is exact.
units::Unit dyadic_unit() {
  units::Unit unit{42, std::vector<double>(kEntries), std::vector<double>(kEntries), {}};
  for (std::size_t e = 0; e < kEntries; ++e) {
    unit.a[e] = static_cast<double>(static_cast<int>((e * 13) % 16) - 8) / 8.0;
    unit.b[e] = static_cast<double>(static_cast<int>((e * 5 + 2) % 16) - 8) / 8.0;
  }
  unit.c = units::exact_product(unit.a, unit.b);
  return unit;
}

double flip(double value, int bit) {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << bit));
}

/// `unit` with `change` applied to the entry its default-grid tile `tile`
/// samples.
template <typename Change>
contracts::OracleCase corrupt_sampled(units::Unit unit, std::size_t tile, Change change) {
  const Entry entry = sample_entry(unit.index, kDefaultGrid, tile);
  double& value = unit.c[entry.row * kN + entry.column];
  value = change(value);
  return case_of(unit);
}

struct DenseLinearSubject {
  static constexpr const char* kWorkload = "dense_linear";
  static constexpr Contract kContract = Contract::kBounded;
  static std::unique_ptr<Oracle> make() { return dense_linear::make(); }
  static std::vector<contracts::OracleCase> good() {
    std::vector<contracts::OracleCase> cases;
    for (const units::Unit& unit : spec_units()) {
      cases.push_back(case_of(unit));
    }
    cases.push_back(case_of(integer_unit()));
    cases.push_back(case_of(dyadic_unit()));
    return cases;
  }
  static std::vector<contracts::OracleCase> wrong() {
    const units::Unit& unit = spec_units()[1];
    std::vector<contracts::OracleCase> cases;
    // One bit of one sampled entry: the top exponent bit, the sign, the
    // highest significand bit.
    cases.push_back(corrupt_sampled(unit, 0, [](double v) { return flip(v, 62); }));
    cases.push_back(corrupt_sampled(unit, 21, [](double v) { return flip(v, 63); }));
    cases.push_back(corrupt_sampled(unit, 63, [](double v) { return flip(v, 51); }));
    // A NaN and an infinity where a number belongs.
    cases.push_back(corrupt_sampled(
        unit, 30, [](double /*v*/) { return std::numeric_limits<double>::quiet_NaN(); }));
    cases.push_back(corrupt_sampled(
        unit, 31, [](double /*v*/) { return -std::numeric_limits<double>::infinity(); }));
    // A whole tile lost: the 32 x 32 block of C at tile (2, 5) left zero.
    units::Unit tile_lost = unit;
    for (std::size_t i = 64; i < 96; ++i) {
      for (std::size_t j = 160; j < 192; ++j) {
        tile_lost.c[i * kN + j] = 0.0;
      }
    }
    cases.push_back(case_of(tile_lost));
    // Wrong algorithms, not wrong bits: C^T, and C = BA.
    units::Unit transposed = unit;
    for (std::size_t i = 0; i < kN; ++i) {
      for (std::size_t j = 0; j < kN; ++j) {
        transposed.c[i * kN + j] = unit.c[j * kN + i];
      }
    }
    cases.push_back(case_of(transposed));
    units::Unit swapped = unit;
    swapped.c = units::exact_product(unit.b, unit.a);
    cases.push_back(case_of(swapped));
    // Every entry off by one part in 2^30: far inside a float's precision,
    // far outside the derived tolerance.
    units::Unit scaled = unit;
    for (double& value : scaled.c) {
      value *= 1.0 + 0x1p-30;
    }
    cases.push_back(case_of(scaled));
    return cases;
  }
};

TEST(DenseLinearOracleTruth, TheSpecUnitsExactProductIsAgreedWithAtEveryEntry) {
  const units::Unit& unit = spec_units()[2];
  const contracts::OracleCase evidence = case_of(unit);
  const Verdict verdict =
      make(kN)->check(UnitEvidence{unit.index, evidence.input, evidence.output});
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(blocks_of(verdict), kEntries);
}

TEST(DenseLinearOracleTruth, EveryOrderAKernelMaySumInIsAgreedWith) {
  // Each order a correct kernel may use, written into the entries the default
  // grid samples (the rest hold the exact product, which the check does not
  // read); then one AVX2-like order -- panels of 64, four fused lanes -- into
  // every entry, judged at every entry.
  const units::Unit& base = spec_units()[0];
  const auto sampled = make();
  for (const orders::Order& order : orders::all(workloads::dense_linear::kPanelDepths)) {
    units::Unit unit = base;
    for (std::size_t tile = 0; tile < kDefaultGrid * kDefaultGrid; ++tile) {
      const Entry entry = sample_entry(unit.index, kDefaultGrid, tile);
      unit.c[entry.row * kN + entry.column] = orders::evaluate(
          order, units::row_of(base, entry.row), units::column_of(base, entry.column));
    }
    const contracts::OracleCase evidence = case_of(unit);
    EXPECT_TRUE(std::holds_alternative<Agreement>(
        sampled->check(UnitEvidence{unit.index, evidence.input, evidence.output})))
        << orders::name_of(order);
  }
  units::Unit unit = base;
  unit.c = units::product_in(base, orders::Order{orders::Shape::kPanel, 64, 4, true});
  const contracts::OracleCase evidence = case_of(unit);
  EXPECT_TRUE(std::holds_alternative<Agreement>(
      make(kN)->check(UnitEvidence{unit.index, evidence.input, evidence.output})));
}

}  // namespace
}  // namespace loadforge::verification::oracle::dense_linear

// The suite's own namespace, which the instantiation must name it from.
namespace loadforge::contracts {
namespace {
using DenseLinearOracles = ::testing::Types<verification::oracle::dense_linear::DenseLinearSubject>;
}  // namespace
INSTANTIATE_TYPED_TEST_SUITE_P(DenseLinear, OracleContract, DenseLinearOracles);
// The registry's suite shares the header and is the integration stream's to
// run against the registry it provides; this module provides no registry.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace loadforge::contracts
