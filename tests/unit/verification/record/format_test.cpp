// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a record's numbers and text are spelled for a person.

#include "verification/record/format.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace loadforge::verification {
namespace {

TEST(RecordFormat, ShortestDecimalReadsBackExactly) {
  EXPECT_EQ(shortest(3.5e-12), "3.5e-12");
  EXPECT_EQ(shortest(0.1), "0.1");
  EXPECT_EQ(shortest(28.0), "28");
  EXPECT_EQ(shortest(-0.0), "-0");
  EXPECT_EQ(shortest(std::numeric_limits<double>::quiet_NaN()), "nan");
  EXPECT_EQ(shortest(-std::numeric_limits<double>::infinity()), "-inf");
  EXPECT_EQ(shortest(std::numeric_limits<double>::denorm_min()), "5e-324");
  EXPECT_EQ(shortest(-std::numeric_limits<double>::max()), "-1.7976931348623157e+308")
      << "the longest a double can be";
}

TEST(RecordFormat, HexHasNoLeadingZeros) {
  EXPECT_EQ(hex(0), "0x0");
  EXPECT_EQ(hex(0x7f0012340000ULL), "0x7f0012340000");
  EXPECT_EQ(hex(std::numeric_limits<std::uint64_t>::max()), "0xffffffffffffffff");
}

TEST(RecordFormat, OnlyPrintableAsciiPassesUnescaped) {
  EXPECT_EQ(escaped(""), "");
  EXPECT_EQ(escaped("componentwise |C - AB| ~ !"), "componentwise |C - AB| ~ !");
  EXPECT_EQ(escaped(" "), " ") << "0x20, the first printable";
  EXPECT_EQ(escaped("~"), "~") << "0x7e, the last";
  EXPECT_EQ(escaped(std::string(1, '\x1f')), "\\x1f");
  EXPECT_EQ(escaped(std::string(1, '\x7f')), "\\x7f") << "DEL";
  EXPECT_EQ(escaped("\x1b[2J"), "\\x1b[2J") << "a terminal escape is shown, not obeyed";
  EXPECT_EQ(escaped("a\\b"), "a\\\\b") << "so an escaped byte cannot be forged";
  EXPECT_EQ(escaped(std::string("\0\n", 2)), "\\x00\\x0a");
  EXPECT_EQ(escaped("\xc2\x9b\xff"), "\\xc2\\x9b\\xff") << "nothing outside ASCII";
}

}  // namespace
}  // namespace loadforge::verification
