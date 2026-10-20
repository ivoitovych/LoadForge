// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6: kBitExactFixedDecomposition (determinism.md §2, §5). The same bits for
// one (cells per cutoff, ISA path) at 1, 2, 3, 4 and 7 threads and on every
// run; across cells per cutoff or across ISA paths, within the derived
// tolerance only; the scalar path the same on x86-64 and ARM64.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "oracle/workloads/particles/direct_check.hpp"
#include "unit/workloads/particles/support.hpp"

namespace loadforge::workloads::particles {
namespace {

using testing::Bytes;

std::uint64_t fnv1a(const Bytes& bytes) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const std::byte b : bytes) {
    hash ^= std::to_integer<std::uint64_t>(b);
    hash *= 1099511628211ULL;
  }
  return hash;
}

TEST(ParticlesDeterminism, EveryThreadCountAndEveryRunGivesTheSameBytesForOneDecomposition) {
  for (const std::uint32_t cells : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      const auto workload = testing::make(testing::config_for(cells, path, 17));
      ASSERT_NE(workload, nullptr);
      for (const std::uint64_t unit : {std::uint64_t{0}, std::uint64_t{3}}) {
        const Bytes input = testing::generate(*workload, unit);
        testing::SerialExecutor serial;
        const Bytes reference = testing::compute(*workload, unit, input, serial);
        for (const std::size_t threads : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                                          std::size_t{4}, std::size_t{7}}) {
          testing::PoolExecutor pool(threads);
          EXPECT_EQ(testing::compute(*workload, unit, input, pool), reference)
              << "c " << cells << " " << simd::describe(path) << " unit " << unit << " threads "
              << threads;
          EXPECT_EQ(testing::compute(*workload, unit, input, pool, std::byte{0xA5}), reference)
              << "a second run, scratch and output prefilled";
        }
      }
    }
  }
}

TEST(ParticlesDeterminism, AFreshWorkloadOfTheSameConfigurationGivesTheSameBytes) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    const auto a = testing::make(testing::config_for(2, path, 5));
    const auto b = testing::make(testing::config_for(2, path, 5));
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    testing::PoolExecutor three(3);
    testing::PoolExecutor seven(7);
    const Bytes input = testing::generate(*a, 2);
    EXPECT_EQ(testing::generate(*b, 2), input);
    EXPECT_EQ(testing::compute(*a, 2, input, three), testing::compute(*b, 2, input, seven));
  }
}

/// Whether two outputs agree within twice the tolerance on the sample of the
/// unit: each is within the tolerance of the direct sum, so they are within
/// twice of one another.
void expect_within_tolerance(const Bytes& input, const Bytes& a, const Bytes& b,
                             const std::string& what) {
  const std::vector<double> positions = testing::as_doubles(input);
  const std::vector<double> first = testing::as_doubles(a);
  const std::vector<double> second = testing::as_doubles(b);
  for (const std::size_t p : direct::sample(1)) {
    const direct::Direct sum = direct::sum_directly(positions, p);
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_LE(std::fabs(first[3 * p + c] - second[3 * p + c]),
                2.0 * direct::tolerance_of(sum, c))
          << what << " particle " << p << " component " << c;
    }
  }
}

TEST(ParticlesDeterminism, DifferentDecompositionsDifferWithinTheToleranceAndNotBeyond) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    Bytes input;
    std::vector<Bytes> outputs;
    for (const std::uint32_t cells : kCellsPerCutoff) {
      const auto workload = testing::make(testing::config_for(cells, path, 8));
      ASSERT_NE(workload, nullptr);
      testing::SerialExecutor serial;
      input = testing::generate(*workload, 1);
      outputs.push_back(testing::compute(*workload, 1, input, serial));
    }
    expect_within_tolerance(input, outputs[0], outputs[1], "c 1 against c 2");
    expect_within_tolerance(input, outputs[1], outputs[2], "c 2 against c 4");
    expect_within_tolerance(input, outputs[0], outputs[2], "c 1 against c 4");
    // The decomposition is part of the answer: the bits are not required to
    // agree, and here they do not.
    EXPECT_NE(outputs[0], outputs[1]) << simd::describe(path);
    EXPECT_NE(outputs[1], outputs[2]) << simd::describe(path);
  }
}

TEST(ParticlesDeterminism, DifferentIsaPathsDifferWithinTheToleranceAndNotBeyond) {
  const auto paths = testing::runnable_paths();
  if (paths.size() < 2) {
    GTEST_SKIP() << "this build or machine has the scalar path only";
  }
  for (const std::uint32_t cells : kCellsPerCutoff) {
    const auto scalar = testing::make(testing::config_for(cells, simd::IsaPath::kScalar, 8));
    const auto vector = testing::make(testing::config_for(cells, paths.back(), 8));
    ASSERT_NE(scalar, nullptr);
    ASSERT_NE(vector, nullptr);
    testing::SerialExecutor serial;
    const Bytes input = testing::generate(*scalar, 1);
    EXPECT_EQ(testing::generate(*vector, 1), input) << "the path never touches the input";
    const Bytes a = testing::compute(*scalar, 1, input, serial);
    const Bytes b = testing::compute(*vector, 1, input, serial);
    expect_within_tolerance(input, a, b, "scalar against vector");
    EXPECT_NE(a, b) << "c " << cells << ": the lane structure is part of the answer";
  }
}

struct Golden {
  std::string path;
  std::uint32_t cells = 0;
  std::uint64_t seed = 0;
  std::uint64_t unit = 0;
  std::uint64_t input_digest = 0;
  std::uint64_t output_digest = 0;
  std::array<std::uint64_t, 3> first{};
};

std::vector<Golden> read_golden() {
  std::ifstream file(std::string(LOADFORGE_SOURCE_DIR) +
                     "/tests/fixtures/determinism/workloads/particles/golden.txt");
  EXPECT_TRUE(file.is_open()) << "the committed fixture";
  std::vector<Golden> rows;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    Golden row;
    fields >> row.path >> row.cells >> row.seed >> row.unit >> std::hex >> row.input_digest >>
        row.output_digest >> row.first[0] >> row.first[1] >> row.first[2];
    EXPECT_FALSE(fields.fail()) << line;
    rows.push_back(row);
  }
  return rows;
}

TEST(ParticlesDeterminism, TheGoldenUnitsKeepTheirBitsOnEveryMachineOfThePathsClass) {
  const std::vector<Golden> rows = read_golden();
  ASSERT_GE(rows.size(), 8U);
  std::size_t scalar_rows = 0;
  for (const Golden& row : rows) {
    simd::IsaPath path = simd::IsaPath::kScalar;
    if (row.path == "avx2") {
      path = simd::IsaPath::kAvx2;
    } else if (row.path == "neon") {
      path = simd::IsaPath::kNeon;
    } else {
      ASSERT_EQ(row.path, "scalar");
      ++scalar_rows;
    }
    const auto paths = testing::runnable_paths();
    if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
      continue;  // a row of a path this machine does not have
    }
    const auto workload = testing::make(testing::config_for(row.cells, path, row.seed));
    ASSERT_NE(workload, nullptr);
    testing::PoolExecutor pool(3);
    const Bytes input = testing::generate(*workload, row.unit);
    const Bytes output = testing::compute(*workload, row.unit, input, pool);
    EXPECT_EQ(fnv1a(input), row.input_digest) << row.path << " c " << row.cells;
    EXPECT_EQ(fnv1a(output), row.output_digest) << row.path << " c " << row.cells;
    std::array<std::uint64_t, 3> first{};
    std::memcpy(first.data(), output.data(), sizeof(first));
    EXPECT_EQ(first, row.first) << row.path << " c " << row.cells;
  }
  EXPECT_EQ(scalar_rows, 4U) << "the scalar rows are checked on every machine";
}

}  // namespace
}  // namespace loadforge::workloads::particles
