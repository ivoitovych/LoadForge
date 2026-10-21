// SPDX-License-Identifier: GPL-3.0-or-later
//
// T6: the declared class, BitExactFixedDecomposition (determinism.md §2), held
// to account. For one radix and the scalar path: the same bits at any thread
// count, on any run, whatever the output and scratch held before, and in any
// order of units. Across radices: the same values within the derived tolerance,
// and not the same bits (which is what separates this class from BitExact).
// And the scalar path's bits pinned by digests committed below, which are the
// same on x86-64 and ARM64 because the table is the same bits and nothing in
// the scalar path is fused or transcendental.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "oracle/workloads/fft/fft_support.hpp"
#include "primitives/rng/philox.hpp"
#include "workloads/fft/fft.hpp"
#include "workloads/fft/spec.hpp"

namespace loadforge::workloads::fft {
namespace {

using testing::Bytes;

constexpr std::uint64_t kSeed = 20261020;

std::unique_ptr<Workload> made(std::uint32_t radix) {
  auto created = create(
      WorkloadConfig{kSeed, 16 * (kInputBytes + kOutputBytes), radix, simd::IsaPath::kScalar});
  EXPECT_TRUE(created.has_value());
  return std::move(created).value();
}

Bytes unit_input(const Workload& workload, std::uint64_t unit) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(unit, input);
  return input;
}

std::uint64_t fnv1a(const Bytes& bytes) {
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (const std::byte b : bytes) {
    hash = (hash ^ std::to_integer<std::uint64_t>(b)) * 0x100000001B3ULL;
  }
  return hash;
}

TEST(FftDeterminism, TheDeclaredClassIsTheOneTheAlgorithmSupports) {
  EXPECT_EQ(made(4)->descriptor().determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(made(2)->descriptor().determinism, DeterminismClass::kBitExactFixedDecomposition);
  EXPECT_EQ(made(4)->descriptor().contract, verification::Contract::kBounded);
}

TEST(FftDeterminism, AnyThreadCountGivesTheBitsOfOne) {
  for (const std::uint32_t radix : kRadices) {
    const auto workload = made(radix);
    for (const std::uint64_t unit : {std::uint64_t{0}, std::uint64_t{9}, std::uint64_t{123456}}) {
      const Bytes input = unit_input(*workload, unit);
      testing::Threads one(1);
      const Bytes reference = testing::compute_unit(*workload, unit, input, one);
      for (const std::size_t threads : {2U, 3U, 4U, 5U, 7U, 8U, 16U}) {
        testing::Threads many(threads);
        EXPECT_EQ(testing::compute_unit(*workload, unit, input, many), reference)
            << "radix " << radix << " unit " << unit << " threads " << threads;
      }
    }
  }
}

TEST(FftDeterminism, ThirtyRunsAtSevenThreadsGiveOneAnswer) {
  for (const std::uint32_t radix : kRadices) {
    const auto workload = made(radix);
    const Bytes input = unit_input(*workload, 4);
    testing::Threads seven(7);
    const Bytes first = testing::compute_unit(*workload, 4, input, seven);
    for (int run = 0; run < 30; ++run) {
      ASSERT_EQ(testing::compute_unit(*workload, 4, input, seven), first)
          << "radix " << radix << " run " << run;
    }
  }
}

TEST(FftDeterminism, WhatTheOutputAndScratchHeldBeforeNeverReachesTheResult) {
  for (const std::uint32_t radix : kRadices) {
    const auto workload = made(radix);
    const Bytes input = unit_input(*workload, 6);
    testing::Threads three(3);
    const Bytes reference = testing::compute_unit(*workload, 6, input, three);
    Bytes noise(workload->plan().output_bytes + workload->plan().scratch_bytes * 3);
    for (const std::uint64_t stream : {1U, 2U, 3U}) {
      rng::fill_stream(77, stream, noise);
      Bytes output(noise.begin(),
                   noise.begin() + static_cast<std::ptrdiff_t>(workload->plan().output_bytes));
      Bytes scratch(noise.begin() + static_cast<std::ptrdiff_t>(workload->plan().output_bytes),
                    noise.end());
      workload->compute(6, input, output, scratch, three);
      EXPECT_EQ(output, reference) << "radix " << radix << " noise stream " << stream;
    }
  }
}

TEST(FftDeterminism, TheOrderUnitsAreComputedInNeverChangesAnyOfThem) {
  const auto workload = made(4);
  testing::Threads two(2);
  std::vector<Bytes> forward;
  for (std::uint64_t unit = 0; unit < 6; ++unit) {
    forward.push_back(testing::compute_unit(*workload, unit, unit_input(*workload, unit), two));
  }
  for (std::uint64_t unit = 6; unit-- > 0;) {
    EXPECT_EQ(testing::compute_unit(*workload, unit, unit_input(*workload, unit), two),
              forward[unit])
        << unit;
  }
}

TEST(FftDeterminism, TheOutputDoesNotDependOnTheUnitIndexOnlyOnTheInput) {
  const auto workload = made(4);
  const Bytes input = unit_input(*workload, 2);
  testing::Threads one(1);
  EXPECT_EQ(testing::compute_unit(*workload, 2, input, one),
            testing::compute_unit(*workload, 99, input, one));
}

TEST(FftDeterminism, TheRadicesAgreeWithinToleranceAndDifferInBits) {
  const auto two = made(2);
  const auto four = made(4);
  testing::Threads one(1);
  bool differed = false;
  for (std::uint64_t unit = 0; unit < 4; ++unit) {
    const Bytes input = unit_input(*two, unit);
    const Bytes a = testing::compute_unit(*two, unit, input, one);
    const Bytes b = testing::compute_unit(*four, unit, input, one);
    differed = differed || a != b;
    const std::vector<double> ya = testing::to_doubles(a);
    const std::vector<double> yb = testing::to_doubles(b);
    const double bound = 64.0 * testing::norm2(testing::to_doubles(input)) *
                         (bin_tolerance_factor(2) + bin_tolerance_factor(4));
    for (std::size_t e = 0; e < ya.size(); ++e) {
      ASSERT_LE(std::fabs(ya[e] - yb[e]), bound) << unit << " " << e;
    }
  }
  EXPECT_TRUE(differed) << "if the radices agreed bit for bit the class would be BitExact";
}

struct Golden {
  std::uint32_t radix;
  std::uint64_t unit;
  std::uint64_t input_digest;
  std::uint64_t output_digest;
};

// Produced on x86-64 Linux with GCC 13 (tests/fixtures/fft/README.md); the scalar
// path gives the same bits on ARM64, which CI checks. A regression tripwire,
// not an oracle: the bits are right because T3 says the values are.
constexpr Golden kGolden[] = {
    {2, 0, 13255534020785674584ULL, 6574358102540520800ULL},
    {2, 1234567, 12102222353049984845ULL, 871669719530497737ULL},
    {4, 0, 13255534020785674584ULL, 15651231412752647548ULL},
    {4, 1234567, 12102222353049984845ULL, 8598499248818544589ULL},
};

TEST(FftDeterminism, TheScalarPathReproducesTheCommittedDigests) {
  testing::Threads one(1);
  for (const Golden& golden : kGolden) {
    const auto workload = made(golden.radix);
    const Bytes input = unit_input(*workload, golden.unit);
    const Bytes output = testing::compute_unit(*workload, golden.unit, input, one);
    EXPECT_EQ(fnv1a(input), golden.input_digest)
        << "radix " << golden.radix << " unit " << golden.unit;
    EXPECT_EQ(fnv1a(output), golden.output_digest)
        << "radix " << golden.radix << " unit " << golden.unit;
  }
}

}  // namespace
}  // namespace loadforge::workloads::fft
