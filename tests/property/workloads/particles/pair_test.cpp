// SPDX-License-Identifier: GPL-3.0-or-later
//
// The force on particles whose answer is a single term, so that it is known to
// the bit: hand-computed values, pairs across cell boundaries, and the pair at
// the greatest cell offset the search is meant to reach, for every offset of
// every decomposition.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "property/workloads/particles/lattice.hpp"
#include "unit/workloads/particles/support.hpp"
#include "workloads/particles/grid.hpp"

namespace loadforge::workloads::particles {
namespace {

using testing::Bytes;
using testing::Point;

/// The specification's term for particle i at `a` from particle j at `b`.
Point model_term(const Point& a, const Point& b) {
  const double dx = a[0] - b[0];
  const double dy = a[1] - b[1];
  const double dz = a[2] - b[2];
  const double q = dx * dx + dy * dy + dz * dz;
  const double t = 1.0 - q / 4.0;
  if (t <= 0.0) {
    return {0.0, 0.0, 0.0};
  }
  const double w = t * t;
  return {w * dx, w * dy, w * dz};
}

std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

/// The bits of the force on the second particle of a pair whose first feels
/// `value`: the negative, except that a component that is zero stays +0 (the
/// accumulator starts at +0 and adding -0 to it gives +0).
std::uint64_t opposite(double value) { return value == 0.0 ? 0U : bits(-value); }

/// Computes the unit and returns the forces on the first test.size() particles
/// and whether every parked particle's force is exactly +0.
struct Outcome {
  std::vector<Point> forces;
  bool parked_untouched = true;
};

Outcome run(const std::vector<Point>& test, std::uint32_t cells, simd::IsaPath path) {
  const auto workload = testing::make(testing::config_for(cells, path));
  EXPECT_NE(workload, nullptr);
  const Bytes input = testing::as_bytes(testing::isolated_unit(test));
  testing::PoolExecutor pool(3);
  const std::vector<double> forces = testing::as_doubles(testing::compute(*workload, 0, input, pool));
  Outcome outcome;
  for (std::size_t p = 0; p < test.size(); ++p) {
    outcome.forces.push_back({forces[3 * p], forces[3 * p + 1], forces[3 * p + 2]});
  }
  for (std::size_t k = 3 * test.size(); k < forces.size(); ++k) {
    outcome.parked_untouched = outcome.parked_untouched && bits(forces[k]) == 0;
  }
  return outcome;
}

class ParticlesPair : public ::testing::TestWithParam<std::tuple<std::uint32_t, simd::IsaPath>> {
 protected:
  std::uint32_t cells() const { return std::get<0>(GetParam()); }
  simd::IsaPath path() const { return std::get<1>(GetParam()); }
};

std::vector<std::tuple<std::uint32_t, simd::IsaPath>> combinations() {
  std::vector<std::tuple<std::uint32_t, simd::IsaPath>> all;
  for (const std::uint32_t cells : kCellsPerCutoff) {
    for (const simd::IsaPath path : testing::runnable_paths()) {
      all.emplace_back(cells, path);
    }
  }
  return all;
}

INSTANTIATE_TEST_SUITE_P(Everywhere, ParticlesPair, ::testing::ValuesIn(combinations()),
                         [](const auto& info) {
                           return "cells" + std::to_string(std::get<0>(info.param)) + "_" +
                                  std::string(simd::describe(std::get<1>(info.param)));
                         });

TEST_P(ParticlesPair, TwoParticlesAtDistanceOneRepelWithTheHandComputedForce) {
  const Outcome outcome = run({{9.0, 9.0, 9.0}, {10.0, 9.0, 9.0}}, cells(), path());
  // q = 1, t = 1 - 1/4 = 3/4, w = 9/16, d = -+1: all exact in binary.
  EXPECT_EQ(outcome.forces[0][0], -0.5625);
  EXPECT_EQ(outcome.forces[1][0], 0.5625);
  EXPECT_EQ(bits(outcome.forces[0][1]), 0U);
  EXPECT_EQ(bits(outcome.forces[0][2]), 0U);
  EXPECT_EQ(bits(outcome.forces[1][1]), 0U);
  EXPECT_EQ(bits(outcome.forces[1][2]), 0U);
  EXPECT_TRUE(outcome.parked_untouched);
}

TEST_P(ParticlesPair, TheForceOnTheSecondOfAPairIsTheNegativeOfTheFirstsToTheBit) {
  const Point a = {3.3, 7.1, 11.9};
  const Point b = {3.9, 7.7, 12.4};
  const Outcome outcome = run({a, b}, cells(), path());
  const Point expected = model_term(a, b);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_EQ(bits(outcome.forces[0][c]), bits(expected[c])) << c;
    EXPECT_EQ(bits(outcome.forces[1][c]), opposite(expected[c])) << c;
  }
  EXPECT_LT(outcome.forces[0][0], 0.0);
  EXPECT_TRUE(outcome.parked_untouched);
}

TEST_P(ParticlesPair, AtTheCutoffThereIsNoForceAndJustInsideThereIsATinyOne) {
  const double inside = std::nextafter(2.0, 0.0);
  const Outcome at = run({{9.0, 9.0, 9.0}, {11.0, 9.0, 9.0}}, cells(), path());
  EXPECT_EQ(bits(at.forces[0][0]), 0U);
  EXPECT_EQ(bits(at.forces[1][0]), 0U);
  // From x = 0 the sum is exact, so the distance really is the double below 2.
  const Outcome near = run({{0.0, 9.0, 9.0}, {inside, 9.0, 9.0}}, cells(), path());
  const Point expected = model_term({0.0, 9.0, 9.0}, {inside, 9.0, 9.0});
  EXPECT_EQ(bits(near.forces[0][0]), bits(expected[0]));
  EXPECT_LT(near.forces[0][0], 0.0);
  EXPECT_GT(near.forces[0][0], -1e-30) << "t is about 2^-51, w about 2^-102";
  const Outcome beyond = run({{0.0, 9.0, 9.0}, {std::nextafter(2.0, 3.0), 9.0, 9.0}}, cells(), path());
  EXPECT_EQ(bits(beyond.forces[0][0]), 0U);
}

TEST_P(ParticlesPair, ThreeOnALineHaveTheMiddleOneInBalance) {
  const Outcome outcome = run({{8.0, 9.0, 9.0}, {9.0, 9.0, 9.0}, {10.0, 9.0, 9.0}}, cells(), path());
  EXPECT_EQ(bits(outcome.forces[1][0]), 0U) << "equal and opposite pushes sum to exactly +0";
  // The ends feel the middle one at distance 1 and each other at exactly 2.
  EXPECT_EQ(outcome.forces[0][0], -0.5625);
  EXPECT_EQ(outcome.forces[2][0], 0.5625);
}

TEST_P(ParticlesPair, APairAcrossACellBoundaryInteractsAsOneInsideACellDoes) {
  const double edge = 2.0 / static_cast<double>(cells());
  for (const std::size_t axis : {std::size_t{0}, std::size_t{1}, std::size_t{2}}) {
    // The two sides of the boundary between cells 5 and 6 along `axis`.
    Point a = {9.1, 9.2, 9.3};
    Point b = a;
    a[axis] = 6.0 * edge - std::ldexp(1.0, -30);
    b[axis] = 6.0 * edge + std::ldexp(1.0, -30);
    b[(axis + 1) % 3] += 0.5;
    const Outcome outcome = run({a, b}, cells(), path());
    const Point expected = model_term(a, b);
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_EQ(bits(outcome.forces[0][c]), bits(expected[c])) << "axis " << axis << " c " << c;
      EXPECT_EQ(bits(outcome.forces[1][c]), opposite(expected[c]));
    }
    EXPECT_NE(bits(outcome.forces[0][axis]), 0U);
  }
}

TEST_P(ParticlesPair, ParticlesAtTheCornersAndFacesOfTheBoxInteractWithTheirNeighbours) {
  const double top = kBoxEdge - std::ldexp(1.0, -49);
  const std::vector<Point> test = {{0.0, 0.0, 0.0},    {0.5, 0.25, 0.75}, {top, top, top},
                                   {top - 0.5, top - 0.25, top - 0.75}};
  const Outcome outcome = run(test, cells(), path());
  const Point low = model_term(test[0], test[1]);
  const Point high = model_term(test[2], test[3]);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_EQ(bits(outcome.forces[0][c]), bits(low[c]));
    EXPECT_EQ(bits(outcome.forces[1][c]), opposite(low[c]));
    EXPECT_EQ(bits(outcome.forces[2][c]), bits(high[c]));
    EXPECT_EQ(bits(outcome.forces[3][c]), opposite(high[c]));
  }
  EXPECT_NE(bits(outcome.forces[0][0]), 0U);
  EXPECT_TRUE(outcome.parked_untouched);
}

TEST_P(ParticlesPair, ParticlesOnTheBoundaryOfTheBoxInEveryAxisAreCounted) {
  // Face of the box, not a corner: (0, y, z) and (x, 0, z) and (x, y, 0).
  const std::vector<Point> test = {{0.0, 8.0, 8.0}, {0.75, 8.5, 8.25}, {8.0, 0.0, 12.0},
                                   {8.25, 0.5, 12.5}};
  const Outcome outcome = run(test, cells(), path());
  const Point first = model_term(test[0], test[1]);
  const Point second = model_term(test[2], test[3]);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_EQ(bits(outcome.forces[0][c]), bits(first[c]));
    EXPECT_EQ(bits(outcome.forces[2][c]), bits(second[c]));
  }
}

/// The pair at cell offset `d` (second cell minus first, per axis) that is as
/// close as the cells allow: each particle at the corner of its cell nearest
/// the other's, inset by 2^-20 so that it stays in its cell. For d = 0 the two
/// share a cell.
std::pair<Point, Point> closest_pair(const std::array<int, 3>& offset, std::uint32_t cells,
                                     const Point& origin_cell) {
  const double edge = 2.0 / static_cast<double>(cells);
  const double inset = std::ldexp(1.0, -20);
  Point first{};
  Point second{};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const int d = offset[axis];
    const double a = origin_cell[axis] + (d < 0 ? -d : 0);  // first particle's cell index
    const double b = a + d;                                 // second particle's
    if (d > 0) {
      first[axis] = (a + 1.0) * edge - inset;
      second[axis] = b * edge + inset;
    } else if (d < 0) {
      first[axis] = a * edge + inset;
      second[axis] = (b + 1.0) * edge - inset;
    } else {
      first[axis] = a * edge + 0.3 * edge;
      second[axis] = a * edge + 0.6 * edge;
    }
  }
  return {first, second};
}

TEST_P(ParticlesPair, ThePairAtEveryCellOffsetIsFoundWhenTheCellsAreInReach) {
  const std::uint32_t cells_per_cutoff = cells();
  const int c = static_cast<int>(cells_per_cutoff);
  const Grid grid = make_grid(cells_per_cutoff);
  const double edge = 2.0 / static_cast<double>(c);
  // Offsets with a lexicographically positive first nonzero axis: the pair
  // (P, Q) at d is also the pair (Q, P) at -d, and both forces are checked.
  std::vector<std::array<int, 3>> offsets;
  for (int dz = -c; dz <= c; ++dz) {
    for (int dy = -c; dy <= c; ++dy) {
      for (int dx = -c; dx <= c; ++dx) {
        if (std::array<int, 3>{dz, dy, dx} >= std::array<int, 3>{0, 0, 0}) {
          offsets.push_back({dx, dy, dz});
        }
      }
    }
  }
  // Eight pairs per unit, one in each octant of the box, far enough apart that
  // the pairs do not meet: their bounding boxes (at most (c + 1) cells across)
  // start at cells 1 and 1 + 8 / edge * ... below.
  const int half = static_cast<int>(8.0 / edge);  // cells in half the box
  std::size_t checked_in_reach = 0;
  std::size_t checked_out_of_reach = 0;
  for (std::size_t first = 0; first < offsets.size(); first += 8) {
    std::vector<Point> test;
    std::vector<std::pair<Point, Point>> pairs;
    for (std::size_t slot = 0; slot < 8 && first + slot < offsets.size(); ++slot) {
      const Point origin = {static_cast<double>(1 + ((slot & 1U) != 0U ? half : 0)),
                            static_cast<double>(1 + ((slot & 2U) != 0U ? half : 0)),
                            static_cast<double>(1 + ((slot & 4U) != 0U ? half : 0))};
      pairs.push_back(closest_pair(offsets[first + slot], cells_per_cutoff, origin));
      test.push_back(pairs.back().first);
      test.push_back(pairs.back().second);
    }
    const Outcome outcome = run(test, cells_per_cutoff, path());
    EXPECT_TRUE(outcome.parked_untouched);
    for (std::size_t slot = 0; slot < pairs.size(); ++slot) {
      const auto& [p, q] = pairs[slot];
      // The cells are the intended ones.
      for (std::size_t axis = 0; axis < 3; ++axis) {
        ASSERT_EQ(static_cast<int>(cell_coordinate(q[axis], grid)) -
                      static_cast<int>(cell_coordinate(p[axis], grid)),
                  offsets[first + slot][axis]);
      }
      const Point expected = model_term(p, q);
      for (std::size_t axis = 0; axis < 3; ++axis) {
        EXPECT_EQ(bits(outcome.forces[2 * slot][axis]), bits(expected[axis]))
            << "offset " << offsets[first + slot][0] << " " << offsets[first + slot][1] << " "
            << offsets[first + slot][2] << " axis " << axis;
        EXPECT_EQ(bits(outcome.forces[2 * slot + 1][axis]), opposite(expected[axis]));
      }
      if (expected[0] != 0.0 || expected[1] != 0.0 || expected[2] != 0.0) {
        ++checked_in_reach;
      } else {
        ++checked_out_of_reach;
      }
    }
  }
  EXPECT_GT(checked_in_reach, offsets.size() / 2) << "most offsets hold an active pair";
  if (c == 4) {
    EXPECT_GT(checked_out_of_reach, 0U) << "the corners the search leaves out are beyond the cutoff";
  }
}

}  // namespace
}  // namespace loadforge::workloads::particles
