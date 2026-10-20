// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b. The graph workload is what the verifier checks, not a verifier,
// so its obligations here are the ones a checker depends on:
//
//   T5   a corruption injected into the unit's input or its output is
//        OBSERVABLE in the output the checker compares, and a single flipped
//        bit is classified as a single bit (verification/record.hpp's own
//        classification is applied to the workload-shaped bytes). Also the one
//        corruption that is NOT observable in the output, a bit of a vertex
//        number that the specification reduces away, is pinned: that
//        corruption is the input-integrity bracket's to find (spec.hpp,
//        TOTALITY), and this test says so in executable form.
//   T5b  a unit whose compute() has not run cannot be mistaken for one that
//        has: whatever the output buffer held -- another unit's correct
//        answer, included -- does not survive compute(), and neither does
//        what the scratch held.

#include <gtest/gtest.h>

#include <bit>

#include "graph_support.hpp"
#include "verification/record.hpp"

namespace loadforge::graph_test {
namespace {

using verification::BitClass;

struct Diff {
  std::uint64_t first = 0;
  std::uint64_t last = 0;
  std::uint64_t popcount = 0;
  [[nodiscard]] bool any() const { return popcount != 0; }
  [[nodiscard]] BitClass bit_class() const {
    return verification::classify_bits(popcount, first, last, 64);
  }
};

Diff diff_of(const Bytes& expected, const Bytes& actual) {
  Diff diff;
  bool seen = false;
  for (std::size_t at = 0; at < expected.size(); ++at) {
    const auto delta = static_cast<unsigned>(expected[at] ^ actual[at]);
    if (delta == 0) {
      continue;
    }
    if (!seen) {
      diff.first = at;
      seen = true;
    }
    diff.last = at;
    diff.popcount += static_cast<std::uint64_t>(std::popcount(delta));
  }
  return diff;
}

struct Fixture {
  std::unique_ptr<workloads::Workload> workload = make(config_for(0, simd::IsaPath::kScalar));
  Bytes input = reference_input(4, 4);
  Bytes clean = run_serial(*workload, input);
};

void flip(Bytes& bytes, std::size_t byte, unsigned bit) {
  bytes[byte] ^= static_cast<std::byte>(1U << bit);
}

TEST(GraphFaultInjection, ASingleBitFlippedInTheDistancesIsClassifiedAsOneBit) {
  const Fixture f;
  for (const std::size_t byte : {std::size_t{0}, std::size_t{777}, spec::kRanksOffset - 1}) {
    Bytes bad = f.clean;
    flip(bad, byte, 3);
    const Diff diff = diff_of(f.clean, bad);
    EXPECT_EQ(diff.popcount, 1U);
    EXPECT_EQ(diff.first, byte);
    EXPECT_LT(diff.first, spec::kRanksOffset) << "the distances section is named";
    EXPECT_EQ(diff.bit_class(), BitClass::kSingleBit);
  }
}

TEST(GraphFaultInjection, ASingleBitFlippedInTheRanksIsClassifiedAsOneBit) {
  const Fixture f;
  for (const std::size_t byte : {spec::kRanksOffset, spec::kRanksOffset + 4097,
                                 spec::kOutputBytes - 1}) {
    for (const unsigned bit : {0U, 7U}) {
      Bytes bad = f.clean;
      flip(bad, byte, bit);
      const Diff diff = diff_of(f.clean, bad);
      EXPECT_EQ(diff.popcount, 1U);
      EXPECT_GE(diff.first, spec::kRanksOffset) << "the ranks section is named";
      EXPECT_EQ(diff.bit_class(), BitClass::kSingleBit);
    }
  }
}

TEST(GraphFaultInjection, SeveralBitsAreClassifiedByWhereTheyLie) {
  const Fixture f;
  Bytes in_word = f.clean;
  flip(in_word, spec::kRanksOffset + 8, 0);
  flip(in_word, spec::kRanksOffset + 11, 5);
  EXPECT_EQ(diff_of(f.clean, in_word).bit_class(), BitClass::kMultiBitInWord);
  Bytes in_line = f.clean;
  flip(in_line, spec::kRanksOffset, 0);
  flip(in_line, spec::kRanksOffset + 40, 0);
  EXPECT_EQ(diff_of(f.clean, in_line).bit_class(), BitClass::kWithinCacheLine);
  Bytes across = f.clean;
  flip(across, spec::kRanksOffset, 0);
  flip(across, spec::kRanksOffset + 64, 0);
  EXPECT_EQ(diff_of(f.clean, across).bit_class(), BitClass::kAcrossCacheLines);
}

TEST(GraphFaultInjection, EveryLowBitOfAHeadWordChangesTheOutputWhenTheInputIsCorrupted) {
  const Fixture f;
  for (const std::size_t edge : {std::size_t{0}, std::size_t{12345}, spec::kEdges - 1}) {
    for (unsigned bit = 0; bit < 15; ++bit) {
      Bytes bad = f.input;
      flip(bad, edge * 4 + bit / 8, bit % 8);
      const Bytes output = run_serial(*f.workload, bad);
      EXPECT_TRUE(diff_of(f.clean, output).any()) << "edge " << edge << " bit " << bit;
    }
  }
}

TEST(GraphFaultInjection, ACorruptSourceChangesTheDistances) {
  const Fixture f;
  Bytes bad = f.input;
  flip(bad, spec::kEdges * 4, 0);
  const Diff diff = diff_of(f.clean, run_serial(*f.workload, bad));
  EXPECT_TRUE(diff.any());
  EXPECT_LT(diff.first, spec::kRanksOffset) << "a different source moves the distances first";
}

TEST(GraphFaultInjection, ABitAboveTheVertexNumberIsReducedAwayAndIsTheBracketsToCatch) {
  const Fixture f;
  for (const unsigned bit : {15U, 16U, 23U, 31U}) {
    Bytes bad = f.input;
    flip(bad, 17 * 4 + bit / 8, bit % 8);
    EXPECT_EQ(bad == f.input, false);
    EXPECT_EQ(run_serial(*f.workload, bad), f.clean)
        << "bit " << bit << ": the specification's modulo makes this input the same graph";
  }
}

TEST(GraphFaultInjection, ANeverRunComputeIsNotAPassWhateverTheBufferHeld) {
  const Fixture f;
  const Bytes other = run_serial(*f.workload, reference_input(4, 5));
  for (const Bytes& held : {Bytes(spec::kOutputBytes, std::byte{0x00}),
                            Bytes(spec::kOutputBytes, std::byte{0xFF}), other}) {
    EXPECT_NE(held, f.clean) << "what an unrun compute leaves is not the unit's answer";
    Bytes output = held;
    Bytes scratch(f.workload->plan().scratch_bytes);
    SerialExecutor serial;
    f.workload->compute(0, f.input, output, scratch, serial);
    EXPECT_EQ(output, f.clean) << "compute overwrote every byte it owes";
  }
}

TEST(GraphFaultInjection, WhatTheScratchHeldDoesNotSurviveIntoTheOutput) {
  const Fixture f;
  SerialExecutor serial;
  Bytes scratch(f.workload->plan().scratch_bytes);
  Bytes first(spec::kOutputBytes);
  f.workload->compute(0, reference_input(4, 5), first, scratch, serial);  // leaves residue
  Bytes second(spec::kOutputBytes);
  f.workload->compute(0, f.input, second, scratch, serial);
  EXPECT_EQ(second, f.clean);
}

}  // namespace
}  // namespace loadforge::graph_test
