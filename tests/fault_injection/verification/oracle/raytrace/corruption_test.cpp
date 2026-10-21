// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b of the ray tracing oracle (verification.md §8; IMPLEMENTATION
// §2).
//
// T5: a corrupted output is caught and classified.
//   * every bit of every sampled pixel's 16 bytes, one at a time: an
//     ExactMismatch naming the pixel, a single-bit Difference at the right
//     offset from the start of the output, classified single-bit;
//   * every bit of every tile sum, the same;
//   * several bits in one word, in one line, as the classification names them
//     (an item is 16 bytes at most and lies inside one cache line, so a
//     difference across lines cannot occur from a single item);
//   * every bit of some pixels the samples do not look at, which only a tile
//     sum can reveal: each is said to be caught as a tile sum that differs,
//     as a record that is not well-formed, or -- proved by the test's own
//     recomputation -- as a change the specification does not require the
//     oracle to see (the sum comes out to the same bits, and the record is
//     still well-formed);
//   * the bits of the input: a value pushed out of its range is NotChecked,
//     and one that stays in it is judged against the output as it is.
//
// T5b: a check that cannot be made is NotChecked, never an Agreement --
// evidence of the wrong size, every kind of value out of range, and a plan
// that is not a ray tracing unit's (no oracle at all).

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "unit/verification/oracle/raytrace/scene_support.hpp"
#include "verification/oracle.hpp"
#include "verification/oracle/raytrace/oracle.hpp"
#include "verification/record.hpp"
#include "workloads/raytrace/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::raytrace {
namespace {

namespace spec = workloads::raytrace;
namespace rt = test::raytrace;

constexpr std::uint64_t kIndex = 13;

struct Unit {
  rt::Values scene;
  rt::Bytes input;
  rt::Bytes output;
};

const Unit& unit() {
  static const Unit kUnit = [] {
    const rt::Values scene = rt::generated(77, kIndex);
    return Unit{scene, rt::bytes_of(scene), rt::render(scene)};
  }();
  return kUnit;
}

Verdict judge(const rt::Bytes& input, const rt::Bytes& output,
              std::uint32_t edge = spec::kDefaultTileEdge) {
  return RaytraceOracle(edge).check(UnitEvidence{kIndex, input, output});
}

void flip(rt::Bytes& bytes, std::size_t bit) {
  bytes[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
}

BitClass class_of(const Difference& difference) {
  return classify_bits(difference.popcount, difference.first_offset, difference.last_offset, 64);
}

ExactMismatch exact_of(const Verdict& verdict) {
  const auto* exact = std::get_if<ExactMismatch>(&verdict);
  EXPECT_NE(exact, nullptr);
  return exact != nullptr ? *exact : ExactMismatch{"none", std::nullopt, 1};
}

TEST(RaytraceCorruption, TheUncorruptedUnitIsAgreedWith) {
  ASSERT_TRUE(std::holds_alternative<Agreement>(judge(unit().input, unit().output)));
}

TEST(RaytraceCorruption, EveryBitOfEverySampledPixelIsCaughtAsASingleBitOfThatPixel) {
  for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
    const std::size_t p = sampled_pixel(kIndex, j);
    for (std::size_t bit = 0; bit < spec::kPixelBytes * 8; ++bit) {
      rt::Bytes output = unit().output;
      flip(output, spec::pixel_offset(p) * 8 + bit);
      const Verdict verdict = judge(unit().input, output);
      const ExactMismatch found = exact_of(verdict);
      EXPECT_EQ(found.check, std::string(spec::kCheckPixel) + " " + std::to_string(p));
      EXPECT_EQ(found.blocks_compared, j + 1);
      ASSERT_TRUE(found.difference.has_value());
      const Difference& difference = *found.difference;
      EXPECT_EQ(difference.domain, Domain::kStored);
      EXPECT_EQ(difference.popcount, 1U);
      EXPECT_EQ(difference.first_offset, spec::pixel_offset(p) + bit / 8);
      EXPECT_EQ(difference.last_offset, difference.first_offset);
      EXPECT_EQ(class_of(difference), BitClass::kSingleBit);
      EXPECT_TRUE(is_valid(verdict, Contract::kExact));
    }
  }
}

TEST(RaytraceCorruption, EveryBitOfEveryTileSumIsCaughtAsASingleBitOfThatSum) {
  for (const std::uint32_t edge : spec::kTileEdges) {
    const rt::Bytes output = rt::render(unit().scene, edge);
    ASSERT_TRUE(std::holds_alternative<Agreement>(judge(unit().input, output, edge)));
    for (std::size_t k = 0; k < spec::tile_count(edge); ++k) {
      for (std::size_t bit = 0; bit < 64; ++bit) {
        rt::Bytes corrupt = output;
        flip(corrupt, spec::tile_sum_offset(k) * 8 + bit);
        const Verdict verdict = judge(unit().input, corrupt, edge);
        const ExactMismatch found = exact_of(verdict);
        EXPECT_EQ(found.check, std::string(spec::kCheckTileSum) + " " + std::to_string(k));
        EXPECT_EQ(found.blocks_compared, spec::kSampledPixels + k + 1);
        ASSERT_TRUE(found.difference.has_value());
        EXPECT_EQ(found.difference->popcount, 1U);
        EXPECT_EQ(found.difference->first_offset, spec::tile_sum_offset(k) + bit / 8);
        EXPECT_EQ(class_of(*found.difference), BitClass::kSingleBit);
        EXPECT_TRUE(is_valid(verdict, Contract::kExact));
      }
    }
  }
}

TEST(RaytraceCorruption, SlotsPastTheTilesOfTheEdgeAreNotItems) {
  // At edge 32 only four tile sums are items; the rest of the 64 slots are
  // zero bytes in the specification and not looked at.
  rt::Bytes output = rt::render(unit().scene, 32);
  flip(output, spec::tile_sum_offset(4) * 8 + 9);
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(unit().input, output, 32)));
}

TEST(RaytraceCorruption, SeveralBitsAreClassifiedByWhereTheyLie) {
  const std::size_t p = sampled_pixel(kIndex, 20);
  const std::size_t at = spec::pixel_offset(p);
  struct Case {
    std::vector<std::size_t> bytes;
    BitClass expected;
  };
  const std::vector<Case> cases = {
      {{1, 3}, BitClass::kMultiBitInWord},    // two bytes of t
      {{0, 7}, BitClass::kMultiBitInWord},    // the first word, ends of it
      {{2, 9}, BitClass::kWithinCacheLine},   // t and the primitive index
      {{0, 15}, BitClass::kWithinCacheLine},  // the ends of the record
  };
  for (const Case& c : cases) {
    rt::Bytes output = unit().output;
    for (const std::size_t byte : c.bytes) {
      output[at + byte] ^= std::byte{0x10};
    }
    const ExactMismatch found = exact_of(judge(unit().input, output));
    ASSERT_TRUE(found.difference.has_value());
    EXPECT_EQ(class_of(*found.difference), c.expected);
    EXPECT_EQ(found.difference->popcount, c.bytes.size());
  }
  // The whole word flipped.
  rt::Bytes output = unit().output;
  for (std::size_t byte = 8; byte < 16; ++byte) {
    output[at + byte] ^= std::byte{0xFF};
  }
  const ExactMismatch found = exact_of(judge(unit().input, output));
  ASSERT_TRUE(found.difference.has_value());
  EXPECT_EQ(found.difference->popcount, 64U);
  EXPECT_EQ(class_of(*found.difference), BitClass::kMultiBitInWord);
}

bool well_formed(const rt::Bytes& output, std::size_t p) {
  const std::size_t at = spec::pixel_offset(p);
  if (rt::get32(output, at + 12) != 0) {
    return false;
  }
  const std::uint32_t primitive = rt::get32(output, at + 8);
  const std::uint64_t bits = rt::get64(output, at);
  if (primitive == spec::kNoHit) {
    return bits == spec::kNoHitDistanceBits;
  }
  const auto t = std::bit_cast<double>(bits);
  return primitive < spec::kPrimitives && std::isfinite(t) && t > 0.0;
}

/// What the specification says about tile 0 of edge 16: nullopt when nothing
/// in it is wrong, otherwise whether the failure carries a difference.
enum class Expected : std::uint8_t { kNothing, kSumDiffers, kMalformed };

Expected expectation_for_tile_zero(const rt::Bytes& output) {
  double sum = 0.0;
  for (std::size_t y = 0; y < 16; ++y) {
    for (std::size_t x = 0; x < 16; ++x) {
      const std::size_t p = y * spec::kWidth + x;
      if (!well_formed(output, p)) {
        return Expected::kMalformed;
      }
      if (rt::get32(output, spec::pixel_offset(p) + 8) != spec::kNoHit) {
        sum += std::bit_cast<double>(rt::get64(output, spec::pixel_offset(p)));
      }
    }
  }
  return std::bit_cast<std::uint64_t>(sum) == rt::get64(output, spec::tile_sum_offset(0))
             ? Expected::kNothing
             : Expected::kSumDiffers;
}

TEST(RaytraceCorruption, ABitOfAPixelNoSampleLooksAtIsCaughtOnlyThroughItsTileSum) {
  std::set<std::size_t> sampled;
  for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
    sampled.insert(sampled_pixel(kIndex, j));
  }
  // A hit and a miss of tile 0, neither sampled.
  std::vector<std::size_t> chosen;
  bool hit_chosen = false;
  bool miss_chosen = false;
  for (std::size_t p = 0; p < spec::kPixels && chosen.size() < 6; ++p) {
    if (p % spec::kWidth >= 16 || p / spec::kWidth >= 16 || sampled.contains(p)) {
      continue;
    }
    const bool hit = rt::get32(unit().output, spec::pixel_offset(p) + 8) != spec::kNoHit;
    if ((hit && hit_chosen && chosen.size() >= 3) || (!hit && miss_chosen && chosen.size() >= 3)) {
      continue;
    }
    hit_chosen = hit_chosen || hit;
    miss_chosen = miss_chosen || !hit;
    chosen.push_back(p);
  }
  ASSERT_TRUE(hit_chosen);
  ASSERT_TRUE(miss_chosen);
  std::size_t differs = 0;
  std::size_t malformed = 0;
  std::size_t unseen = 0;
  for (const std::size_t p : chosen) {
    for (std::size_t bit = 0; bit < spec::kPixelBytes * 8; ++bit) {
      rt::Bytes output = unit().output;
      flip(output, spec::pixel_offset(p) * 8 + bit);
      const Verdict verdict = judge(unit().input, output);
      switch (expectation_for_tile_zero(output)) {
        case Expected::kNothing: {
          ++unseen;
          EXPECT_TRUE(std::holds_alternative<Agreement>(verdict))
              << "pixel " << p << " bit " << bit;
          break;
        }
        case Expected::kSumDiffers: {
          ++differs;
          const ExactMismatch found = exact_of(verdict);
          EXPECT_EQ(found.check, std::string(spec::kCheckTileSum) + " 0");
          EXPECT_TRUE(found.difference.has_value());
          EXPECT_EQ(found.blocks_compared, spec::kSampledPixels + 1);
          break;
        }
        case Expected::kMalformed: {
          ++malformed;
          const ExactMismatch found = exact_of(verdict);
          EXPECT_EQ(found.check.rfind(std::string(spec::kCheckTileSum) + " 0: ", 0), 0U);
          EXPECT_NE(found.check.find(std::to_string(p)), std::string::npos);
          EXPECT_FALSE(found.difference.has_value());
          break;
        }
      }
      EXPECT_TRUE(is_valid(verdict, Contract::kExact));
    }
  }
  EXPECT_GT(differs, 0U);
  EXPECT_GT(malformed, 0U);
  EXPECT_GT(unseen, 0U) << "some changes (the index of a hit, the lowest bits of a distance) are "
                           "invisible to the specification's check";
}

bool in_range_after(const rt::Bytes& input) {
  const rt::Values values = rt::values_of(input);
  for (std::size_t i = 0; i < values.size(); ++i) {
    spec::Interval range;
    if (i < rt::kSphereValues) {
      const std::size_t c = i % 4;
      if (c < 2) {
        range = spec::kSphereXy;
      } else if (c == 2) {
        range = spec::kSphereZ;
      } else {
        range = spec::kRadius;
      }
    } else {
      range = (i - rt::kSphereValues) % 3 < 2 ? spec::kVertexXy : spec::kVertexZ;
    }
    if (!spec::contains(range, values[i])) {
      return false;
    }
  }
  return true;
}

TEST(RaytraceCorruption, ABitOfTheInputIsNotCheckedWhenItLeavesTheRangeAndJudgedWhenItDoesNot) {
  std::size_t not_checked = 0;
  std::size_t mismatches = 0;
  std::size_t agreements = 0;
  // Every bit of the first three primitives of each kind and of the last
  // sphere and triangle, and every 29th bit of the rest.
  for (std::size_t bit = 0; bit < spec::kInputBytes * 8; ++bit) {
    const std::size_t value = bit / 64;
    const bool listed = value < 12 ||
                        (value >= rt::kSphereValues - 4 && value < rt::kSphereValues + 27) ||
                        value >= rt::kSphereValues + std::size_t{9} * 127 || bit % 29 == 0;
    if (!listed) {
      continue;
    }
    rt::Bytes input = unit().input;
    flip(input, bit);
    const Verdict verdict = judge(input, unit().output);
    EXPECT_TRUE(is_valid(verdict, Contract::kExact)) << "bit " << bit;
    if (!in_range_after(input)) {
      ++not_checked;
      ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << "bit " << bit;
      EXPECT_EQ(blocks_of(verdict), 0U);
    } else if (std::holds_alternative<ExactMismatch>(verdict)) {
      ++mismatches;
    } else {
      ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "bit " << bit;
      ++agreements;
    }
  }
  EXPECT_GT(not_checked, 0U);
  EXPECT_GT(mismatches, 0U);
  EXPECT_GT(agreements, 0U) << "a change of a low bit of a scene value moves no sampled pixel";
}

// T5b.

TEST(RaytraceNotChecked, EvidenceOfAnyOtherSizeComparesNothing) {
  const std::vector<std::pair<rt::Bytes, rt::Bytes>> sizes = {
      {rt::Bytes(spec::kInputBytes + 1), unit().output},
      {unit().input, rt::Bytes(spec::kOutputBytes - 1)},
      {rt::Bytes(spec::kInputBytes - 8), rt::Bytes(spec::kOutputBytes + 8)},
      {{}, {}},
  };
  for (const auto& [input, output] : sizes) {
    const Verdict verdict = judge(input, output);
    ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict));
    EXPECT_EQ(blocks_of(verdict), 0U);
    EXPECT_TRUE(is_valid(verdict, Contract::kExact));
  }
}

TEST(RaytraceNotChecked, EveryKindOfValueOutOfRangeComparesNothingEvenOverACorruptOutput) {
  const double inf = std::numeric_limits<double>::infinity();
  rt::Bytes corrupt = unit().output;
  flip(corrupt, spec::pixel_offset(sampled_pixel(kIndex, 0)) * 8);
  for (std::size_t where = 0; where < unit().scene.size(); where += 37) {
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), inf, -inf, 1.0e300}) {
      rt::Values scene = unit().scene;
      scene[where] = bad;
      const Verdict verdict = judge(rt::bytes_of(scene), corrupt);
      ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict)) << where;
      EXPECT_EQ(blocks_of(verdict), 0U);
    }
  }
}

TEST(RaytraceNotChecked, APlanThatIsNotARayTracingUnitGivesNoOracleAtAll) {
  workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, 16, 0, 0};
  plan.decomposition = 12;
  const auto refused = create(workloads::WorkloadConfig{}, plan);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().fault, OracleFault::kConfiguration);
}

}  // namespace
}  // namespace loadforge::verification::oracle::raytrace
