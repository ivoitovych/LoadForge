// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6: the ray tracing workload is BitExactFixedDecomposition, and the
// specification gives more than the class names: the pixel records do not
// depend on the tile edge, and nothing depends on the thread count, the path
// or the architecture. Each claim is a test.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "oracle/workloads/raytrace/executors.hpp"
#include "workloads/raytrace/raytrace.hpp"

namespace loadforge::tests::raytrace {
namespace {

std::uint64_t fnv1a64(const Bytes& bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (const std::byte byte : bytes) {
    hash = (hash ^ static_cast<std::uint64_t>(byte)) * 0x100000001B3ULL;
  }
  return hash;
}

std::unique_ptr<workloads::Workload> make(simd::IsaPath path, std::uint32_t edge,
                                          std::uint64_t seed) {
  auto created = workloads::raytrace::create(config_for(path, edge, seed));
  EXPECT_TRUE(created.has_value());
  return std::move(created).value();
}

struct GoldenLine {
  std::uint64_t seed = 0;
  std::uint64_t unit = 0;
  std::uint32_t edge = 0;
  std::uint64_t digest = 0;
};

std::vector<GoldenLine> golden_lines() {
  const std::string path =
      std::string(LOADFORGE_SOURCE_DIR) + "/tests/fixtures/workloads/raytrace/golden.txt";
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << path;
  std::vector<GoldenLine> lines;
  std::string text;
  while (std::getline(file, text)) {
    if (text.empty() || text.front() == '#') {
      continue;
    }
    std::istringstream fields(text);
    GoldenLine line;
    std::string digest;
    fields >> line.seed >> line.unit >> line.edge >> digest;
    line.digest = std::stoull(digest, nullptr, 16);
    lines.push_back(line);
  }
  return lines;
}

TEST(RaytraceDeterminism, TheOutputIsWhatAnIndependentModelOfTheSpecificationProduces) {
  // tests/fixtures/workloads/raytrace/reference.py: Python floats, Philox
  // written out again, every primitive for every pixel. Equal digests of the
  // whole output, for each path and the three tile edges and three units
  // across the seed and index ranges, are equal bytes.
  const std::vector<GoldenLine> lines = golden_lines();
  ASSERT_EQ(lines.size(), 9U);
  SerialExecutor serial;
  for (const GoldenLine& line : lines) {
    for (const simd::IsaPath path : runnable_paths()) {
      const auto workload = make(path, line.edge, line.seed);
      const Bytes input = generate_unit(*workload, line.unit);
      const Bytes output = compute_unit(*workload, line.unit, input, serial);
      EXPECT_EQ(fnv1a64(output), line.digest)
          << simd::describe(path) << " seed " << line.seed << " unit " << line.unit << " edge "
          << line.edge;
    }
  }
}

TEST(RaytraceDeterminism, AnyThreadCountOnAnyPathGivesTheSerialScalarBytes) {
  SerialExecutor serial;
  for (const std::uint32_t edge : {8U, 16U, 32U}) {
    const auto reference = make(simd::IsaPath::kScalar, edge, 21);
    for (const std::uint64_t unit : {std::uint64_t{0}, std::uint64_t{13}}) {
      const Bytes input = generate_unit(*reference, unit);
      const Bytes expected = compute_unit(*reference, unit, input, serial);
      for (const simd::IsaPath path : runnable_paths()) {
        const auto workload = make(path, edge, 21);
        for (const std::size_t threads : {1U, 2U, 3U, 4U, 7U, 16U, 64U, 100U}) {
          PoolExecutor pool(threads);
          EXPECT_EQ(compute_unit(*workload, unit, input, pool), expected)
              << simd::describe(path) << " edge " << edge << " unit " << unit << " threads "
              << threads;
        }
      }
    }
  }
}

TEST(RaytraceDeterminism, TheTileEdgeChangesTheSumsAndNothingElse) {
  SerialExecutor serial;
  const auto eight = make(simd::IsaPath::kScalar, 8, 4);
  const auto sixteen = make(simd::IsaPath::kScalar, 16, 4);
  const auto thirty_two = make(simd::IsaPath::kScalar, 32, 4);
  const Bytes input = generate_unit(*eight, 6);
  const Bytes a = compute_unit(*eight, 6, input, serial);
  const Bytes b = compute_unit(*sixteen, 6, input, serial);
  const Bytes c = compute_unit(*thirty_two, 6, input, serial);
  const auto records = [](const Bytes& out) {
    return Bytes(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(rt::kTileSumsOffset));
  };
  EXPECT_EQ(records(a), records(b));
  EXPECT_EQ(records(b), records(c));
  EXPECT_NE(a, b) << "the sums are different sums";
  EXPECT_NE(b, c);
}

TEST(RaytraceDeterminism, RunsRepeatAndWhatWasInTheBuffersBeforeDoesNotMatter) {
  const auto workload = make(runnable_paths().back(), 16, 8);
  PoolExecutor pool(4);
  const Bytes input = generate_unit(*workload, 2);
  const Bytes first = compute_unit(*workload, 2, input, pool, std::byte{0x00});
  for (int run = 0; run < 12; ++run) {
    EXPECT_EQ(compute_unit(*workload, 2, input, pool, run % 2 == 0 ? std::byte{0xFF} : std::byte{0x5A}),
              first)
        << "run " << run;
  }
}

TEST(RaytraceDeterminism, TheUnitIndexIsNotAnInputToCompute) {
  // compute() is a function of its input bytes: the index only names the unit.
  const auto workload = make(simd::IsaPath::kScalar, 16, 2);
  SerialExecutor serial;
  const Bytes input = generate_unit(*workload, 3);
  EXPECT_EQ(compute_unit(*workload, 3, input, serial), compute_unit(*workload, 4, input, serial));
}

}  // namespace
}  // namespace loadforge::tests::raytrace
