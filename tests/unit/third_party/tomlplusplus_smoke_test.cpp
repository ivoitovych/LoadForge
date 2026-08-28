// SPDX-License-Identifier: GPL-3.0-or-later
//
// A smoke test for the vendored TOML parser.
//
// This is not a test of toml++ — upstream tests toml++. It is the only
// validation a vendored blob otherwise gets, and it answers three questions the
// dependency checker cannot:
//
//   1. does the vendored header compile under THIS project's toolchain matrix —
//      {x86-64, ARM64} × {GCC 13, Clang 18}, plus both sanitizers and the static
//      build — rather than under upstream's;
//   2. does it behave, on the small surface config/ will actually use;
//   3. does it report a malformed document as an error rather than as a default,
//      which is the F20 property every parser in this project owes.
//
// tools/check-dependencies.py proves the vendored bytes are the reviewed bytes.
// This proves the reviewed bytes work here. Neither substitutes for the other:
// a digest over a file that does not compile is a checksum of a brick.
//
// The document below is deliberately shaped like the config file M1 will parse
// (docs/IMPLEMENTATION.md §4), so that if toml++ is ever swapped out, this test
// states what a replacement has to do.

#include <gtest/gtest.h>

#include <string_view>
#include <toml.hpp>

namespace {

constexpr std::string_view kConfig = R"(
[run]
mode     = "stress"
duration = "5h"

[limits]
max_temperature_c = 95
memory            = "70%"

[[workload]]
name    = "compression"
threads = 8
enabled = true
)";

TEST(VendoredTomlPlusPlus, ParsesTheShapeConfigWillUse) {
  const toml::table config = toml::parse(kConfig);

  EXPECT_EQ(config["run"]["mode"].value_or(std::string_view{}), "stress");
  EXPECT_EQ(config["run"]["duration"].value_or(std::string_view{}), "5h");
  EXPECT_EQ(config["limits"]["max_temperature_c"].value_or(0), 95);
  EXPECT_EQ(config["limits"]["memory"].value_or(std::string_view{}), "70%");

  const auto* workloads = config["workload"].as_array();
  ASSERT_NE(workloads, nullptr);
  ASSERT_EQ(workloads->size(), 1U);
  const auto* first = workloads->at(0).as_table();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ((*first)["name"].value_or(std::string_view{}), "compression");
  EXPECT_EQ((*first)["threads"].value_or(0), 8);
  EXPECT_TRUE((*first)["enabled"].value_or(false));
}

// A missing key must be distinguishable from a key that is present. config/ will
// depend on this to tell "the user did not set it" from "the user set it to the
// default", which are different facts about a run.
TEST(VendoredTomlPlusPlus, AbsentKeyIsAbsentRatherThanDefaulted) {
  const toml::table config = toml::parse(kConfig);

  EXPECT_FALSE(config["run"]["no_such_key"].is_value());
  EXPECT_EQ(config["run"]["no_such_key"].value_or(-1), -1);
  EXPECT_TRUE(config["limits"]["max_temperature_c"].is_value());
}

// The F20 property: a parser that cannot understand its input must say so.
// Returning an empty table for a malformed document would let a corrupt config
// silently become a default one — and a stress run at default limits is not the
// run the user asked for.
TEST(VendoredTomlPlusPlus, MalformedDocumentThrowsRatherThanDefaulting) {
  EXPECT_THROW((void)toml::parse("[run\nmode = "), toml::parse_error);

  try {
    (void)toml::parse("[run\nmode = ");
    FAIL() << "a malformed document parsed without error";
  } catch (const toml::parse_error& error) {
    // The error must locate the problem, or a user cannot act on it.
    EXPECT_GT(error.source().begin.line, 0U);
    EXPECT_FALSE(std::string_view{error.description()}.empty());
  }
}

// Type confusion must fail rather than coerce. "duration = 5" where a string is
// expected is a config error, not a 5.
TEST(VendoredTomlPlusPlus, WrongTypeDoesNotSilentlyCoerce) {
  const toml::table config = toml::parse("[run]\nduration = 5\n");

  EXPECT_EQ(config["run"]["duration"].value_or(std::string_view{"unset"}), "unset");
  EXPECT_EQ(config["run"]["duration"].value_or(0), 5);
}

}  // namespace
