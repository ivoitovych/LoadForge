// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5b for the compression workload: evidence of the wrong size is reported as
// not checked -- never an agreement, and never a mismatch either, since there
// is no unit of this workload to have compared (spec.hpp: "evidence of the
// wrong size is NotChecked"; verification.md §5).

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "oracle/workloads/compression/lz4_decoder.hpp"
#include "unit/workloads/compression/support.hpp"
#include "workloads/compression/compression.hpp"
#include "workloads/compression/generator.hpp"
#include "workloads/compression/lz4_block.hpp"
#include "workloads/compression/spec.hpp"

namespace loadforge::workloads::compression {
namespace {

using testing::Bytes;
using testing::judge_unit;
using testing::Judgement;
using testing::Verdict;

void expect_not_checked(const Bytes& input, const Bytes& output) {
  const Judgement judgement = judge_unit(input, output);
  EXPECT_EQ(judgement.verdict, Verdict::kNotChecked);
  EXPECT_EQ(judgement.check, testing::Check::kNone);
  EXPECT_EQ(judgement.why, "evidence of the wrong size: input " + std::to_string(input.size()) +
                               " bytes, output " + std::to_string(output.size()) + " bytes");
}

Bytes good_input() {
  Bytes input(kInputBytes);
  generate_input(2, 2, input);
  return input;
}

Bytes good_output(const Bytes& input) {
  Bytes output(output_bytes_for(kInputBytes));
  Bytes table(kTableBytes);
  write_unit(input, output, table);
  return output;
}

TEST(CompressionNotChecked, AnInputOfTheWrongSizeIsNotChecked) {
  const Bytes input = good_input();
  const Bytes output = good_output(input);
  for (const std::size_t size : {std::size_t{0}, kInputBytes - 1, kInputBytes + 1}) {
    Bytes resized = input;
    resized.resize(size);
    expect_not_checked(resized, output);
  }
}

TEST(CompressionNotChecked, AnOutputOfTheWrongSizeIsNotChecked) {
  const Bytes input = good_input();
  const Bytes output = good_output(input);
  for (const std::size_t size :
       {std::size_t{0}, std::size_t{8}, output.size() - 1, output.size() + 1}) {
    Bytes resized = output;
    resized.resize(size);
    expect_not_checked(input, resized);
  }
}

TEST(CompressionNotChecked, BothWrongIsNotCheckedEither) { expect_not_checked({}, {}); }

TEST(CompressionNotChecked, AnOutputThatWouldAgreeButForOneExtraZeroIsStillNotChecked) {
  const Bytes input = good_input();
  Bytes output = good_output(input);
  ASSERT_EQ(judge_unit(input, output).verdict, Verdict::kAgreement);
  output.push_back(std::byte{0});
  expect_not_checked(input, output);
}

TEST(CompressionNotChecked, AnOutputThatWouldMismatchButIsShortIsNotAMismatch) {
  const Bytes input = good_input();
  const Bytes output(output_bytes_for(kInputBytes) - 1, std::byte{0xFF});
  expect_not_checked(input, output);
}

}  // namespace
}  // namespace loadforge::workloads::compression
