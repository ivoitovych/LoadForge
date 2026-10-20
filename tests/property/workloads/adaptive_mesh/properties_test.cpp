// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4 for the adaptive mesh workload: properties of the mesh and the integrals
// that hold for every input, checked over generated units and hand-made
// families, with no reference to a recomputation.
//
//   * Shape. A coarse cell has 1 + 3 s leaves for s splits, so its count is 1
//     mod 3 and at most kMaxLeavesPerCell; every integral is finite and
//     positive, above the cell's area times the smallest f (2^-5) and at most
//     the cell's area times the largest (256 hotspots at distance zero).
//   * The mesh depends on the set of hotspots and not on their order: counts
//     are equal, and integrals agree within the bound for a change in the
//     order of the sum over hotspots (derived below).
//   * Replacing a hotspot by one outside the lattice never adds a leaf.
//   * The unit square's symmetries: reflecting or transposing every hotspot
//     reflects or transposes the counts exactly, and the integrals within the
//     tolerance (the leaf values are identical, only the order of the sum
//     over a cell's leaves differs).
//
// THE ORDER BOUND. Permuting the hotspots changes each leaf's f by at most
// 2 gamma_256 f (each is within gamma_256 of the exact sum of positive terms,
// 256 additions at most), so each x_leaf differs by that relative amount, the
// exact sum of a cell's leaves by the same, and the two computed integrals,
// each within gamma_{m-1} of its exact sum, by 2 gamma_256 + 2 gamma_{m-1}
// relative to the larger.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "primitives/rng/philox.hpp"
#include "unit/workloads/adaptive_mesh/support.hpp"

namespace loadforge::workloads::adaptive_mesh::testing {
namespace {

double gamma(std::size_t k) {
  const double ku = static_cast<double>(k) * kUnitRoundoff;
  return ku / (1.0 - ku);
}

struct Fixture {
  std::unique_ptr<Workload> workload = make(config_for(21, 64));
  Serial serial;

  Bytes run(const Hotspots& spots) { return computed(*workload, 0, input_of(spots), serial); }
  [[nodiscard]] Hotspots unit(std::uint64_t index) const {
    return hotspots_of(generated(*workload, index));
  }
};

TEST(AdaptiveMeshProperties, TheShapeOfAnyOutput) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  for (std::uint64_t index = 0; index < 12; ++index) {
    const Bytes output = f.run(f.unit(index));
    for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
      const std::uint32_t count = count_of(output, cell);
      const double integral = integral_of(output, cell);
      const double area = 1.0 / 256.0;
      EXPECT_EQ(count % 3, 1U) << "cell " << cell;
      EXPECT_LE(count, kMaxLeavesPerCell);
      EXPECT_TRUE(std::isfinite(integral));
      EXPECT_GT(integral, area / 32.0);
      EXPECT_LE(integral, area * 256.0 * (1.0 + 1e-12));
    }
  }
}

TEST(AdaptiveMeshProperties, OrderOfHotspotsChangesNoCountAndNoIntegralBeyondTheBound) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  for (std::uint64_t index = 0; index < 6; ++index) {
    const Hotspots spots = f.unit(index);
    Hotspots shuffled = spots;
    for (std::size_t at = shuffled.size(); at > 1; --at) {
      std::swap(shuffled[at - 1], shuffled[rng::u64_of(rng::block_at(3, index, at)) % at]);
    }
    const Bytes one = f.run(spots);
    const Bytes two = f.run(shuffled);
    for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
      ASSERT_EQ(count_of(one, cell), count_of(two, cell));
      const double a = integral_of(one, cell);
      const double b = integral_of(two, cell);
      const double bound = ((2.0 * gamma(kHotspots)) + (2.0 * gamma(count_of(one, cell)))) *
                           std::max(a, b) * (1.0 + 1e-9);
      EXPECT_LE(std::fabs(a - b), bound) << "cell " << cell;
    }
  }
}

TEST(AdaptiveMeshProperties, MovingAHotspotOutOfTheLatticeNeverAddsALeaf) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  const Hotspots spots = f.unit(2);
  const Bytes before = f.run(spots);
  for (std::size_t drop = 0; drop < 40; ++drop) {
    Hotspots fewer = spots;
    for (std::size_t h = 0; h <= drop * 6; ++h) {
      fewer[h] = {kLattice, kLattice};
    }
    const Bytes after = f.run(fewer);
    for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
      ASSERT_LE(count_of(after, cell), count_of(before, cell)) << "drop " << drop;
    }
  }
}

Hotspots reflected(const Hotspots& spots) {
  Hotspots out;
  for (const Hotspot& s : spots) {
    out.push_back({kLattice - 1 - s.x, s.y});
  }
  return out;
}

Hotspots transposed(const Hotspots& spots) {
  Hotspots out;
  for (const Hotspot& s : spots) {
    out.push_back({s.y, s.x});
  }
  return out;
}

void expect_same_cell(const Bytes& lhs, std::size_t lhs_cell, const Bytes& rhs,
                      std::size_t rhs_cell) {
  ASSERT_EQ(count_of(lhs, lhs_cell), count_of(rhs, rhs_cell));
  const double a = integral_of(lhs, lhs_cell);
  const double b = integral_of(rhs, rhs_cell);
  EXPECT_LE(std::fabs(a - b), cell_tolerance(count_of(lhs, lhs_cell), std::max(a, b)))
      << "cells " << lhs_cell << " and " << rhs_cell;
}

TEST(AdaptiveMeshProperties, ReflectingAndTransposingTheHotspotsReflectAndTransposeTheOutput) {
  Fixture f;
  ASSERT_NE(f.workload, nullptr);
  for (std::uint64_t index = 0; index < 4; ++index) {
    const Hotspots spots = f.unit(index);
    const Bytes plain = f.run(spots);
    const Bytes mirror = f.run(reflected(spots));
    const Bytes flip = f.run(transposed(spots));
    for (std::size_t cy = 0; cy < kBaseCells; ++cy) {
      for (std::size_t cx = 0; cx < kBaseCells; ++cx) {
        expect_same_cell(plain, (cy * kBaseCells) + cx, mirror,
                         (cy * kBaseCells) + (kBaseCells - 1 - cx));
        expect_same_cell(plain, (cy * kBaseCells) + cx, flip, (cx * kBaseCells) + cy);
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::workloads::adaptive_mesh::testing
