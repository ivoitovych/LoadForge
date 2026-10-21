// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6: the class the workload declares, BitExactFixedDecomposition
// (determinism.md §2, §5), enforced.
//   * For one tile edge and one ISA path, the same bits at 1, 2, 3, 4 and 7
//     threads, under executors that deal the tiles out in different ways, in
//     reverse order, and run after run.
//   * Across tile edges and across ISA paths, within the derived tolerance.
//   * The scalar path, identical on x86-64 and ARM64: a committed digest of
//     what it computed, checked on whichever machine runs this.
//
// A finding the specification invites a stream to record (spec.hpp,
// "DECOMPOSITION"): these kernels give the same bits for every tile edge and
// every path, which is stronger than the declared class. The test that pins it
// is named for what it is, so that a kernel change that gives it up fails
// there and points at the contract question in the journal, not at the class.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "core/crc32.hpp"
#include "unit/workloads/stencil/stencil_support.hpp"
#include "workloads/stencil/spec.hpp"

namespace loadforge::workloads::stencil {
namespace {

using testing::Bytes;

constexpr std::uint32_t kTiles[] = {16, 32, 64, 128};

Bytes with(simd::IsaPath path, std::uint32_t tile, std::uint64_t seed, std::uint64_t index,
           worker::Executor& executor) {
  const auto workload = testing::make(testing::config_for(path, tile, 4, seed));
  const Bytes input = testing::generated(*workload, index);
  return testing::computed(*workload, index, input, executor);
}

TEST(StencilDeterminism, OneTileEdgeAndPathGiveTheSameBitsAtAnyThreadCountAndOrder) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    for (const std::uint32_t tile : kTiles) {
      testing::SerialExecutor serial;
      const Bytes reference = with(path, tile, 0x5EED, 1, serial);
      for (const std::size_t threads : {1U, 2U, 3U, 4U, 7U}) {
        testing::ThreadedExecutor interleaved(threads, true);
        testing::ThreadedExecutor blocks(threads, false);
        EXPECT_EQ(with(path, tile, 0x5EED, 1, interleaved), reference)
            << simd::describe(path) << " tile " << tile << " threads " << threads << " interleaved";
        EXPECT_EQ(with(path, tile, 0x5EED, 1, blocks), reference)
            << simd::describe(path) << " tile " << tile << " threads " << threads << " blocks";
      }
      testing::ReverseExecutor reverse;
      EXPECT_EQ(with(path, tile, 0x5EED, 1, reverse), reference)
          << simd::describe(path) << " tile " << tile << " reversed";
    }
  }
}

TEST(StencilDeterminism, RepeatRunsAtOneCountAreIdentical) {
  for (const simd::IsaPath path : testing::runnable_paths()) {
    testing::ThreadedExecutor three(3);
    const Bytes first = with(path, 0, 9, 4, three);
    for (int repeat = 0; repeat < 5; ++repeat) {
      EXPECT_EQ(with(path, 0, 9, 4, three), first) << simd::describe(path) << " run " << repeat;
    }
  }
}

TEST(StencilDeterminism, AcrossTileEdgesAndPathsTheOutputStaysWithinTheDerivedTolerance) {
  testing::SerialExecutor serial;
  const Bytes reference = with(simd::IsaPath::kScalar, kDefaultTileEdge, 0x5EED, 2, serial);
  const Bytes input = testing::spec_input(0x5EED, 2);
  // Two computations, each within M0 gamma_{4T} of the exact value.
  const double bound = cell_tolerance_factor() * testing::max_magnitude(input);
  for (const simd::IsaPath path : testing::runnable_paths()) {
    for (const std::uint32_t tile : kTiles) {
      const Bytes other = with(path, tile, 0x5EED, 2, serial);
      double largest = 0.0;
      for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
        largest =
            std::max(largest, std::fabs(testing::cell_of(other, cell / kEdge, cell % kEdge) -
                                        testing::cell_of(reference, cell / kEdge, cell % kEdge)));
      }
      EXPECT_LE(largest, bound) << simd::describe(path) << " tile " << tile;
    }
  }
}

TEST(StencilDeterminism, FindingTheseKernelsGiveTheSameBitsForEveryTileEdgeAndPath) {
  testing::SerialExecutor serial;
  for (const std::uint64_t seed : {std::uint64_t{0x5EED}, std::uint64_t{99}}) {
    const Bytes reference = with(simd::IsaPath::kScalar, kDefaultTileEdge, seed, 0, serial);
    for (const simd::IsaPath path : testing::runnable_paths()) {
      for (const std::uint32_t tile : kTiles) {
        EXPECT_EQ(with(path, tile, seed, 0, serial), reference)
            << simd::describe(path) << " tile " << tile << " seed " << seed;
      }
    }
  }
}

TEST(StencilDeterminism, TheDeclaredClassIsTheOneTheSpecificationNames) {
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar));
  EXPECT_EQ(workload->descriptor().determinism, DeterminismClass::kBitExactFixedDecomposition);
}

// ---- the scalar path across architectures ----------------------------------------

struct Golden {
  std::uint64_t seed = 0;
  std::uint64_t index = 0;
  std::uint32_t tile = 0;
  std::uint32_t input_crc = 0;
  std::uint32_t output_crc = 0;
};

std::uint32_t crc_of(const Bytes& bytes) { return core::crc32(bytes); }

Golden compute_golden(std::uint64_t seed, std::uint64_t index, std::uint32_t tile) {
  testing::SerialExecutor serial;
  const auto workload = testing::make(testing::config_for(simd::IsaPath::kScalar, tile, 4, seed));
  const Bytes input = testing::generated(*workload, index);
  const Bytes output = testing::computed(*workload, index, input, serial);
  return {seed, index, tile, crc_of(input), crc_of(output)};
}

std::vector<Golden> read_goldens() {
  std::ifstream file(std::string(LOADFORGE_SOURCE_DIR) +
                     "/tests/fixtures/determinism/workloads/stencil/scalar-crc32.txt");
  std::vector<Golden> goldens;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line.front() == '#') {
      continue;
    }
    std::istringstream fields(line);
    Golden golden;
    fields >> golden.seed >> golden.index >> golden.tile >> std::hex >> golden.input_crc >>
        golden.output_crc;
    goldens.push_back(golden);
  }
  return goldens;
}

TEST(StencilDeterminism, TheScalarPathReproducesTheCommittedDigestsOnThisArchitecture) {
  if (std::getenv("LOADFORGE_STENCIL_PRINT_GOLDEN") != nullptr) {
    // The way the fixture is regenerated: see its header.
    for (const std::uint32_t tile : kTiles) {
      for (const auto& [seed, index] :
           {std::pair<std::uint64_t, std::uint64_t>{1, 0}, {0x5EED, 7}, {~std::uint64_t{0}, 3}}) {
        const Golden golden = compute_golden(seed, index, tile);
        std::cout << golden.seed << " " << golden.index << " " << golden.tile << " " << std::hex
                  << golden.input_crc << " " << golden.output_crc << std::dec << "\n";
      }
    }
  }
  const std::vector<Golden> goldens = read_goldens();
  ASSERT_EQ(goldens.size(), 12U) << "the fixture is missing or truncated";
  for (const Golden& golden : goldens) {
    const Golden now = compute_golden(golden.seed, golden.index, golden.tile);
    EXPECT_EQ(now.input_crc, golden.input_crc) << golden.seed << "/" << golden.index;
    EXPECT_EQ(now.output_crc, golden.output_crc)
        << "seed " << golden.seed << " unit " << golden.index << " tile " << golden.tile;
  }
}

}  // namespace
}  // namespace loadforge::workloads::stencil
