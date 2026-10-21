// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for src/primitives/cache: geometry read from the topology (its structs,
// and this machine's real /sys), every refusal of a configuration, the chase's
// cycle structure, an oracle that does not chase, and the executor contract
// run against this module's mock.

#include "primitives/cache/cache.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <vector>

#include "contracts/worker/executor_contract.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "primitives/rng/philox.hpp"
#include "topology/cache.hpp"
#include "topology/cpu_topology.hpp"
#include "unit/primitives/cache/executors.hpp"

namespace loadforge::contracts {
using CacheMockExecutors = ::testing::Types<primitives::cache::mocks::SerialSubject,
                                            primitives::cache::mocks::ThreadedSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(CacheMocks, ExecutorContract, CacheMockExecutors);
}  // namespace loadforge::contracts

namespace loadforge::primitives::cache {
namespace {

using mocks::SerialExecutor;
using mocks::ThreadedExecutor;

topology::Cache make_cache(std::uint32_t level, topology::CacheType type,
                           std::optional<std::uint64_t> size, std::optional<std::uint32_t> line) {
  topology::Cache cache;
  cache.kind = topology::CacheKind{level, type};
  cache.size_bytes = size;
  cache.line_bytes = line;
  return cache;
}

using topology::CacheType;

// --- geometry -----------------------------------------------------------------

TEST(CacheGeometry, ReadsSizeAndLineOfTheLevel) {
  const std::vector<topology::Cache> caches = {make_cache(1, CacheType::kData, 32768, 64),
                                               make_cache(2, CacheType::kUnified, 1048576, 128)};
  const auto l2 = geometry_for_level(caches, 2);
  ASSERT_TRUE(l2.has_value());
  EXPECT_EQ(l2.value().cache_bytes, 1048576U);
  EXPECT_EQ(l2.value().line_bytes, 128U) << "the line size is the machine's, not an assumed 64";
}

TEST(CacheGeometry, SkipsTheInstructionCacheOfALevel) {
  const std::vector<topology::Cache> caches = {make_cache(1, CacheType::kInstruction, 65536, 64),
                                               make_cache(1, CacheType::kData, 32768, 64)};
  const auto l1 = geometry_for_level(caches, 1);
  ASSERT_TRUE(l1.has_value());
  EXPECT_EQ(l1.value().cache_bytes, 32768U);
}

TEST(CacheGeometry, AnAbsentLevelIsAnAnswer) {
  const std::vector<topology::Cache> caches = {make_cache(1, CacheType::kData, 32768, 64),
                                               make_cache(1, CacheType::kInstruction, 32768, 64)};
  const auto l3 = geometry_for_level(caches, 3);
  ASSERT_FALSE(l3.has_value());
  EXPECT_EQ(l3.error(), GeometryError::kLevelAbsent);
  // A level that only has an instruction cache has no data cache to chase.
  const std::vector<topology::Cache> instruction_only = {
      make_cache(1, CacheType::kInstruction, 32768, 64)};
  EXPECT_EQ(geometry_for_level(instruction_only, 1).error(), GeometryError::kLevelAbsent);
  EXPECT_EQ(geometry_for_level({}, 1).error(), GeometryError::kLevelAbsent);
}

TEST(CacheGeometry, AnUnpublishedSizeIsRefusedNotGuessed) {
  const std::vector<topology::Cache> caches = {
      make_cache(2, CacheType::kUnified, std::nullopt, 64)};
  EXPECT_EQ(geometry_for_level(caches, 2).error(), GeometryError::kSizeUnknown);
}

TEST(CacheGeometry, AnUnpublishedOrZeroLineIsRefusedNotGuessed) {
  const std::vector<topology::Cache> missing = {
      make_cache(2, CacheType::kUnified, 1048576, std::nullopt)};
  EXPECT_EQ(geometry_for_level(missing, 2).error(), GeometryError::kLineUnknown);
  const std::vector<topology::Cache> zero = {make_cache(2, CacheType::kUnified, 1048576, 0)};
  EXPECT_EQ(geometry_for_level(zero, 2).error(), GeometryError::kLineUnknown);
}

TEST(CacheGeometry, TheRealMachineGivesAGeometryOrANamedRefusal) {
  // The structs above are what the tests believe; this reads the kernel's.
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem{syscalls};
  auto cpus = topology::CpuTopology::discover(filesystem);
  if (!cpus.has_value()) {
    GTEST_SKIP() << "no readable CPU topology on this machine";
  }
  auto hierarchy = topology::CacheHierarchy::discover(filesystem, cpus.value());
  if (!hierarchy.has_value()) {
    GTEST_SKIP() << "no readable cache topology on this machine";
  }
  const auto geometry = geometry_for_level(hierarchy.value().caches(), 1);
  if (geometry.has_value()) {
    const std::uint32_t line = geometry.value().line_bytes;
    EXPECT_EQ(line & (line - 1), 0U) << "a cache line is a power of two";
    EXPECT_GE(line, 16U);
    EXPECT_LE(line, kMaxLineBytes);
    EXPECT_GT(geometry.value().cache_bytes, 0U);
  }
}

TEST(CacheGeometry, NodesFillAFractionOfTheCacheRoundedDown) {
  const Geometry geometry{.cache_bytes = 32768, .line_bytes = 64};  // 512 lines
  EXPECT_EQ(nodes_in(geometry, 100), 512U);
  EXPECT_EQ(nodes_in(geometry, 50), 256U);
  EXPECT_EQ(nodes_in(geometry, 1), 5U) << "floor(5.12)";
  EXPECT_EQ(nodes_in(geometry, 0), 0U);
  EXPECT_EQ(nodes_in(Geometry{.cache_bytes = 100, .line_bytes = 64}, 100), 1U);
}

TEST(CacheGeometry, NodesInDoesNotOverflowForTheLargestCache) {
  const Geometry geometry{.cache_bytes = ~std::uint64_t{0}, .line_bytes = 1};
  const std::uint64_t lines = ~std::uint64_t{0};
  EXPECT_EQ(nodes_in(geometry, 100), lines);
  EXPECT_EQ(nodes_in(geometry, 50), lines / 2);
}

// --- configuration ------------------------------------------------------------

TEST(CacheConfig, ScratchIsNodesTimesWordsPerLine) {
  const auto words = scratch_words(Config{.seed = 0, .nodes = 10, .line_bytes = 64, .steps = 0});
  ASSERT_TRUE(words.has_value());
  EXPECT_EQ(words.value(), 160U);
}

TEST(CacheConfig, EachRefusalIsNamed) {
  EXPECT_EQ(scratch_words(Config{.nodes = 0, .line_bytes = 64}).error(), ConfigError::kTooFewNodes);
  EXPECT_EQ(scratch_words(Config{.nodes = 1, .line_bytes = 64}).error(), ConfigError::kTooFewNodes);
  EXPECT_EQ(scratch_words(Config{.nodes = kMaxNodes + 1, .line_bytes = 64}).error(),
            ConfigError::kTooManyNodes);
  EXPECT_EQ(scratch_words(Config{.nodes = 2, .line_bytes = 0}).error(),
            ConfigError::kLineBytesInvalid);
  EXPECT_EQ(scratch_words(Config{.nodes = 2, .line_bytes = 6}).error(),
            ConfigError::kLineBytesInvalid);
  EXPECT_EQ(scratch_words(Config{.nodes = 2, .line_bytes = kMaxLineBytes + 4}).error(),
            ConfigError::kLineBytesInvalid);
}

TEST(CacheConfig, TheBoundariesAreAccepted) {
  EXPECT_TRUE(scratch_words(Config{.nodes = kMinNodes, .line_bytes = 4}).has_value());
  EXPECT_TRUE(scratch_words(Config{.nodes = kMaxNodes, .line_bytes = kMaxLineBytes}).has_value());
}

// --- the chase ----------------------------------------------------------------

/// The layout the header defines, read back: node i's successor.
std::vector<std::uint32_t> successors(const Config& config,
                                      const std::vector<std::uint32_t>& scratch) {
  const std::uint64_t stride = config.line_bytes / 4;
  std::vector<std::uint32_t> next(config.nodes);
  for (std::uint64_t node = 0; node < config.nodes; ++node) {
    next[node] = scratch[node * stride];
  }
  return next;
}

/// The oracle's cycle, built without chasing through memory: Sattolo's
/// algorithm on a plain vector, then the order of visits read off it.
std::vector<std::uint32_t> visit_order(const Config& config, std::uint64_t unit) {
  std::vector<std::uint32_t> next(config.nodes);
  std::iota(next.begin(), next.end(), 0U);
  for (std::uint64_t node = config.nodes - 1; node >= 1; --node) {
    const std::uint64_t other = rng::u64_of(rng::block_at(config.seed, unit, node)) % node;
    std::swap(next[node], next[other]);
  }
  std::vector<std::uint32_t> order;
  std::uint32_t at = 0;
  for (std::uint64_t i = 0; i < config.nodes; ++i) {
    order.push_back(at);
    at = next[at];
  }
  return order;
}

TEST(CacheChase, NodesFormOneCycleThroughEveryNode) {
  for (const std::uint64_t nodes : {2ULL, 3ULL, 4ULL, 17ULL, 256ULL}) {
    const Config config{.seed = 5, .nodes = nodes, .line_bytes = 64, .steps = 0};
    std::vector<std::uint32_t> scratch(scratch_words(config).value(), 0xDEADBEEFU);
    (void)unit_digest(config, 1, scratch);
    const std::vector<std::uint32_t> next = successors(config, scratch);
    std::vector<bool> seen(nodes, false);
    std::uint32_t at = 0;
    for (std::uint64_t i = 0; i < nodes; ++i) {
      EXPECT_FALSE(seen[at]) << "node " << at << " visited twice before the cycle closed";
      seen[at] = true;
      at = next[at];
    }
    EXPECT_EQ(at, 0U) << "the cycle returns to the start after exactly `nodes` steps";
    EXPECT_TRUE(std::all_of(seen.begin(), seen.end(), [](bool v) { return v; }));
  }
}

TEST(CacheChase, OnlyTheFirstWordOfEachNodeIsTouched) {
  const Config config{.seed = 1, .nodes = 9, .line_bytes = 32, .steps = 4};
  std::vector<std::uint32_t> scratch(scratch_words(config).value(), 0xDEADBEEFU);
  (void)unit_digest(config, 0, scratch);
  for (std::size_t word = 0; word < scratch.size(); ++word) {
    if (word % 8 != 0) {
      EXPECT_EQ(scratch[word], 0xDEADBEEFU) << "word " << word;
    }
  }
}

TEST(CacheChase, TheDigestIsWhereTheChaseEndsAndMatchesTheOracle) {
  for (const std::uint64_t line : {4ULL, 16ULL, 64ULL, 128ULL}) {
    for (const std::uint64_t steps : {0ULL, 1ULL, 2ULL, 6ULL, 7ULL, 14ULL, 100ULL}) {
      const Config config{
          .seed = 77, .nodes = 7, .line_bytes = static_cast<std::uint32_t>(line), .steps = steps};
      std::vector<std::uint32_t> scratch(scratch_words(config).value());
      const std::vector<std::uint32_t> order = visit_order(config, 3);
      EXPECT_EQ(unit_digest(config, 3, scratch), order[steps % config.nodes])
          << "line " << line << " steps " << steps;
    }
  }
}

TEST(CacheChase, WholeLapsReturnToTheStart) {
  const Config config{.seed = 9, .nodes = 13, .line_bytes = 64, .steps = std::uint64_t{13} * 11};
  std::vector<std::uint32_t> scratch(scratch_words(config).value());
  EXPECT_EQ(unit_digest(config, 0, scratch), 0U);
}

TEST(CacheChase, TheDigestDoesNotDependOnTheLineSize) {
  Config config{.seed = 4, .nodes = 50, .line_bytes = 16, .steps = 1234};
  std::vector<std::uint32_t> small(scratch_words(config).value());
  const std::uint64_t at_16 = unit_digest(config, 2, small);
  config.line_bytes = 256;
  std::vector<std::uint32_t> large(scratch_words(config).value());
  EXPECT_EQ(unit_digest(config, 2, large), at_16);
}

// --- run ----------------------------------------------------------------------

TEST(CacheRun, RefusesABadConfigurationAndRunsNothing) {
  SerialExecutor executor;
  std::vector<std::uint32_t> scratch(1000);
  std::vector<std::uint64_t> partials(3, 0xAAAA);
  const auto result = run(executor, Config{.nodes = 1, .line_bytes = 64}, scratch, partials);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConfigError::kTooFewNodes);
  EXPECT_EQ(partials, std::vector<std::uint64_t>(3, 0xAAAA));
}

TEST(CacheRun, RefusesScratchSmallerThanEverySlotNeeds) {
  ThreadedExecutor executor(3);
  const Config config{.seed = 1, .nodes = 10, .line_bytes = 64, .steps = 5};  // 160 words a slot
  std::vector<std::uint32_t> scratch(3 * 160 - 1);
  std::vector<std::uint64_t> partials(4, 0xAAAA);
  const auto result = run(executor, config, scratch, partials);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), ConfigError::kScratchTooSmall);
  EXPECT_EQ(partials, std::vector<std::uint64_t>(4, 0xAAAA));
  scratch.resize(std::size_t{3} * 160);
  EXPECT_TRUE(run(executor, config, scratch, partials).has_value()) << "exactly enough is enough";
}

TEST(CacheRun, WritesEachUnitsDigestAndSumsThemInOrder) {
  ThreadedExecutor executor(3);
  const Config config{.seed = 31, .nodes = 33, .line_bytes = 64, .steps = 500};
  std::vector<std::uint32_t> scratch(3 * scratch_words(config).value());
  std::vector<std::uint64_t> partials(10, 0);
  const auto total = run(executor, config, scratch, partials);
  ASSERT_TRUE(total.has_value());
  std::uint64_t expected = 0;
  for (std::uint64_t unit = 0; unit < partials.size(); ++unit) {
    EXPECT_EQ(partials[unit], visit_order(config, unit)[config.steps % config.nodes]) << unit;
    expected += partials[unit];
  }
  EXPECT_EQ(total.value(), expected);
}

TEST(CacheRun, NoUnitsIsZero) {
  SerialExecutor executor;
  const Config config{.seed = 1, .nodes = 4, .line_bytes = 8, .steps = 3};
  std::vector<std::uint32_t> scratch(scratch_words(config).value());
  std::vector<std::uint64_t> none;
  const auto total = run(executor, config, scratch, none);
  ASSERT_TRUE(total.has_value());
  EXPECT_EQ(total.value(), 0U);
}

}  // namespace
}  // namespace loadforge::primitives::cache
