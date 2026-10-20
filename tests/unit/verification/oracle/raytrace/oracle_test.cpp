// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ray tracing oracle against OracleContract (ADR 0003), and what the
// contract leaves to this oracle (T1): the sampled pixels, the items and their
// order, what a failed item says, the ranges of the input, the records that
// are not well-formed, and the refusals of a plan that is not a ray tracing
// unit's. Good units are rendered by the model in scene_support.hpp.

#include "verification/oracle/raytrace/oracle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "unit/verification/oracle/raytrace/scene_support.hpp"
#include "verification/oracle.hpp"
#include "workloads/raytrace/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::raytrace {

namespace spec = workloads::raytrace;
namespace rt = test::raytrace;

workloads::Plan raytrace_plan(std::uint32_t edge = spec::kDefaultTileEdge) {
  return workloads::Plan{spec::kInputBytes,     spec::kOutputBytes, 0, 4, edge,
                         2 * spec::kInputBytes, spec::kPixels};
}

std::unique_ptr<Oracle> make_oracle(std::uint32_t edge = spec::kDefaultTileEdge) {
  workloads::WorkloadConfig config;
  config.decomposition = edge;
  return std::move(create(config, raytrace_plan(edge))).value();
}

/// A pixel of tile 0 that no sample of `index` looks at.
std::size_t unsampled_in_tile_zero(std::uint64_t index) {
  std::set<std::size_t> sampled;
  for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
    sampled.insert(sampled_pixel(index, j));
  }
  for (std::size_t p = 0; p < spec::kPixels; ++p) {
    if (p % spec::kWidth < 16 && p / spec::kWidth < 16 && !sampled.contains(p)) {
      return p;
    }
  }
  return spec::kPixels;
}

void flip(rt::Bytes& bytes, std::size_t at, unsigned bit) {
  bytes[at] ^= static_cast<std::byte>(1U << bit);
}

struct RaytraceOracleSubject {
  static constexpr const char* kWorkload = "raytrace";
  static constexpr Contract kContract = Contract::kExact;
  static std::unique_ptr<Oracle> make() { return make_oracle(); }
  static std::vector<contracts::OracleCase> good() {
    std::vector<contracts::OracleCase> cases;
    cases.push_back(rt::unit_of(0, rt::generated(1, 0)));
    cases.push_back(rt::unit_of(7, rt::generated(1, 7)));
    cases.push_back(rt::unit_of(8, rt::generated(2, 8)));
    cases.push_back(rt::unit_of(3, rt::hidden()));
    return cases;
  }
  static std::vector<contracts::OracleCase> wrong() {
    std::vector<contracts::OracleCase> cases;
    for (const std::uint64_t index : {0U, 7U, 12U}) {
      const contracts::OracleCase good = rt::unit_of(index, rt::generated(5, index));
      // Each kind of item, whichever pixels the index samples.
      contracts::OracleCase sampled = good;
      flip(sampled.output, spec::pixel_offset(sampled_pixel(index, 9)) + 3, 2);
      cases.push_back(sampled);
      contracts::OracleCase sum = good;
      flip(sum.output, spec::tile_sum_offset(2) + 1, 5);
      cases.push_back(sum);
      contracts::OracleCase malformed = good;
      flip(malformed.output, spec::pixel_offset(unsampled_in_tile_zero(index)) + 13, 0);
      cases.push_back(malformed);
    }
    return cases;
  }
};

namespace {

ExactMismatch mismatch_of(const Verdict& verdict) {
  const auto* exact = std::get_if<ExactMismatch>(&verdict);
  EXPECT_NE(exact, nullptr) << "not an ExactMismatch";
  static const ExactMismatch kNone{"none", std::nullopt, 1};
  return exact != nullptr ? *exact : kNone;
}

NotChecked not_checked_of(const Verdict& verdict) {
  const auto* unchecked = std::get_if<NotChecked>(&verdict);
  EXPECT_NE(unchecked, nullptr) << "not a NotChecked";
  static const NotChecked kNone{"none", "none", 1};
  return unchecked != nullptr ? *unchecked : kNone;
}

Verdict judge(const Oracle& oracle, std::uint64_t index, const rt::Bytes& input,
              const rt::Bytes& output) {
  return oracle.check(UnitEvidence{index, input, output});
}

TEST(RaytraceOracle, NamesTheRayTracingWorkloadAndTheExactContract) {
  const RaytraceOracle oracle(spec::kDefaultTileEdge);
  EXPECT_EQ(oracle.workload(), spec::kName);
  EXPECT_EQ(oracle.workload(), kWorkload);
  EXPECT_EQ(oracle.contract(), Contract::kExact);
}

TEST(RaytraceSamples, EachSampleIsAPixelInsideItsTileOfTheEightByEightTiling) {
  for (const std::uint64_t index : {0ULL, 1ULL, 99ULL, 0xFFFFFFFFFFFFFFFFULL}) {
    std::set<std::size_t> seen;
    for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
      const std::size_t p = sampled_pixel(index, j);
      ASSERT_LT(p, spec::kPixels);
      const std::size_t x = p % spec::kWidth;
      const std::size_t y = p / spec::kWidth;
      EXPECT_EQ(y / 8 * 8 + x / 8, j) << "index " << index << " sample " << j;
      seen.insert(p);
    }
    EXPECT_EQ(seen.size(), spec::kSampledPixels);
  }
}

TEST(RaytraceSamples, TheSamplesAreAFunctionOfTheIndexAndDifferBetweenIndices) {
  std::set<std::vector<std::size_t>> distinct;
  for (std::uint64_t index = 0; index < 20; ++index) {
    std::vector<std::size_t> pixels;
    for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
      pixels.push_back(sampled_pixel(index, j));
      EXPECT_EQ(pixels.back(), sampled_pixel(index, j));
    }
    distinct.insert(pixels);
  }
  EXPECT_EQ(distinct.size(), 20U);
}

TEST(RaytraceSamples, EveryPositionInATileIsReachedBySomeIndex) {
  std::set<std::size_t> positions;
  for (std::uint64_t index = 0; index < 400; ++index) {
    const std::size_t p = sampled_pixel(index, 0);
    positions.insert(p / spec::kWidth * 8 + p % 8);
  }
  EXPECT_EQ(positions.size(), 64U);
}

TEST(RaytraceOracle, AgreesWithEveryTileEdgeAndCountsTheItemsItEvaluated) {
  const rt::Values scene = rt::generated(9, 4);
  for (const std::uint32_t edge : spec::kTileEdges) {
    const rt::Bytes input = rt::bytes_of(scene);
    const Verdict verdict = judge(*make_oracle(edge), 4, input, rt::render(scene, edge));
    ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "edge " << edge;
    EXPECT_EQ(blocks_of(verdict), spec::kSampledPixels + spec::tile_count(edge));
  }
}

TEST(RaytraceOracle, AnOutputRenderedAtAnotherEdgeIsWrongAtThisOne) {
  const rt::Values scene = rt::generated(9, 4);
  const rt::Bytes input = rt::bytes_of(scene);
  // At edge 8 the second tile sum is the sum of a different set of pixels.
  const Verdict verdict = judge(*make_oracle(8), 4, input, rt::render(scene, 16));
  const ExactMismatch found = mismatch_of(verdict);
  EXPECT_EQ(found.check.rfind(std::string(spec::kCheckTileSum) + " ", 0), 0U);
}

TEST(RaytraceOracle, ASampledPixelThatDiffersNamesItAndReportsTheStoredBytes) {
  const rt::Values scene = rt::generated(3, 21);
  const rt::Bytes input = rt::bytes_of(scene);
  const rt::Bytes right = rt::render(scene);
  const std::size_t p = sampled_pixel(21, 17);
  const std::size_t at = spec::pixel_offset(p);
  rt::Bytes output = right;
  flip(output, at + 1, 0);   // a bit of t
  flip(output, at + 10, 6);  // a bit of the primitive index
  const ExactMismatch found = mismatch_of(judge(*make_oracle(), 21, input, output));
  EXPECT_EQ(found.check, std::string(spec::kCheckPixel) + " " + std::to_string(p));
  EXPECT_EQ(found.blocks_compared, 18U);
  ASSERT_TRUE(found.difference.has_value());
  const Difference& difference = *found.difference;
  EXPECT_EQ(difference.domain, Domain::kStored);
  EXPECT_EQ(difference.first_offset, at + 1);
  EXPECT_EQ(difference.last_offset, at + 10);
  EXPECT_EQ(difference.popcount, 2U);
  EXPECT_EQ(difference.window_offset, at);
  ASSERT_EQ(difference.expected.size(), spec::kPixelBytes);
  EXPECT_TRUE(
      std::equal(difference.expected.begin(), difference.expected.end(), right.begin() + at));
  EXPECT_TRUE(std::equal(difference.actual.begin(), difference.actual.end(), output.begin() + at));
  EXPECT_TRUE(is_valid(Verdict{found}, Contract::kExact));
}

TEST(RaytraceOracle, TheWindowStartsAtTheWordOfTheFirstDifferingByte) {
  const rt::Values scene = rt::generated(3, 21);
  const rt::Bytes input = rt::bytes_of(scene);
  const std::size_t p = sampled_pixel(21, 2);
  const std::size_t at = spec::pixel_offset(p);
  rt::Bytes output = rt::render(scene);
  flip(output, at + 12, 4);  // a byte that must be zero, in the second word
  const ExactMismatch found = mismatch_of(judge(*make_oracle(), 21, input, output));
  ASSERT_TRUE(found.difference.has_value());
  EXPECT_EQ(found.difference->first_offset, at + 12);
  EXPECT_EQ(found.difference->window_offset, at + 8);
  EXPECT_EQ(found.difference->expected.size(), 8U);
  EXPECT_EQ(found.difference->actual.size(), 8U);
  EXPECT_EQ(found.blocks_compared, 3U);
}

TEST(RaytraceOracle, StopsAtTheFirstFailingItemInTheOrderOfTheItems) {
  const rt::Values scene = rt::generated(3, 21);
  const rt::Bytes input = rt::bytes_of(scene);
  rt::Bytes output = rt::render(scene);
  flip(output, spec::pixel_offset(sampled_pixel(21, 40)), 0);
  flip(output, spec::pixel_offset(sampled_pixel(21, 6)) + 4, 0);
  flip(output, spec::tile_sum_offset(0), 0);
  ExactMismatch found = mismatch_of(judge(*make_oracle(), 21, input, output));
  EXPECT_EQ(found.check, "pixel " + std::to_string(sampled_pixel(21, 6)));
  EXPECT_EQ(found.blocks_compared, 7U);
  // The sampled pixels come before every tile sum, even a tile sum 0.
  rt::Bytes sums_only = rt::render(scene);
  flip(sums_only, spec::tile_sum_offset(9), 7);
  flip(sums_only, spec::tile_sum_offset(3), 7);
  found = mismatch_of(judge(*make_oracle(), 21, input, sums_only));
  EXPECT_EQ(found.check, std::string(spec::kCheckTileSum) + " 3");
  EXPECT_EQ(found.blocks_compared, spec::kSampledPixels + 4);
}

TEST(RaytraceOracle, ATileSumThatDiffersReportsItsEightBytesFromTheStartOfTheOutput) {
  const rt::Values scene = rt::generated(3, 21);
  const rt::Bytes input = rt::bytes_of(scene);
  rt::Bytes output = rt::render(scene, 8);
  const std::size_t at = spec::tile_sum_offset(37);
  flip(output, at, 3);
  flip(output, at + 7, 1);
  const ExactMismatch found = mismatch_of(judge(*make_oracle(8), 21, input, output));
  EXPECT_EQ(found.check, std::string(spec::kCheckTileSum) + " 37");
  EXPECT_EQ(found.blocks_compared, spec::kSampledPixels + 38);
  ASSERT_TRUE(found.difference.has_value());
  EXPECT_EQ(found.difference->domain, Domain::kStored);
  EXPECT_EQ(found.difference->first_offset, at);
  EXPECT_EQ(found.difference->last_offset, at + 7);
  EXPECT_EQ(found.difference->popcount, 2U);
  EXPECT_EQ(found.difference->window_offset, at);
  EXPECT_EQ(found.difference->expected.size(), 8U);
  EXPECT_EQ(rt::get64(found.difference->actual, 0), rt::get64(output, at));
}

TEST(RaytraceOracle, ATileSumIsJudgedAgainstTheStoredRecordsNotTheScene) {
  // Change an unsampled pixel's t and the tile's sum to match: nothing the
  // oracle looks at is wrong, and it cannot know. Change only the pixel and
  // the sum no longer follows from the records.
  const rt::Values scene = rt::generated(3, 21);
  const rt::Bytes input = rt::bytes_of(scene);
  const std::size_t p = unsampled_in_tile_zero(21);
  const std::size_t at = spec::pixel_offset(p);
  rt::Bytes output = rt::render(scene);
  rt::put32(output, at + 8, 5);
  rt::put64(output, at, std::bit_cast<std::uint64_t>(5.0));
  const ExactMismatch found = mismatch_of(judge(*make_oracle(), 21, input, output));
  EXPECT_EQ(found.check, std::string(spec::kCheckTileSum) + " 0");
  ASSERT_TRUE(found.difference.has_value());
  EXPECT_EQ(found.difference->first_offset / 8 * 8, found.difference->window_offset);
}

TEST(RaytraceOracle, ARecordThatIsNotWellFormedFailsItsTileWithoutADifference) {
  const rt::Values scene = rt::generated(3, 21);
  const rt::Bytes input = rt::bytes_of(scene);
  const rt::Bytes right = rt::render(scene);
  const std::size_t p = unsampled_in_tile_zero(21);
  const std::size_t at = spec::pixel_offset(p);
  const std::uint64_t kNan = 0x7FF8000000000000ULL;
  struct Breach {
    std::uint32_t primitive;
    std::uint64_t t_bits;
    std::uint32_t padding;
  };
  const std::vector<Breach> breaches = {
      {spec::kNoHit, std::bit_cast<std::uint64_t>(3.0), 0},  // a miss with a finite t
      {spec::kNoHit, spec::kNoHitDistanceBits ^ 1U, 0},      // a miss, not infinity's bits
      {3, spec::kNoHitDistanceBits, 0},                      // a hit at infinity
      {3, kNan, 0},                                          // a hit at NaN
      {3, std::bit_cast<std::uint64_t>(0.0), 0},             // a hit at zero
      {3, std::bit_cast<std::uint64_t>(-4.0), 0},            // a hit behind the eye
      {static_cast<std::uint32_t>(spec::kPrimitives), std::bit_cast<std::uint64_t>(4.0), 0},
      {spec::kNoHit - 1, std::bit_cast<std::uint64_t>(4.0), 0},
      {3, std::bit_cast<std::uint64_t>(4.0), 1},  // padding that is not zero
      {spec::kNoHit, spec::kNoHitDistanceBits, 0x80000000U},
  };
  for (const Breach& breach : breaches) {
    rt::Bytes output = right;
    rt::put64(output, at, breach.t_bits);
    rt::put32(output, at + 8, breach.primitive);
    rt::put32(output, at + 12, breach.padding);
    const ExactMismatch found = mismatch_of(judge(*make_oracle(), 21, input, output));
    EXPECT_EQ(found.check, std::string(spec::kCheckTileSum) + " 0: the stored record of pixel " +
                               std::to_string(p) + " is not well-formed");
    EXPECT_FALSE(found.difference.has_value());
    EXPECT_EQ(found.blocks_compared, spec::kSampledPixels + 1);
    EXPECT_TRUE(is_valid(Verdict{found}, Contract::kExact));
  }
}

TEST(RaytraceOracle, TheFirstMalformedPixelOfATileInRowMajorOrderIsTheOneNamed) {
  const rt::Values scene = rt::hidden();
  const rt::Bytes input = rt::bytes_of(scene);
  // Tile 1 of edge 16 is columns 16..31, rows 0..15. Malform (20, 5) and
  // (18, 9): row 5 comes first, though pixel 9 * 64 + 18 has the lower x.
  // Both are told apart from the sampled pixels by choosing the index.
  std::uint64_t index = 0;
  const auto sampled_among = [&](std::uint64_t i, std::size_t a, std::size_t b) {
    for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
      if (sampled_pixel(i, j) == a || sampled_pixel(i, j) == b) {
        return true;
      }
    }
    return false;
  };
  const std::size_t first = 5 * spec::kWidth + 20;
  const std::size_t second = 9 * spec::kWidth + 18;
  while (sampled_among(index, first, second)) {
    ++index;
  }
  rt::Bytes output = rt::render(scene);
  rt::put32(output, spec::pixel_offset(second) + 12, 1);
  rt::put32(output, spec::pixel_offset(first) + 12, 1);
  const ExactMismatch found = mismatch_of(judge(*make_oracle(), index, input, output));
  EXPECT_EQ(found.check, std::string(spec::kCheckTileSum) + " 1: the stored record of pixel " +
                             std::to_string(first) + " is not well-formed");
}

TEST(RaytraceOracle, ATileSumAddsTheHitsInRowMajorOrderAndLeavesTheMissesOut) {
  // The sum of three hits whose order matters: (1e16 + 1) + 1 differs from 1e16 + (1 + 1)
  // in binary64, so a different order is a different sum.
  const rt::Values scene = rt::hidden();
  const rt::Bytes input = rt::bytes_of(scene);
  std::uint64_t index = 0;
  const std::array<std::size_t, 3> pixels = {0, 1, 64};
  const auto sampled = [&](std::uint64_t i) {
    for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
      if (std::find(pixels.begin(), pixels.end(), sampled_pixel(i, j)) != pixels.end()) {
        return true;
      }
    }
    return false;
  };
  while (sampled(index)) {
    ++index;
  }
  rt::Bytes output = rt::render(scene);
  const std::array<double, 3> ts = {1e16, 1.0, 1.0};
  for (std::size_t k = 0; k < pixels.size(); ++k) {
    rt::put64(output, spec::pixel_offset(pixels[k]), std::bit_cast<std::uint64_t>(ts[k]));
    rt::put32(output, spec::pixel_offset(pixels[k]) + 8, static_cast<std::uint32_t>(k));
  }
  double in_order = 0.0;
  for (const double t : ts) {
    in_order = in_order + t;
  }
  double reversed = 0.0;
  for (std::size_t k = ts.size(); k > 0; --k) {
    reversed = reversed + ts[k - 1];
  }
  ASSERT_NE(in_order, reversed);
  rt::put64(output, spec::tile_sum_offset(0), std::bit_cast<std::uint64_t>(in_order));
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), index, input, output)));
  rt::put64(output, spec::tile_sum_offset(0), std::bit_cast<std::uint64_t>(reversed));
  EXPECT_EQ(mismatch_of(judge(*make_oracle(), index, input, output)).check,
            std::string(spec::kCheckTileSum) + " 0");
}

TEST(RaytraceOracle, ATieBetweenPrimitivesGoesToTheLowerIndex) {
  rt::Values scene = rt::hidden();
  for (std::size_t i = 0; i < 4; ++i) {
    rt::set_sphere(scene, i, spec::Sphere{spec::Vec3{0.0, 0.0, -1.0}, 0.1875 - 1.0 / 8388608.0});
  }
  const rt::Bytes input = rt::bytes_of(scene);
  const rt::Bytes output = rt::render(scene);
  std::uint64_t index = 0;
  std::size_t which = spec::kSampledPixels;
  for (; index < 400 && which == spec::kSampledPixels; ++index) {
    for (std::size_t j = 0; j < spec::kSampledPixels; ++j) {
      if (rt::get32(output, spec::pixel_offset(sampled_pixel(index, j)) + 8) == 0) {
        which = j;
        break;
      }
    }
  }
  --index;
  ASSERT_LT(which, spec::kSampledPixels) << "no index samples a pixel the spheres cover";
  EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), index, input, output)));
  rt::Bytes later = output;
  rt::put32(later, spec::pixel_offset(sampled_pixel(index, which)) + 8, 1);
  EXPECT_EQ(mismatch_of(judge(*make_oracle(), index, input, later)).check,
            "pixel " + std::to_string(sampled_pixel(index, which)));
}

TEST(RaytraceOracle, ASphereInFrontOfATriangleHidesItAndTheTriangleShowsAroundIt) {
  // The triangle lies in the plane z = 0 around the origin; the sphere sits
  // just in front of that plane, so it is nearer wherever both are hit and
  // the triangle shows outside the sphere.
  rt::Values scene = rt::hidden();
  rt::set_sphere(scene, 5, spec::Sphere{spec::Vec3{0.0, 0.0, -0.01}, 0.125});
  rt::set_triangle(scene, 9,
                   spec::Triangle{spec::Vec3{-1.0, -1.0, 0.0}, spec::Vec3{1.0, -1.0, 0.0},
                                  spec::Vec3{0.0, 1.0, 0.0}});
  const auto pixels = rt::trace(scene);
  std::size_t spheres = 0;
  std::size_t triangles = 0;
  for (const auto& pixel : pixels) {
    spheres += pixel.primitive == 5 ? 1 : 0;
    triangles += pixel.primitive == spec::kSpheres + 9 ? 1 : 0;
  }
  EXPECT_GT(spheres, 0U);
  EXPECT_GT(triangles, spheres);
  const rt::Bytes input = rt::bytes_of(scene);
  const rt::Bytes output = rt::render(scene);
  for (std::uint64_t index = 0; index < 6; ++index) {
    EXPECT_TRUE(std::holds_alternative<Agreement>(judge(*make_oracle(), index, input, output)));
  }
}

TEST(RaytraceOracle, EveryRangeOfTheInputIsHeldAtBothEndsAndRefusedBeyond) {
  struct Kind {
    std::size_t where;
    spec::Interval range;
    const char* name;
  };
  const std::vector<Kind> kinds = {
      {0, spec::kSphereXy, "sphere 0 centre x"},
      {1, spec::kSphereXy, "sphere 0 centre y"},
      {2, spec::kSphereZ, "sphere 0 centre z"},
      {3, spec::kRadius, "sphere 0 radius"},
      {4 * 127, spec::kSphereXy, "sphere 127 centre x"},
      {4 * 127 + 3, spec::kRadius, "sphere 127 radius"},
      {rt::kSphereValues, spec::kVertexXy, "triangle 0 vertex 0 x"},
      {rt::kSphereValues + 1, spec::kVertexXy, "triangle 0 vertex 0 y"},
      {rt::kSphereValues + 2, spec::kVertexZ, "triangle 0 vertex 0 z"},
      {rt::kSphereValues + 3, spec::kVertexXy, "triangle 0 vertex 1 x"},
      {rt::kSphereValues + 7, spec::kVertexXy, "triangle 0 vertex 2 y"},
      {rt::kSphereValues + 9 * 127 + 8, spec::kVertexZ, "triangle 127 vertex 2 z"},
  };
  const rt::Values base = rt::generated(4, 4);
  for (const Kind& kind : kinds) {
    for (const double inside : {kind.range.lo, kind.range.hi}) {
      rt::Values scene = base;
      scene[kind.where] = inside;
      const Verdict verdict = judge(*make_oracle(), 4, rt::bytes_of(scene), rt::render(scene));
      EXPECT_TRUE(std::holds_alternative<Agreement>(verdict)) << kind.name << " at " << inside;
    }
    const double inf = std::numeric_limits<double>::infinity();
    for (const double outside :
         {std::nextafter(kind.range.lo, -inf), std::nextafter(kind.range.hi, inf),
          std::numeric_limits<double>::quiet_NaN(), inf, -inf, 0.0 * -1.0 + 1e300}) {
      rt::Values scene = base;
      scene[kind.where] = outside;
      const Verdict verdict = judge(*make_oracle(), 4, rt::bytes_of(scene), rt::render(base));
      const NotChecked unchecked = not_checked_of(verdict);
      EXPECT_EQ(unchecked.blocks_compared, 0U);
      EXPECT_TRUE(is_valid(verdict, Contract::kExact));
      EXPECT_NE(unchecked.why.find(kind.name), std::string::npos)
          << unchecked.why << " should name " << kind.name;
    }
  }
}

TEST(RaytraceOracle, EvidenceOfAnotherSizeSaysWhatItGotAndWhatAUnitIs) {
  const contracts::OracleCase good = rt::unit_of(1, rt::generated(1, 1));
  const Verdict verdict = judge(*make_oracle(), 1, good.input, rt::Bytes(10));
  const NotChecked unchecked = not_checked_of(verdict);
  EXPECT_EQ(unchecked.blocks_compared, 0U);
  EXPECT_NE(unchecked.why.find("10 of output"), std::string::npos);
  EXPECT_NE(unchecked.why.find(std::to_string(spec::kOutputBytes)), std::string::npos);
}

TEST(RaytraceOracle, AGeneratedSceneHitsBetweenATenthAndNineTenthsOfItsPixels) {
  for (std::uint64_t index = 0; index < 4; ++index) {
    const std::size_t hits = rt::hits_of(rt::generated(11, index));
    EXPECT_GT(hits, spec::kPixels / 10);
    EXPECT_LT(hits, spec::kPixels * 9 / 10);
  }
}

TEST(RaytraceCreate, GivesTheOracleForEachTileEdgeAndItsDefault) {
  for (const std::uint32_t edge : spec::kTileEdges) {
    workloads::WorkloadConfig config;
    config.decomposition = edge;
    const auto created = create(config, raytrace_plan(edge));
    ASSERT_TRUE(created.has_value()) << edge;
    EXPECT_EQ(created.value()->workload(), "raytrace");
  }
  const auto defaulted = create(workloads::WorkloadConfig{}, raytrace_plan(spec::kDefaultTileEdge));
  EXPECT_TRUE(defaulted.has_value());
}

TEST(RaytraceCreate, RefusesWhatIsNotARayTracingUnit) {
  workloads::Plan wrong_input = raytrace_plan();
  wrong_input.input_bytes += 1;
  workloads::Plan wrong_output = raytrace_plan();
  wrong_output.output_bytes -= 1;
  const auto refusals = {
      create(workloads::WorkloadConfig{}, wrong_input),
      create(workloads::WorkloadConfig{}, wrong_output),
      create(workloads::WorkloadConfig{}, raytrace_plan(8)),  // configured 0 resolves to 16
      create(workloads::WorkloadConfig{}, raytrace_plan(0)),
      create(workloads::WorkloadConfig{0, 0, 7, simd::IsaPath::kScalar}, raytrace_plan(7)),
      create(workloads::WorkloadConfig{0, 0, 8, simd::IsaPath::kScalar}, raytrace_plan(16)),
      create(workloads::WorkloadConfig{0, 0, 16, simd::IsaPath::kScalar}, raytrace_plan(32)),
  };
  for (const auto& refused : refusals) {
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().fault, OracleFault::kConfiguration);
    EXPECT_FALSE(refused.error().detail.empty());
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::raytrace

namespace loadforge::contracts {
namespace {
using RaytraceOracleSubjects =
    ::testing::Types<verification::oracle::raytrace::RaytraceOracleSubject>;
INSTANTIATE_TYPED_TEST_SUITE_P(RaytraceOracle, OracleContract, RaytraceOracleSubjects);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);
}  // namespace
}  // namespace loadforge::contracts
