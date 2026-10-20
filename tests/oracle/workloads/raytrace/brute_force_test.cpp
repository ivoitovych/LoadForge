// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3: the workload's bytes against brute force (reference.hpp), for every path
// this machine runs and every tile edge. The hierarchy, the packets and the
// kernels are the things under test; the arithmetic is the specification's.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "oracle/workloads/raytrace/executors.hpp"
#include "workloads/raytrace/raytrace.hpp"
#include "workloads/raytrace/spec.hpp"

namespace loadforge::tests::raytrace {
namespace {

std::unique_ptr<workloads::Workload> make(simd::IsaPath path, std::uint32_t edge,
                                          std::uint64_t seed) {
  auto created = workloads::raytrace::create(config_for(path, edge, seed));
  EXPECT_TRUE(created.has_value());
  return std::move(created).value();
}

TEST(RaytraceBruteForce, EveryByteOfEveryUnitMatchesForEveryPathAndTileEdge) {
  SerialExecutor serial;
  for (const simd::IsaPath path : runnable_paths()) {
    for (const std::uint32_t edge : {0U, 8U, 16U, 32U}) {
      for (const std::uint64_t seed : {1ULL, 0xDEADBEEFULL}) {
        const auto workload = make(path, edge, seed);
        for (std::uint64_t index = 0; index < 6; ++index) {
          const Bytes input = generate_unit(*workload, index);
          const Bytes output = compute_unit(*workload, index, input, serial);
          const Comparison comparison =
              compare_with_brute_force(input, output, workload->plan().decomposition);
          EXPECT_EQ(comparison.verdict, Verdict::kAgreement)
              << simd::describe(path) << " edge " << edge << " seed " << seed << " unit " << index
              << ": " << comparison.pixels.size() << " pixels and " << comparison.tiles.size()
              << " tiles differ";
        }
      }
    }
  }
}

TEST(RaytraceBruteForce, ASingleValueReplacedByAnotherInRangeStillMatches) {
  SerialExecutor serial;
  for (const simd::IsaPath path : runnable_paths()) {
    const auto workload = make(path, 16, 7);
    const Bytes clean = generate_unit(*workload, 3);
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    for (int round = 0; round < 24; ++round) {
      Bytes input = clean;
      // A sphere's centre or radius, or a triangle's vertex, moved somewhere
      // else in its range: a scene the specification generates, a different one.
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const std::size_t sphere = (state >> 33U) % rt::kSpheres;
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      const double shifted = rt::position_xy(state);
      set_double(input, sphere * rt::kSphereDoubles, shifted);
      set_double(input, sphere * rt::kSphereDoubles + 1, -shifted * 0.5);
      const Bytes output = compute_unit(*workload, 3, input, serial);
      EXPECT_EQ(compare_with_brute_force(input, output, 16).verdict, Verdict::kAgreement)
          << simd::describe(path) << " round " << round;
    }
  }
}

TEST(RaytraceBruteForce, ABitFlippedAnywhereInTheInputStillMatchesWhereFinite) {
  // A flipped bit may leave the specification's ranges (the oracle then says
  // NotChecked) but the hierarchy must still give brute force's answer on any
  // finite scene the box arithmetic cannot overflow: the box test is
  // conservative for all of them.
  SerialExecutor serial;
  std::size_t compared = 0;
  for (const simd::IsaPath path : runnable_paths()) {
    const auto workload = make(path, 16, 11);
    const Bytes clean = generate_unit(*workload, 1);
    for (std::size_t byte = 0; byte < clean.size(); byte += 977) {
      for (const unsigned bit : {21U, 51U}) {
        Bytes input = clean;
        input[byte] ^= static_cast<std::byte>(1U << (bit % 8U));
        const double touched = double_at(input, byte / 8);
        if (!(touched > -1e6 && touched < 1e6)) {
          continue;  // a huge exponent: the box arithmetic leaves the range of the claim
        }
        const Bytes output = compute_unit(*workload, 1, input, serial);
        EXPECT_EQ(compare_with_brute_force(input, output, 16).verdict, Verdict::kAgreement)
            << simd::describe(path) << " byte " << byte << " bit " << bit;
        ++compared;
      }
    }
  }
  EXPECT_GT(compared, 100U);
}

}  // namespace
}  // namespace loadforge::tests::raytrace
