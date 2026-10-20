// SPDX-License-Identifier: GPL-3.0-or-later
// T6: kBitExactFixedDecomposition, enforced (determinism.md §5). For one chunk
// length and one ISA path the bits are the same at 1, 2, 3, 4 and 7 threads
// and on every run; across chunk lengths and across paths they differ only
// within the specification's rounding bound; and the scalar order is pinned
// by golden sums made by an implementation in another language
// (tests/fixtures/workloads/monte_carlo/make_golden.py), the same on x86-64
// and ARM64.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "oracle/workloads/monte_carlo/replay.hpp"
#include "primitives/simd/dispatch.hpp"
#include "unit/workloads/monte_carlo/support.hpp"
#include "workloads/monte_carlo/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::monte_carlo {
namespace {

using testing::compute_unit;
using testing::generate_unit;
using testing::lanes_of;
using testing::make;
using testing::native_path;
using testing::SerialExecutor;
using testing::ThreadedExecutor;

constexpr std::uint64_t kSeed = 0xC0FFEE;
constexpr std::uint64_t kUnit = 3;

TEST(Determinism, ForOneChunkAndPathTheBitsAreThoseOfEveryThreadCountAndRun) {
  SerialExecutor serial;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, native_path()}) {
    for (const std::uint32_t chunk : {64U, kDefaultChunkLength, 16384U}) {
      const auto workload = make(kSeed, chunk, path);
      ASSERT_NE(workload, nullptr);
      const std::vector<std::byte> input = generate_unit(*workload, kUnit);
      const std::vector<std::byte> expected = compute_unit(*workload, kUnit, input, serial);
      EXPECT_EQ(compute_unit(*workload, kUnit, input, serial), expected) << "a second run";
      for (const std::size_t threads : {2U, 3U, 4U, 7U}) {
        ThreadedExecutor executor(threads);
        EXPECT_EQ(compute_unit(*workload, kUnit, input, executor), expected)
            << simd::describe(path) << " chunk " << chunk << " threads " << threads;
      }
    }
  }
}

TEST(Determinism, AcrossChunkLengthsAndPathsTheSumsDifferOnlyWithinTheBound) {
  SerialExecutor serial;
  const auto base = make(kSeed, 0, simd::IsaPath::kScalar);
  ASSERT_NE(base, nullptr);
  const std::vector<std::byte> input = generate_unit(*base, kUnit);
  const std::vector<std::byte> baseline = compute_unit(*base, kUnit, input, serial);
  bool some_difference = false;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, native_path()}) {
    for (const std::uint32_t chunk : kChunkLengths) {
      const auto workload = make(kSeed, chunk, path);
      ASSERT_NE(workload, nullptr);
      const std::vector<std::byte> output = compute_unit(*workload, kUnit, input, serial);
      some_difference = some_difference || output != baseline;
      for (std::uint64_t batch = 0; batch < kBatches; ++batch) {
        const double a =
            replay::load_double(std::span<const std::byte>(baseline).subspan(batch * 8, 8));
        const double b =
            replay::load_double(std::span<const std::byte>(output).subspan(batch * 8, 8));
        // The bound with r_b replaced by either result (spec.hpp).
        EXPECT_LE(std::fabs(a - b), tolerance(std::max(a, b)))
            << simd::describe(path) << " chunk " << chunk << " batch " << batch;
      }
    }
  }
  EXPECT_TRUE(some_difference) << "if no order ever differed the test would show nothing";
}

TEST(Determinism, TheToleranceIsDerivedFromTheSampleCountAndTheRoundoff) {
  EXPECT_EQ(kToleranceScale, 2.0 * static_cast<double>(kSamplesPerBatch) * 0x1p-53);
  EXPECT_EQ(kToleranceScale, 0x1p-38);
}

using Golden = std::map<std::tuple<unsigned, std::uint32_t, std::uint64_t>, std::uint64_t>;

Golden read_golden() {
  std::ifstream file(std::string(LOADFORGE_SOURCE_DIR) +
                     "/tests/fixtures/workloads/monte_carlo/golden.txt");
  Golden golden;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    unsigned lanes = 0;
    std::uint32_t chunk = 0;
    std::uint64_t batch = 0;
    std::string hex;
    fields >> lanes >> chunk >> batch >> hex;
    golden[{lanes, chunk, batch}] = std::stoull(hex, nullptr, 16);
  }
  return golden;
}

TEST(Determinism, EachPathReproducesTheGoldenSumsOfItsOrderBitForBit) {
  const Golden golden = read_golden();
  ASSERT_EQ(golden.size(), 3U * kChunkLengths.size() * kBatches) << "the fixture was read";
  SerialExecutor serial;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, native_path()}) {
    for (const std::uint32_t chunk : kChunkLengths) {
      const auto workload = make(kSeed, chunk, path);
      ASSERT_NE(workload, nullptr);
      const std::vector<std::byte> input = generate_unit(*workload, kUnit);
      const std::vector<std::byte> output = compute_unit(*workload, kUnit, input, serial);
      for (std::uint64_t batch = 0; batch < kBatches; ++batch) {
        const auto found = golden.find({lanes_of(path), chunk, batch});
        ASSERT_NE(found, golden.end());
        const double sum =
            replay::load_double(std::span<const std::byte>(output).subspan(batch * 8, 8));
        EXPECT_EQ(std::bit_cast<std::uint64_t>(sum), found->second)
            << simd::describe(path) << " chunk " << chunk << " batch " << batch;
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::monte_carlo
