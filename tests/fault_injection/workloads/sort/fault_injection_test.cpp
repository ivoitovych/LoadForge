// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for src/workloads/sort. The sort's verification is the checker its
// tests are written with (tests/oracle/workloads/sort/check.hpp), and the
// workload's faults are an executor that breaks its contract. So this file
// shows two things, each by breaking the thing on purpose:
//
//   T5   a corrupt output, or an output from a faulty run, is classified as the
//        specification classifies it -- which step, which position -- and never
//        as an agreement;
//   T5b  evidence the checker cannot use is NotChecked, never an agreement and
//        never a mismatch.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "oracle/workloads/sort/check.hpp"
#include "unit/workloads/sort/executors.hpp"
#include "unit/workloads/sort/harness.hpp"
#include "workloads/sort/spec.hpp"

namespace loadforge::workloads::sort::testing {
namespace {

using checking::Outcome;
using checking::Verdict;

class SortFaults : public ::testing::Test {
 protected:
  void SetUp() override {
    workload_ = make(16, 0xF00D);
    ASSERT_NE(workload_, nullptr);
    input_ = generate(*workload_, 2);
    SerialExecutor serial;
    good_ = sorted(*workload_, 2, input_, serial);
    ASSERT_EQ(checking::check(input_, good_).verdict, Verdict::kAgreement);
  }

  static void put_index(Bytes& output, std::size_t position, std::uint32_t index) {
    for (std::size_t byte = 0; byte < kIndexBytes; ++byte) {
      output[index_offset(position) + byte] =
          static_cast<std::byte>((index >> (8U * byte)) & 0xFFU);
    }
  }

  std::unique_ptr<Workload> workload_;
  Bytes input_;
  Bytes good_;
};

// ---- T5: the classifications of the specification ---------------------------

TEST_F(SortFaults, AFlippedKeyBitIsAKeyMismatchAtItsPosition) {
  for (const std::size_t position : {std::size_t{0}, std::size_t{4242}, kKeys - 1}) {
    for (const unsigned bit : {0U, 31U, 63U}) {
      Bytes output = good_;
      output[key_offset(position) + bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
      const Outcome outcome = checking::check(input_, output);
      EXPECT_EQ(outcome.verdict, Verdict::kKeyDiffers) << position << " bit " << bit;
      EXPECT_EQ(outcome.position, position);
      EXPECT_EQ(outcome.blocks_compared, kKeys);
    }
  }
}

TEST_F(SortFaults, TheSmallestDifferingPositionIsTheOneNamed) {
  Bytes output = good_;
  output[key_offset(900)] ^= std::byte{1};
  output[key_offset(300)] ^= std::byte{1};
  output[key_offset(70000)] ^= std::byte{1};
  EXPECT_EQ(checking::check(input_, output).position, 300U);
}

TEST_F(SortFaults, AnIndexBitMakingItOutOfRangeIsCaughtBeforeAnyKeyIsLookedAt) {
  Bytes output = good_;
  output[index_offset(5000) + 3] ^= std::byte{0x80};  // bit 31
  output[key_offset(9)] ^= std::byte{1};              // a key fault earlier: step 1 still wins
  const Outcome outcome = checking::check(input_, output);
  EXPECT_EQ(outcome.verdict, Verdict::kIndexOutOfRange);
  EXPECT_EQ(outcome.position, 5000U);
  EXPECT_EQ(outcome.blocks_compared, 5001U);
}

TEST_F(SortFaults, TheLargestIndexThatIsOutOfRangeIsKKeys) {
  Bytes output = good_;
  put_index(output, 17, static_cast<std::uint32_t>(kKeys));
  EXPECT_EQ(checking::check(input_, output).verdict, Verdict::kIndexOutOfRange);
  put_index(output, 17, static_cast<std::uint32_t>(kKeys - 1));
  EXPECT_NE(checking::check(input_, output).verdict, Verdict::kIndexOutOfRange)
      << "kKeys - 1 is in range";
}

TEST_F(SortFaults, AnIndexBitMakingItARepeatIsCaughtAtTheLaterPosition) {
  std::vector<std::size_t> where(kKeys);
  for (std::size_t position = 0; position < kKeys; ++position) {
    where[index_of(good_, position)] = position;
  }
  const std::size_t position = 20000;
  Bytes output = good_;
  const std::uint32_t flipped = index_of(good_, position) ^ 1U;
  put_index(output, position, flipped);
  const std::size_t other = where[flipped];
  const Outcome outcome = checking::check(input_, output);
  EXPECT_EQ(outcome.verdict, Verdict::kIndexRepeated);
  EXPECT_EQ(outcome.position, std::max(position, other));
  EXPECT_EQ(outcome.blocks_compared, std::max(position, other) + 1);
}

TEST_F(SortFaults, ADuplicatedEntryIsARepeat) {
  Bytes output = good_;
  put_index(output, 61, index_of(good_, 60));
  const Outcome outcome = checking::check(input_, output);
  EXPECT_EQ(outcome.verdict, Verdict::kIndexRepeated);
  EXPECT_EQ(outcome.position, 61U);
}

TEST_F(SortFaults, TwoEntriesSwappedWholeBreakTheOrderAtTheSecondOfThem) {
  Bytes output = good_;
  for (std::size_t byte = 0; byte < kKeyBytes; ++byte) {
    std::swap(output[key_offset(777) + byte], output[key_offset(778) + byte]);
  }
  for (std::size_t byte = 0; byte < kIndexBytes; ++byte) {
    std::swap(output[index_offset(777) + byte], output[index_offset(778) + byte]);
  }
  const Outcome outcome = checking::check(input_, output);
  EXPECT_EQ(outcome.verdict, Verdict::kOrderBroken);
  EXPECT_EQ(outcome.position, 778U);
  EXPECT_EQ(outcome.blocks_compared, kKeys);
}

TEST_F(SortFaults, TwoKeysSwappedWithoutTheirIndicesAreKeyMismatches) {
  Bytes output = good_;
  for (std::size_t byte = 0; byte < kKeyBytes; ++byte) {
    std::swap(output[key_offset(777) + byte], output[key_offset(778) + byte]);
  }
  const Outcome outcome = checking::check(input_, output);
  EXPECT_EQ(outcome.verdict, Verdict::kKeyDiffers);
  EXPECT_EQ(outcome.position, 777U);
}

TEST_F(SortFaults, EqualKeysInTheWrongPositionOrderBreakTheOrder) {
  // An input of one repeated key, sorted right, with two neighbours' indices swapped.
  const Bytes same = input_of([](std::size_t) { return std::uint64_t{42}; });
  SerialExecutor serial;
  Bytes output = sorted(*workload_, 0, same, serial);
  ASSERT_EQ(checking::check(same, output).verdict, Verdict::kAgreement);
  put_index(output, 10, 11);
  put_index(output, 11, 10);
  const Outcome outcome = checking::check(same, output);
  EXPECT_EQ(outcome.verdict, Verdict::kOrderBroken);
  EXPECT_EQ(outcome.position, 11U);
}

TEST_F(SortFaults, InputCorruptedAfterTheSortIsAKeyMismatchAtTheOutputPositionThatReadsIt) {
  Bytes input = input_;
  const std::size_t position = 31337;
  input[position * kKeyBytes + 2] ^= std::byte{0x10};
  const Outcome outcome = checking::check(input, good_);
  EXPECT_EQ(outcome.verdict, Verdict::kKeyDiffers);
  EXPECT_EQ(index_of(good_, outcome.position), position);
}

TEST_F(SortFaults, InputCorruptedBeforeTheSortIsSortedRightAndAgrees) {
  Bytes input = input_;
  input[1234 * kKeyBytes + 5] ^= std::byte{0x40};
  SerialExecutor serial;
  const Bytes output = sorted(*workload_, 2, input, serial);
  EXPECT_EQ(checking::check(input, output).verdict, Verdict::kAgreement);
  EXPECT_NE(output, good_) << "the corruption is in the output; only the bracket can say it is";
}

// ---- T5: an executor that breaks its contract -------------------------------

TEST_F(SortFaults, AnExecutorThatDropsARangeOfTheFirstMoveLosesThoseKeys) {
  // The dropped range's destinations keep the 0xFF the scratch was filled
  // with, key and index alike. They are the largest key there is, so they end
  // the output, and the first of them has an index no input position has.
  DroppingExecutor drops(1, 0);  // run() number 1 is the first pass's move
  const Bytes output = sorted(*workload_, 2, input_, drops, std::byte{0xFF});
  const Outcome outcome = checking::check(input_, output);
  EXPECT_EQ(outcome.verdict, Verdict::kIndexOutOfRange);
  EXPECT_EQ(outcome.position, kKeys - kKeys / 16);
  EXPECT_EQ(outcome.blocks_compared, kKeys - kKeys / 16 + 1);
}

TEST_F(SortFaults, AnExecutorThatDropsARangeOfTheLastMoveLeavesStaleEntriesThatRepeat) {
  DroppingExecutor drops(2 * 8 - 1, 3);  // the last pass's move, range 3
  const Bytes output = sorted(*workload_, 2, input_, drops, std::byte{0xFF});
  const Outcome outcome = checking::check(input_, output);
  EXPECT_EQ(outcome.verdict, Verdict::kIndexRepeated);
}

TEST_F(SortFaults, AnExecutorThatDropsNothingChangesNothing) {
  DroppingExecutor never(~std::size_t{0}, 0);
  EXPECT_EQ(sorted(*workload_, 2, input_, never), good_);
}

// ---- T5b: checks that could not be performed --------------------------------

TEST_F(SortFaults, EvidenceOfTheWrongSizeIsNotCheckedAndComparesNothing) {
  const Bytes short_input(kInputBytes - 1);
  const Bytes long_input(kInputBytes + 1);
  const Bytes short_output(kOutputBytes - 1);
  const Bytes long_output(kOutputBytes + 1);
  const std::vector<std::pair<const Bytes*, const Bytes*>> evidence = {
      {&short_input, &good_},
      {&long_input, &good_},
      {&input_, &short_output},
      {&input_, &long_output},
      {&short_input, &short_output}};
  for (const auto& [input, output] : evidence) {
    const Outcome outcome = checking::check(*input, *output);
    EXPECT_EQ(outcome.verdict, Verdict::kNotChecked);
    EXPECT_EQ(outcome.blocks_compared, 0U);
  }
  EXPECT_EQ(checking::check({}, {}).verdict, Verdict::kNotChecked);
  EXPECT_EQ(checking::check(input_, {}).verdict, Verdict::kNotChecked);
  EXPECT_EQ(checking::check({}, good_).verdict, Verdict::kNotChecked);
}

TEST_F(SortFaults, ACorruptOutputOfTheWrongSizeIsNotCheckedEvenThoughItIsWrong) {
  Bytes truncated(good_.begin(), good_.end() - 1);
  truncated[3] ^= std::byte{1};
  EXPECT_EQ(checking::check(input_, truncated).verdict, Verdict::kNotChecked);
}

}  // namespace
}  // namespace loadforge::workloads::sort::testing
