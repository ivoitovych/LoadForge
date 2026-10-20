// SPDX-License-Identifier: GPL-3.0-or-later
//
// The working set's layout: every region on a line boundary, every stride a
// whole number of lines, and every way the sizes can fail to fit refused
// before a mapping is asked for (P1, P3).

#include "verification/integrity/layout.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace loadforge::verification::integrity {
namespace {

constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();

workloads::Plan plan(std::uint64_t input, std::uint64_t output, std::uint64_t scratch,
                     std::uint64_t units) {
  return workloads::Plan{input, output, scratch, units, 1, 0, 0};
}

TEST(LayoutTest, EveryRegionStartsOnALineAndEveryStrideIsWholeLines) {
  const auto layout = plan_layout(plan(200, 8, 24, 3), 2, 64);
  ASSERT_TRUE(layout.has_value());
  const Layout& l = layout.value();
  EXPECT_EQ(l.line_bytes, 64U);
  EXPECT_EQ(l.input_bytes, 200U);
  EXPECT_EQ(l.output_bytes, 8U);
  EXPECT_EQ(l.units, 3U);
  EXPECT_EQ(l.input_stride, 256U) << "200 rounded up to four lines";
  EXPECT_EQ(l.output_stride, 64U) << "8 rounded up to one line";
  EXPECT_EQ(l.outputs, 3U * 256U);
  EXPECT_EQ(l.regenerated, 3U * 256U + 3U * 64U);
  EXPECT_EQ(l.scratch, l.regenerated + 256U) << "the regeneration buffer is one input stride";
  EXPECT_EQ(l.scratch_bytes, 48U) << "24 bytes for each of two threads";
  EXPECT_EQ(l.bytes, l.scratch + 64U) << "scratch rounded up to a line";
  EXPECT_EQ(l.mapping_bytes, l.bytes + 64U) << "one line of slack to align the start";
  for (const std::size_t offset : {l.outputs, l.regenerated, l.scratch, l.bytes}) {
    EXPECT_EQ(offset % 64, 0U) << offset;
  }
}

TEST(LayoutTest, ASizeThatIsAlreadyWholeLinesIsNotRoundedFurther) {
  const auto layout = plan_layout(plan(128, 64, 0, 1), 4, 64);
  ASSERT_TRUE(layout.has_value());
  EXPECT_EQ(layout.value().input_stride, 128U);
  EXPECT_EQ(layout.value().output_stride, 64U);
  EXPECT_EQ(layout.value().scratch_bytes, 0U) << "no scratch, at any thread count";
  EXPECT_EQ(layout.value().bytes, layout.value().scratch);
}

TEST(LayoutTest, OneByteUnitsAtTheSmallestLineAndALineLargerThanAPage) {
  const auto small = plan_layout(plan(1, 1, 1, 1), 1, 8);
  ASSERT_TRUE(small.has_value());
  EXPECT_EQ(small.value().input_stride, 8U);
  EXPECT_EQ(small.value().output_stride, 8U);
  EXPECT_EQ(small.value().mapping_bytes, 8U * 4 + 8U);

  const auto large = plan_layout(plan(1, 1, 0, 2), 1, 8192);
  ASSERT_TRUE(large.has_value());
  EXPECT_EQ(large.value().input_stride, 8192U);
  EXPECT_EQ(large.value().outputs, 2U * 8192U);
  EXPECT_EQ(large.value().mapping_bytes, 5U * 8192U + 8192U);
}

TEST(LayoutTest, ALineSizeThatIsNotAPowerOfTwoOfAtLeastEightIsRefused) {
  for (const std::uint32_t line : {0U, 1U, 4U, 7U, 12U, 48U, 96U, 0xFFFFFFFFU}) {
    const auto layout = plan_layout(plan(8, 8, 0, 1), 1, line);
    ASSERT_FALSE(layout.has_value()) << line;
    EXPECT_EQ(layout.error().fault, RunnerFault::kConfiguration);
    EXPECT_NE(layout.error().detail.find(std::to_string(line)), std::string::npos);
  }
  EXPECT_TRUE(plan_layout(plan(8, 8, 0, 1), 1, 0x80000000U).has_value())
      << "the largest power of two a line size can be";
}

TEST(LayoutTest, APlanWithoutAUnitIsRefusedBeforeItDividesByZero) {
  for (const workloads::Plan& broken : {plan(8, 8, 0, 0), plan(0, 8, 0, 1), plan(8, 0, 0, 1)}) {
    const auto layout = plan_layout(broken, 1, 64);
    ASSERT_FALSE(layout.has_value());
    EXPECT_EQ(layout.error().fault, RunnerFault::kConfiguration);
    EXPECT_NE(layout.error().detail.find("no unit"), std::string::npos);
  }
}

TEST(LayoutTest, EverySizeThatOverflowsIsRefusedAsResourcesWithNoMappingAskedFor) {
  const struct {
    const char* why = "";
    workloads::Plan plan{};
    std::size_t threads = 0;
  } cases[] = {
      {"the input stride rounds past the top", plan(kMax, 8, 0, 1), 1},
      {"the output stride rounds past the top", plan(8, kMax - 3, 0, 1), 1},
      {"the inputs region", plan(64, 8, 0, kMax / 32), 1},
      {"the outputs region alone", plan(8, 1U << 20U, 0, kMax / 1024), 1},
      {"inputs plus outputs", plan(1U << 20U, 1U << 20U, 0, ((kMax / 2) >> 20U) + 1), 1},
      {"plus the regeneration buffer", plan(kMax / 2 + 1, 64, 0, 1), 1},
      {"scratch times threads", plan(8, 8, kMax / 2, 1), 3},
      {"scratch rounded up", plan(8, 8, kMax - 10, 1), 1},
      {"plus the scratch", plan(kMax / 4, kMax / 4, kMax / 4, 1), 2},
      {"plus the slack, and nothing before it", plan(kMax / 2 - 63, 64, 0, 1), 1},
  };
  for (const auto& overflow : cases) {
    const auto layout = plan_layout(overflow.plan, overflow.threads, 64);
    ASSERT_FALSE(layout.has_value()) << overflow.why;
    EXPECT_EQ(layout.error().fault, RunnerFault::kResources) << overflow.why;
    EXPECT_NE(layout.error().detail.find("does not fit in the address space"), std::string::npos);
    EXPECT_EQ(layout.error().detail.rfind("working set: ", 0), 0U) << layout.error().detail;
  }
}

TEST(LayoutTest, TheLargestWorkingSetThatFitsIsAccepted) {
  // Inputs, outputs, regeneration and slack: (units + 1) * 64 + units * 64
  // + 64 bytes, all of it addressable.
  const std::size_t units = (kMax - 191) / 128;
  const auto layout = plan_layout(plan(64, 64, 0, units), 1, 64);
  ASSERT_TRUE(layout.has_value());
  EXPECT_LE(layout.value().mapping_bytes, kMax);
  EXPECT_EQ(layout.value().mapping_bytes, units * 128 + 128);
}

}  // namespace
}  // namespace loadforge::verification::integrity
