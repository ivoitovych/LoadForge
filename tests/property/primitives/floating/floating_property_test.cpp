// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for src/primitives/floating: properties over generated cases.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <vector>

#include "primitives/floating/floating.hpp"
#include "primitives/rng/philox.hpp"
#include "unit/primitives/floating/executors.hpp"

namespace loadforge::primitives::floating {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

constexpr std::uint64_t kCases = 100;

Config case_config(std::uint64_t index) {
  const rng::Block draw = rng::block_at(0xF10A7, 1, index);
  return Config{.seed = rng::u64_of(draw), .elements = draw[2] % 400};
}

TEST(FloatingProperties, ValuesAreNonNegativeAndBelowTheElementCount) {
  for (std::uint64_t index = 0; index < kCases; ++index) {
    const Config config = case_config(index);
    const double value = unit_value(config, index);
    EXPECT_GE(value, 0.0) << "case " << index;
    EXPECT_LE(value, static_cast<double>(config.elements)) << "case " << index;
    EXPECT_FALSE(std::isnan(value));
  }
}

TEST(FloatingProperties, EveryRunIsByteIdenticalToTheSerialOne) {
  for (std::uint64_t index = 0; index < kCases / 4; ++index) {
    const Config config = case_config(index);
    const std::size_t units = 1 + (index % 9);
    SerialExecutor serial;
    ThreadedExecutor threaded(1 + (index % 6));
    std::vector<double> a(units);
    std::vector<double> b(units);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(run(serial, config, a)),
              std::bit_cast<std::uint64_t>(run(threaded, config, b)))
        << "case " << index;
  }
}

TEST(FloatingProperties, AnotherUnitIsAnotherStreamAndGivesAnotherValue) {
  for (std::uint64_t index = 0; index < kCases; ++index) {
    Config config = case_config(index);
    config.elements += 16;
    EXPECT_NE(unit_value(config, 0), unit_value(config, 1)) << "case " << index;
  }
}

}  // namespace
}  // namespace loadforge::primitives::floating
