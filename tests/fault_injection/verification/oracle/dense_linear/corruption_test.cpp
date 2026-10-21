// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for src/verification/oracle/dense_linear.
//
// T5: corruption of a correct output, and the classification the oracle
// gives it -- a BoundedMismatch naming the corrupted entry, never an
// ExactMismatch, never an Agreement for a corruption the bound does not
// cover. Every bit of every sampled entry is flipped in turn; NaNs and
// infinities are written in; the tolerance is approached from both sides
// (M2b exit criterion 3); and the record built from the verdict is a
// NumericalVerificationFailure with no bit fields at all (the M2b P4 row).
//
// T5b: evidence the oracle cannot judge -- an input entry outside the range
// the derivation needs, evidence of the wrong size -- is NotChecked with
// nothing compared, and the record it becomes is VerificationNotPerformed,
// never a pass and never a wrong result.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "unit/verification/oracle/dense_linear/units.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/dense_linear/dense_linear.hpp"
#include "verification/record.hpp"
#include "verification/residual/residual.hpp"

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

const units::Unit& spec_unit() {
  static const units::Unit built = units::spec_unit(77);
  return built;
}

Verdict check(const Oracle& oracle, const units::Unit& unit) {
  const units::Case evidence = units::to_case(unit);
  return oracle.check(UnitEvidence{evidence.index, evidence.input, evidence.output});
}

double flip(double value, int bit) {
  return std::bit_cast<double>(std::bit_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << bit));
}

/// The record a runner makes of a bounded mismatch: the verdict's fields,
/// unchanged, beside provenance the runner knows.
NumericalVerificationFailure record_of(const BoundedMismatch& mismatch, std::uint64_t index) {
  Provenance provenance;
  provenance.iteration = index;
  provenance.workload = std::string(kWorkload);
  return NumericalVerificationFailure{provenance,
                                      mismatch.check,
                                      mismatch.residual,
                                      mismatch.tolerance,
                                      mismatch.condition_number,
                                      mismatch.element};
}

TEST(DenseLinearOracleCorruption, EveryBitOfSampledEntriesIsClassified) {
  // A correct output, then bits of sampled entries flipped one at a time:
  // all 64 bits of the entries of eight tiles, and the sign and exponent bits
  // of every other tile's entry -- 1,184 corruptions, each judged. The sign
  // and every exponent bit are always caught, and so is every significand
  // bit whose flip moves the entry by more than its tolerance plus the honest
  // residual; one that moves it by less than the tolerance minus that is
  // within the bound and agreed with -- the limit of a bounded check, stated.
  const units::Unit& base = spec_unit();
  const auto oracle = make();
  units::Case evidence = units::to_case(base);
  std::size_t caught = 0;
  std::size_t covered = 0;
  for (std::size_t tile = 0; tile < kDefaultGrid * kDefaultGrid; ++tile) {
    const Entry entry = sample_entry(base.index, kDefaultGrid, tile);
    const std::size_t element = entry.row * kN + entry.column;
    const residual::Accumulation reference =
        residual::dot(units::row_of(base, entry.row), units::column_of(base, entry.column));
    const double t = residual::tolerance(residual::dot_model(kN), reference.magnitude).value();
    const double honest = std::abs(base.c[element] - reference.value);
    for (int bit = tile % 8 == 0 ? 0 : 52; bit < 64; ++bit) {
      // The flip, made in the output's bytes: bit b of a little-endian double
      // is bit b % 8 of byte b / 8.
      std::byte& byte = evidence.output[element * 8 + static_cast<std::size_t>(bit / 8)];
      const auto mask = static_cast<std::byte>(1U << static_cast<unsigned>(bit % 8));
      byte ^= mask;
      const double moved = std::abs(flip(base.c[element], bit) - base.c[element]);
      const Verdict verdict =
          oracle->check(UnitEvidence{evidence.index, evidence.input, evidence.output});
      byte ^= mask;
      ASSERT_FALSE(std::holds_alternative<ExactMismatch>(verdict));
      ASSERT_FALSE(std::holds_alternative<NotChecked>(verdict));
      if (const auto* mismatch = std::get_if<BoundedMismatch>(&verdict)) {
        ++caught;
        EXPECT_EQ(mismatch->element, element) << "tile " << tile << " bit " << bit;
        EXPECT_EQ(mismatch->tolerance, t);
        EXPECT_EQ(mismatch->blocks_compared, 64U);
        EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
      } else {
        ++covered;
      }
      if (bit >= 52 || moved > t + honest) {
        EXPECT_TRUE(std::holds_alternative<BoundedMismatch>(verdict))
            << "tile " << tile << " bit " << bit;
      }
      if (bit < 52 && moved < t - honest) {
        EXPECT_TRUE(std::holds_alternative<Agreement>(verdict))
            << "tile " << tile << " bit " << bit;
      }
    }
  }
  EXPECT_EQ(caught + covered, 8U * 64U + 56U * 12U);
  std::cout << "single-bit flips of sampled entries: " << caught << " caught, " << covered
            << " within the bound\n";
}

TEST(DenseLinearOracleCorruption, ANaNOrAnInfinityInTheOutputIsAMismatchNamingIt) {
  const auto oracle = make();
  const Entry entry = sample_entry(spec_unit().index, kDefaultGrid, 45);
  const std::size_t element = entry.row * kN + entry.column;
  for (const double corrupt :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::signaling_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    units::Unit unit = spec_unit();
    unit.c[element] = corrupt;
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict)) << corrupt;
    const auto& mismatch = std::get<BoundedMismatch>(verdict);
    EXPECT_EQ(mismatch.element, element);
    EXPECT_FALSE(mismatch.residual <= mismatch.tolerance);
    EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
  }
}

TEST(DenseLinearOracleCorruption, EverySingleEntryIsCaughtWhenEveryEntryIsSampled) {
  // At grid = n the sample is the whole output: an exponent flip anywhere --
  // here in an entry of the first, a middle and the last 32 x 32 tile, none of
  // them an entry the default grid samples for this unit -- is found and
  // named.
  const auto oracle = make(kN);
  for (const std::size_t tile : {std::size_t{0}, std::size_t{27}, std::size_t{63}}) {
    const std::size_t element = (tile / 8) * 32 * kN + (tile % 8) * 32 + 5 * kN + 17;
    const Entry sampled = sample_entry(spec_unit().index, kDefaultGrid, tile);
    ASSERT_NE(sampled.row * kN + sampled.column, element);
    units::Unit unit = spec_unit();
    unit.c[element] = flip(unit.c[element], 60);
    EXPECT_TRUE(std::holds_alternative<Agreement>(check(*make(), unit))) << "not sampled";
    unit = spec_unit();
    unit.c[element] = flip(unit.c[element], 60);
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict)) << element;
    EXPECT_EQ(std::get<BoundedMismatch>(verdict).element, element);
    EXPECT_EQ(blocks_of(verdict), kEntries);
  }
}

/// A unit whose sampled entry has an exact oracle value: A = 1/2 and B = 1/4
/// everywhere, so c' = 32 and S^ = 32 exactly.
units::Unit flat_unit(std::uint64_t index) {
  return units::Unit{index, std::vector<double>(kEntries, 0.5), std::vector<double>(kEntries, 0.25),
                     std::vector<double>(kEntries, 32.0)};
}

TEST(DenseLinearOracleBound, BreachedAtOnePlusEpsilonHeldAtOneMinusEpsilon) {
  // M2b exit criterion 3, through the oracle: C[i][j] = c' +/- (1 +/- eps) T^
  // at a sampled entry. eps = 1/16 is sixteen ulps of 32 at this T^ (about
  // 2^-39), so the rounding of the sum cannot carry it across the bound.
  const auto oracle = make();
  const double t = residual::tolerance(residual::dot_model(kN), 32.0).value();
  const double eps = 1.0 / 16.0;
  ASSERT_GT(t * eps, 0x1p-47);
  const Entry entry = sample_entry(9, kDefaultGrid, 27);
  const std::size_t element = entry.row * kN + entry.column;
  for (const double sign : {1.0, -1.0}) {
    units::Unit unit = flat_unit(9);
    unit.c[element] = 32.0 + sign * t * (1.0 + eps);
    const Verdict breached = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(breached)) << sign;
    const auto& mismatch = std::get<BoundedMismatch>(breached);
    EXPECT_EQ(mismatch.element, element);
    EXPECT_EQ(mismatch.tolerance, t);
    const double ratio = verification::ratio(record_of(mismatch, 9));
    EXPECT_GT(ratio, 1.0);
    EXPECT_LT(ratio, 1.0 + 2.0 * eps);

    unit.c[element] = 32.0 + sign * t * (1.0 - eps);
    const Verdict held = check(*oracle, unit);
    EXPECT_TRUE(std::holds_alternative<Agreement>(held)) << sign;
  }
}

// What a record with bits has. Each is checked true of the type that has it,
// so a concept that could never be satisfied cannot pass for an absence.
template <typename T>
concept HasDifference = requires(T r) { r.difference; };
template <typename T>
concept HasLineBytes = requires(T r) { r.line_bytes; };
template <typename T>
concept HasSide = requires(T r) { r.side; };
template <typename T>
concept HasPopcount = requires(T r) { r.popcount; };
template <typename T>
concept HasBytes = requires(T r) {
  r.expected;
  r.actual;
};
template <typename T>
concept Classifiable = requires(const T& r) { classify(r); };

TEST(DenseLinearOracleRecord, ANumericalFailureFromTheseVerdictsHasNoBitFields) {
  // The M2b P4 row: the record of a bounded breach carries no expected bits,
  // no delta, no popcount and no classification -- not empty ones, none. The
  // type cannot hold them (asserted at compile time), the record is not the
  // kind that has them, and it is a valid record of the kind it is.
  static_assert(HasDifference<ExactBitCorruption> && !HasDifference<NumericalVerificationFailure>);
  static_assert(HasLineBytes<ExactBitCorruption> && !HasLineBytes<NumericalVerificationFailure>);
  static_assert(HasSide<ExactBitCorruption> && !HasSide<NumericalVerificationFailure>);
  static_assert(Classifiable<ExactBitCorruption> && !Classifiable<NumericalVerificationFailure>);
  static_assert(HasPopcount<Difference> && !HasPopcount<NumericalVerificationFailure>);
  static_assert(HasBytes<Difference> && !HasBytes<NumericalVerificationFailure>);

  const auto oracle = make();
  const Entry entry = sample_entry(spec_unit().index, kDefaultGrid, 12);
  std::vector<units::Unit> corrupted(3, spec_unit());
  corrupted[0].c[entry.row * kN + entry.column] =
      flip(spec_unit().c[entry.row * kN + entry.column], 62);
  corrupted[1].c[entry.row * kN + entry.column] = std::numeric_limits<double>::quiet_NaN();
  corrupted[2].c[entry.row * kN + entry.column] *= 1.0 + 0x1p-30;
  for (const units::Unit& unit : corrupted) {
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<BoundedMismatch>(verdict));
    const ErrorRecord record = record_of(std::get<BoundedMismatch>(verdict), unit.index);
    EXPECT_EQ(kind_name(record), "NumericalVerificationFailure");
    EXPECT_FALSE(std::holds_alternative<ExactBitCorruption>(record));
    EXPECT_TRUE(is_wrong_result(record));
    EXPECT_TRUE(is_valid(record));
    const auto& numerical = std::get<NumericalVerificationFailure>(record);
    EXPECT_EQ(numerical.element, entry.row * kN + entry.column);
    EXPECT_FALSE(numerical.residual <= numerical.tolerance);
    EXPECT_TRUE(std::isfinite(numerical.tolerance));
    EXPECT_GT(numerical.tolerance, 0.0);
  }
}

TEST(DenseLinearOracleNotPerformed, AnInputTheDerivationCannotUseIsNotChecked) {
  // Corrupt INPUT entries, one at a time, anywhere in A or B: the tolerance
  // cannot be derived, nothing is compared, and the record is a check not
  // made -- even where the output, judged against the corrupted input, might
  // have looked right or wrong.
  const auto oracle = make();
  struct Case {
    bool in_a;
    std::size_t element;
    double value;
  };
  const std::vector<Case> cases{
      {true, 0, std::numeric_limits<double>::quiet_NaN()},
      {true, 4097, std::numeric_limits<double>::infinity()},
      {false, kEntries - 1, -std::numeric_limits<double>::infinity()},
      {false, 300, 1.0},
      {true, 65000, std::nextafter(-1.0, -2.0)},
      {false, 12, flip(0.75, 62)},  // the top exponent bit: 0.75 becomes 0.75 * 2^1024
  };
  for (const Case& entry : cases) {
    units::Unit unit = spec_unit();
    (entry.in_a ? unit.a : unit.b)[entry.element] = entry.value;
    const Verdict verdict = check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << entry.value;
    const auto& unchecked = std::get<NotChecked>(verdict);
    EXPECT_EQ(unchecked.blocks_compared, 0U);
    EXPECT_TRUE(is_valid(verdict, Contract::kBounded));
    // The record: VerificationNotPerformed, with zero compared and no
    // provenance invented -- the runner adds what it knows, here nothing.
    const ErrorRecord record = VerificationNotPerformed{Provenance{}, unchecked.what, unchecked.why,
                                                        unchecked.blocks_compared};
    EXPECT_EQ(kind_name(record), "VerificationNotPerformed");
    EXPECT_FALSE(is_wrong_result(record));
    EXPECT_TRUE(is_valid(record));
    EXPECT_FALSE(provenance_of(record).buffer_address.has_value());
  }
}

TEST(DenseLinearOracleNotPerformed, EvidenceOfTheWrongSizeIsNotChecked) {
  const auto oracle = make();
  const units::Case good = units::to_case(spec_unit());
  for (const std::size_t input : {std::size_t{0}, good.input.size() / 2, good.input.size() + 8}) {
    std::vector<std::byte> bytes(input);
    const Verdict verdict = oracle->check(UnitEvidence{0, bytes, good.output});
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << input;
    EXPECT_EQ(blocks_of(verdict), 0U);
  }
  for (const std::size_t output : {std::size_t{0}, std::size_t{8}, good.output.size() - 1}) {
    std::vector<std::byte> bytes(output);
    const Verdict verdict = oracle->check(UnitEvidence{0, good.input, bytes});
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << output;
    EXPECT_EQ(blocks_of(verdict), 0U);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::dense_linear
