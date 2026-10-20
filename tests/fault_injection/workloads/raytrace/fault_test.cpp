// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5: what a fault in or around the ray tracing workload looks like to a
// comparison with brute force, and which items of the comparison name it. The
// comparison here is the workload's own (reference.hpp); the independent
// oracle of src/verification/oracle/raytrace is another stream's.
//
// T5b: a comparison that cannot be made answers kNotPerformed, never a pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

#include "oracle/workloads/raytrace/executors.hpp"
#include "workloads/raytrace/generator.hpp"
#include "workloads/raytrace/kernels.hpp"
#include "workloads/raytrace/raytrace.hpp"
#include "workloads/raytrace/scene.hpp"

namespace loadforge::tests::raytrace {
namespace {

constexpr std::uint32_t kEdge = 16;

struct Unit {
  std::unique_ptr<workloads::Workload> workload;
  Bytes input;
};

Unit unit_on(simd::IsaPath path, std::uint64_t index) {
  Unit unit;
  unit.workload = std::move(workloads::raytrace::create(config_for(path, kEdge, 77))).value();
  unit.input = generate_unit(*unit.workload, index);
  return unit;
}

TEST(RaytraceFaults, ACleanOutputAgrees) {
  SerialExecutor serial;
  for (const simd::IsaPath path : runnable_paths()) {
    const Unit unit = unit_on(path, 1);
    const Bytes output = compute_unit(*unit.workload, 1, unit.input, serial);
    const Comparison comparison = compare_with_brute_force(unit.input, output, kEdge);
    EXPECT_EQ(comparison.verdict, Verdict::kAgreement);
    EXPECT_TRUE(comparison.pixels.empty());
    EXPECT_TRUE(comparison.tiles.empty());
  }
}

TEST(RaytraceFaults, ADroppedTaskIsNamedByTheRowOfPixelsItLeftAndTheTilesOverThem) {
  for (const simd::IsaPath path : runnable_paths()) {
    const Unit unit = unit_on(path, 2);
    for (const std::size_t row : {std::size_t{0}, std::size_t{17}, std::size_t{63}}) {
      DroppingExecutor dropping({row});
      // Prefilled with a pattern no record can be, so the dropped row's stale
      // bytes cannot agree by luck.
      const Bytes output = compute_unit(*unit.workload, 2, unit.input, dropping, std::byte{0xEE});
      const Comparison comparison = compare_with_brute_force(unit.input, output, kEdge);
      ASSERT_EQ(comparison.verdict, Verdict::kMismatch) << "row " << row;
      std::vector<std::size_t> expected_pixels;
      for (std::size_t x = 0; x < rt::kWidth; ++x) {
        expected_pixels.push_back(row * rt::kWidth + x);
      }
      EXPECT_EQ(comparison.pixels, expected_pixels) << "row " << row;
      // The sums are taken from the stored records, so the tiles that differ
      // are the ones over the dropped row, and only those.
      std::vector<std::size_t> expected_tiles;
      for (std::size_t tile = 0; tile < rt::tile_count(kEdge); ++tile) {
        if (tile / (rt::kWidth / kEdge) == row / kEdge) {
          expected_tiles.push_back(tile);
        }
      }
      EXPECT_EQ(comparison.tiles, expected_tiles) << "row " << row;
    }
  }
}

TEST(RaytraceFaults, SeveralDroppedTasksAreEachNamed) {
  const Unit unit = unit_on(simd::IsaPath::kScalar, 3);
  DroppingExecutor dropping({4, 5, 40});
  const Bytes output = compute_unit(*unit.workload, 3, unit.input, dropping, std::byte{0xEE});
  const Comparison comparison = compare_with_brute_force(unit.input, output, kEdge);
  ASSERT_EQ(comparison.verdict, Verdict::kMismatch);
  const std::set<std::size_t> rows = [&] {
    std::set<std::size_t> found;
    for (const std::size_t pixel : comparison.pixels) {
      found.insert(pixel / rt::kWidth);
    }
    return found;
  }();
  EXPECT_EQ(rows, (std::set<std::size_t>{4, 5, 40}));
  EXPECT_EQ(comparison.pixels.size(), 3 * rt::kWidth);
}

TEST(RaytraceFaults, AHierarchyWithBoxesTooSmallIsCaughtAndOnlyEverMissesWhatIsThere) {
  // A stale or shrunk bound: the hierarchy skips a primitive it should have
  // tried. Every pixel it gets wrong is one where the true nearest was
  // skipped: its t is not nearer than brute force's, never nearer.
  const auto table = workloads::raytrace::row_kernels();
  Bytes input(rt::kInputBytes);
  rt::generate_scene(5, 5, std::span<std::byte, rt::kInputBytes>(input));
  const Bytes expected = brute_force_output(input, kEdge);
  for (const simd::IsaPath path : runnable_paths()) {
    auto scene = std::make_unique<rt::SceneStorage>();
    rt::decode_scene(std::span<const std::byte, rt::kInputBytes>(input), *scene);
    rt::build_bvh(*scene, -0.04);
    Bytes records(rt::kTileSumsOffset);
    for (std::uint32_t y = 0; y < rt::kHeight; ++y) {
      simd::kernel_at(table, path)(
          *scene, y,
          std::span<std::byte, rt::kRowBytes>(records.data() + y * rt::kRowBytes, rt::kRowBytes));
    }
    std::size_t wrong = 0;
    for (std::size_t p = 0; p < rt::kPixels; ++p) {
      const std::size_t at = rt::pixel_offset(p);
      if (std::memcmp(records.data() + at, expected.data() + at, rt::kPixelBytes) == 0) {
        continue;
      }
      ++wrong;
      const double stored = double_at(std::span<const std::byte>(records).subspan(at), 0);
      const double truth = double_at(std::span<const std::byte>(expected).subspan(at), 0);
      EXPECT_GE(stored, truth) << simd::describe(path) << " pixel " << p;
    }
    EXPECT_GT(wrong, 0U) << simd::describe(path) << ": the shrunken boxes must be caught";
  }
}

TEST(RaytraceFaults, AFlippedBitIsNamedByTheItemThatHoldsIt) {
  SerialExecutor serial;
  const Unit unit = unit_on(simd::IsaPath::kScalar, 4);
  const Bytes clean = compute_unit(*unit.workload, 4, unit.input, serial);
  const auto flip = [&](std::size_t byte, unsigned bit) {
    Bytes output = clean;
    output[byte] ^= static_cast<std::byte>(1U << bit);
    return compare_with_brute_force(unit.input, output, kEdge);
  };
  const std::size_t pixel = 1234;
  // t, the primitive, the padding of a record: that pixel and no other item.
  for (const std::size_t within : {std::size_t{0}, std::size_t{7}, std::size_t{8}, std::size_t{11},
                                   std::size_t{12}, std::size_t{15}}) {
    const Comparison comparison = flip(rt::pixel_offset(pixel) + within, 3);
    EXPECT_EQ(comparison.verdict, Verdict::kMismatch) << within;
    EXPECT_EQ(comparison.pixels, std::vector<std::size_t>{pixel}) << within;
    EXPECT_TRUE(comparison.tiles.empty()) << "the stored sums were not touched: " << within;
  }
  // A tile's sum, and a slot beyond the tiles.
  for (const std::size_t tile :
       {std::size_t{0}, std::size_t{15}, std::size_t{16}, std::size_t{63}}) {
    const Comparison comparison = flip(rt::tile_sum_offset(tile) + 5, 6);
    EXPECT_EQ(comparison.verdict, Verdict::kMismatch) << tile;
    EXPECT_TRUE(comparison.pixels.empty()) << tile;
    EXPECT_EQ(comparison.tiles, std::vector<std::size_t>{tile}) << tile;
  }
}

TEST(RaytraceFaults, AFlippedBitInTheInputIsNotAFaultOfTheWorkload) {
  // The workload computes on the input it is given; corruption of the input
  // is the input-integrity bracket's to see. The output is the image of the
  // scene as it now is, and agrees with brute force over that scene.
  SerialExecutor serial;
  for (const simd::IsaPath path : runnable_paths()) {
    const Unit unit = unit_on(path, 5);
    Bytes corrupt = unit.input;
    corrupt[9] ^= std::byte{0x40};
    const Bytes output = compute_unit(*unit.workload, 5, corrupt, serial);
    EXPECT_EQ(compare_with_brute_force(corrupt, output, kEdge).verdict, Verdict::kAgreement);
    const Bytes clean = compute_unit(*unit.workload, 5, unit.input, serial);
    EXPECT_EQ(compare_with_brute_force(unit.input, clean, kEdge).verdict, Verdict::kAgreement);
  }
}

// T5b ------------------------------------------------------------------------

TEST(RaytraceUnrunChecks, ABufferOfTheWrongSizeIsNotPerformedAndNeverAPass) {
  SerialExecutor serial;
  const Unit unit = unit_on(simd::IsaPath::kScalar, 6);
  const Bytes output = compute_unit(*unit.workload, 6, unit.input, serial);
  const Bytes short_input(unit.input.begin(), unit.input.end() - 1);
  Bytes long_input = unit.input;
  long_input.push_back(std::byte{0});
  const Bytes short_output(output.begin(), output.end() - 1);
  Bytes long_output = output;
  long_output.push_back(std::byte{0});
  for (const Comparison& comparison : {compare_with_brute_force(short_input, output, kEdge),
                                       compare_with_brute_force(long_input, output, kEdge),
                                       compare_with_brute_force(unit.input, short_output, kEdge),
                                       compare_with_brute_force(unit.input, long_output, kEdge),
                                       compare_with_brute_force({}, {}, kEdge)}) {
    EXPECT_EQ(comparison.verdict, Verdict::kNotPerformed);
    EXPECT_TRUE(comparison.pixels.empty());
    EXPECT_TRUE(comparison.tiles.empty());
  }
}

TEST(RaytraceUnrunChecks, TheSameOutputFailsAgainstTheWrongTileEdge) {
  // The tile edge is part of the comparison: sums made at one edge are not the
  // sums of another, and the comparison says so rather than passing.
  SerialExecutor serial;
  const Unit unit = unit_on(simd::IsaPath::kScalar, 7);
  const Bytes output = compute_unit(*unit.workload, 7, unit.input, serial);
  EXPECT_EQ(compare_with_brute_force(unit.input, output, kEdge).verdict, Verdict::kAgreement);
  const Comparison wrong = compare_with_brute_force(unit.input, output, 8);
  EXPECT_EQ(wrong.verdict, Verdict::kMismatch);
  EXPECT_TRUE(wrong.pixels.empty());
  EXPECT_FALSE(wrong.tiles.empty());
}

}  // namespace
}  // namespace loadforge::tests::raytrace
