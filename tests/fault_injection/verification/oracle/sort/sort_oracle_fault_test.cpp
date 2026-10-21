// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for the sort oracle: every corruption of a unit is caught and
// classified as what it is, and every way the check can fail to happen is
// NotChecked, never an Agreement.

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_case.hpp"
#include "sort_units.hpp"
#include "verification/oracle/sort/sort_oracle.hpp"
#include "verification/record.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::contracts {
// The contract header registers both suites; this binary runs neither.
}  // namespace loadforge::contracts

namespace loadforge::tests {
namespace {

namespace spec = workloads::sort;
using contracts::OracleCase;
using verification::ExactMismatch;
using verification::NotChecked;
using verification::Verdict;

std::unique_ptr<verification::Oracle> make_oracle() {
  const workloads::Plan plan{
      spec::kInputBytes, spec::kOutputBytes, 0, 1, spec::kDefaultChunkCount, 0, 0};
  return verification::oracle::make_sort_oracle({}, plan).value();
}

Verdict check(const OracleCase& unit) {
  return contracts::oracle_suite::check(*make_oracle(), unit);
}

const OracleCase& good() { return sort_units::cached_good_units()[0]; }

ExactMismatch mismatch_of(const Verdict& verdict) { return std::get<ExactMismatch>(verdict); }

verification::BitClass class_of(const ExactMismatch& mismatch) {
  const verification::Difference& difference = *mismatch.difference;
  return verification::classify_bits(difference.popcount, difference.first_offset,
                                     difference.last_offset, 64);
}

// ---- T5: the classification of a corrupted key ---------------------------------

TEST(SortOracleFaults, EveryBitOfAKeyFlippedIsASingleBitMismatchAtThatBit) {
  const auto oracle = make_oracle();
  for (std::size_t bit = 0; bit < 64; ++bit) {
    OracleCase unit = good();
    unit.output[spec::key_offset(5) + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    const Verdict verdict = contracts::oracle_suite::check(*oracle, unit);
    const ExactMismatch mismatch = mismatch_of(verdict);
    ASSERT_TRUE(mismatch.difference.has_value()) << bit;
    EXPECT_EQ(mismatch.difference->popcount, 1U);
    EXPECT_EQ(mismatch.difference->first_offset, 40 + bit / 8);
    EXPECT_EQ(mismatch.difference->last_offset, 40 + bit / 8);
    EXPECT_EQ(class_of(mismatch), verification::BitClass::kSingleBit);
  }
}

TEST(SortOracleFaults, TwoBitsInOneKeyAreMultipleBitsInOneWord) {
  OracleCase unit = good();
  unit.output[spec::key_offset(9)] ^= std::byte{0x81};
  unit.output[spec::key_offset(9) + 6] ^= std::byte{0x40};
  const ExactMismatch mismatch = mismatch_of(check(unit));
  EXPECT_EQ(mismatch.difference->popcount, 3U);
  EXPECT_EQ(class_of(mismatch), verification::BitClass::kMultiBitInWord);
}

TEST(SortOracleFaults, BitsInTwoKeysOfOneCacheLineAreWithinTheLine) {
  OracleCase unit = good();
  unit.output[spec::key_offset(16)] ^= std::byte{0x01};      // line 2 (bytes 128 ..)
  unit.output[spec::key_offset(19) + 2] ^= std::byte{0x01};  // the same line
  const ExactMismatch mismatch = mismatch_of(check(unit));
  EXPECT_EQ(mismatch.difference->popcount, 2U);
  EXPECT_EQ(class_of(mismatch), verification::BitClass::kWithinCacheLine);
}

TEST(SortOracleFaults, BitsInKeysOfTwoCacheLinesAreAcrossLines) {
  OracleCase unit = good();
  unit.output[spec::key_offset(16)] ^= std::byte{0x01};
  unit.output[spec::key_offset(200)] ^= std::byte{0x01};
  const ExactMismatch mismatch = mismatch_of(check(unit));
  EXPECT_EQ(mismatch.difference->popcount, 2U);
  EXPECT_EQ(class_of(mismatch), verification::BitClass::kAcrossCacheLines);
}

TEST(SortOracleFaults, EveryDifferenceTheOracleReportsIsValidAndTellsTheTruth) {
  OracleCase unit = good();
  unit.output[spec::key_offset(31) + 4] ^= std::byte{0x20};
  const Verdict verdict = check(unit);
  const ExactMismatch mismatch = mismatch_of(verdict);
  EXPECT_TRUE(verification::is_valid(*mismatch.difference));
  const std::vector<std::byte> delta = verification::xor_delta(*mismatch.difference);
  std::uint64_t ones = 0;
  for (const std::byte byte : delta) {
    ones += static_cast<std::uint64_t>(std::popcount(std::to_integer<unsigned>(byte)));
  }
  EXPECT_EQ(ones, mismatch.difference->popcount);
}

// ---- T5: the corruption of an index ---------------------------------------------

TEST(SortOracleFaults, EveryBitOfAnIndexFlippedIsCaughtAsARangeOrARepeat) {
  const auto oracle = make_oracle();
  for (std::size_t bit = 0; bit < 32; ++bit) {
    OracleCase unit = good();
    unit.output[spec::index_offset(1234) + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
    const Verdict verdict = contracts::oracle_suite::check(*oracle, unit);
    const ExactMismatch mismatch = mismatch_of(verdict);
    EXPECT_FALSE(mismatch.difference.has_value()) << bit;
    const bool range = mismatch.check.starts_with("index out of range at position ");
    const bool repeat = mismatch.check.starts_with("index repeated at position ");
    EXPECT_TRUE(range || repeat) << bit << ": " << mismatch.check;
    if (bit >= 17) {
      EXPECT_EQ(mismatch.check, "index out of range at position 1234") << bit;
    }
  }
}

// ---- T5: the input corrupted, before and after compute() -----------------------

TEST(SortOracleFaults, InputCorruptedBeforeComputeSortsCorrectlyAndAgrees) {
  std::vector<std::uint64_t> keys = sort_units::generated_keys(1, 0);
  keys[700] ^= std::uint64_t{1} << 33U;  // the corruption happened before the sort read it
  const OracleCase unit = sort_units::unit_of(0, keys);
  EXPECT_TRUE(std::holds_alternative<verification::Agreement>(check(unit)));
}

TEST(SortOracleFaults, InputCorruptedAfterComputeReadsAsAKeyMismatchAtThatKey) {
  OracleCase unit = good();
  // Input position 4000 is named by exactly one output position.
  unit.input[4000 * 8 + 2] ^= std::byte{0x08};
  const ExactMismatch mismatch = mismatch_of(check(unit));
  EXPECT_NE(mismatch.check.find("key differs from input key 4000 at position "), std::string::npos);
  EXPECT_EQ(mismatch.difference->popcount, 1U);
}

// ---- T5b: a check that was not made --------------------------------------------

TEST(SortOracleUnrun, EvidenceOfTheWrongSizeIsNotCheckedWithNothingCompared) {
  const auto oracle = make_oracle();
  std::vector<OracleCase> units(6, good());
  units[0].output.pop_back();
  units[1].output.push_back(std::byte{0});
  units[2].output.clear();
  units[3].input.pop_back();
  units[4].input.push_back(std::byte{0});
  units[5].input.clear();
  for (const OracleCase& unit : units) {
    const Verdict verdict = contracts::oracle_suite::check(*oracle, unit);
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
    EXPECT_EQ(verification::blocks_of(verdict), 0U);
    EXPECT_FALSE(std::get<NotChecked>(verdict).what.empty());
    EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kExact));
  }
}

TEST(SortOracleUnrun, ScratchThatCannotBeObtainedIsNotCheckedWhateverTheOutputHolds) {
  std::size_t asked = 0;
  const auto oracle = verification::oracle::make_sort_oracle(
      {},
      workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, spec::kDefaultChunkCount, 0, 0},
      [&asked](std::size_t words) {
        asked = words;
        return verification::oracle::Scratch();
      });
  ASSERT_TRUE(oracle.has_value());
  const OracleCase& good_unit = good();
  OracleCase bad = good_unit;
  bad.output[spec::key_offset(3)] ^= std::byte{1};
  const std::vector<const OracleCase*> units = {&good_unit, &bad};
  for (const OracleCase* unit : units) {
    const Verdict verdict = contracts::oracle_suite::check(*oracle.value(), *unit);
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
    EXPECT_EQ(verification::blocks_of(verdict), 0U);
    EXPECT_FALSE(std::get<NotChecked>(verdict).why.empty());
    EXPECT_TRUE(verification::is_valid(verdict, verification::Contract::kExact));
  }
  EXPECT_EQ(asked, spec::kKeys / 64);  // one bit per input position
}

TEST(SortOracleUnrun, TheDefaultScratchHoldsTheWordsAsked) {
  const verification::oracle::Scratch scratch = verification::oracle::allocate_scratch(2048);
  ASSERT_NE(scratch, nullptr);
  for (std::size_t word = 0; word < 2048; ++word) {
    scratch.get()[word] = word;
  }
  for (std::size_t word = 0; word < 2048; ++word) {
    EXPECT_EQ(scratch.get()[word], word);
  }
}

}  // namespace
}  // namespace loadforge::tests
