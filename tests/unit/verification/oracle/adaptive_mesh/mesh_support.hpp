// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_ADAPTIVE_MESH_MESH_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_ORACLE_ADAPTIVE_MESH_MESH_SUPPORT_HPP

// What the adaptive mesh oracle's tests share, in every tier: hotspots
// (generated as workloads/adaptive_mesh/spec.hpp says, or placed by hand), the
// units' input bytes, and a MODEL of the output built differently from the
// oracle on purpose: the cells that split are the occupied cells of each
// level of a lattice of sets, and the leaves are found with an explicit
// stack, where the oracle recurses. The values come from the specification's
// own helpers, as the specification requires. The model never calls the oracle.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <set>
#include <utility>
#include <vector>

#include "contracts/verification/oracle_case.hpp"
#include "primitives/rng/philox.hpp"
#include "workloads/adaptive_mesh/spec.hpp"

namespace loadforge::test::mesh {

namespace spec = workloads::adaptive_mesh;

using Bytes = std::vector<std::byte>;
using Hotspots = std::array<spec::Hotspot, spec::kHotspots>;

inline Hotspots generated(std::uint64_t seed, std::uint64_t index) {
  Hotspots hotspots{};
  for (std::size_t h = 0; h < spec::kHotspots; ++h) {
    hotspots[h] = spec::hotspot_of(rng::u64_of(rng::block_at(seed, index, h)));
  }
  return hotspots;
}

inline void put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
  for (std::size_t k = 0; k < 4; ++k) {
    bytes[at + k] = static_cast<std::byte>((value >> (8U * k)) & 0xFFU);
  }
}

inline void put64(Bytes& bytes, std::size_t at, std::uint64_t value) {
  for (std::size_t k = 0; k < 8; ++k) {
    bytes[at + k] = static_cast<std::byte>((value >> (8U * k)) & 0xFFU);
  }
}

inline std::uint32_t get32(const Bytes& bytes, std::size_t at) {
  std::uint32_t value = 0;
  for (std::size_t k = 0; k < 4; ++k) {
    value |= std::to_integer<std::uint32_t>(bytes[at + k]) << (8U * k);
  }
  return value;
}

inline double get_double(const Bytes& bytes, std::size_t at) {
  std::uint64_t value = 0;
  for (std::size_t k = 0; k < 8; ++k) {
    value |= std::to_integer<std::uint64_t>(bytes[at + k]) << (8U * k);
  }
  return std::bit_cast<double>(value);
}

inline void put_double(Bytes& bytes, std::size_t at, double value) {
  put64(bytes, at, std::bit_cast<std::uint64_t>(value));
}

inline Bytes bytes_of(const Hotspots& hotspots) {
  Bytes bytes(spec::kInputBytes);
  for (std::size_t h = 0; h < spec::kHotspots; ++h) {
    put32(bytes, h * spec::kHotspotBytes, hotspots[h].x);
    put32(bytes, h * spec::kHotspotBytes + 4, hotspots[h].y);
  }
  return bytes;
}

/// One leaf of a coarse cell, with its x_leaf.
struct Leaf {
  spec::Cell cell;
  double value = 0.0;
};

/// The leaves of coarse cell `c` in leaf order (depth first, children q = 0..3).
inline std::vector<Leaf> leaves_of(const Hotspots& hotspots, std::size_t c) {
  // splitting[level] holds the (ix, iy) of every cell of that level with a hotspot in it.
  std::array<std::set<std::pair<std::uint32_t, std::uint32_t>>, spec::kMaxLevel + 1> occupied;
  for (const spec::Hotspot& hotspot : hotspots) {
    for (std::uint32_t level = 0; level <= spec::kMaxLevel; ++level) {
      occupied[level].insert(
          {hotspot.x >> (spec::kMaxLevel - level), hotspot.y >> (spec::kMaxLevel - level)});
    }
  }
  std::vector<Leaf> leaves;
  std::vector<spec::Cell> stack = {spec::Cell{spec::kBaseLevel,
                                              static_cast<std::uint32_t>(c % spec::kBaseCells),
                                              static_cast<std::uint32_t>(c / spec::kBaseCells)}};
  while (!stack.empty()) {
    const spec::Cell cell = stack.back();
    stack.pop_back();
    if (cell.level < spec::kMaxLevel && occupied[cell.level].contains({cell.ix, cell.iy})) {
      for (std::uint32_t q = 4; q > 0; --q) {
        stack.push_back(spec::Cell{cell.level + 1, 2 * cell.ix + ((q - 1) & 1U),
                                   2 * cell.iy + ((q - 1) >> 1U)});
      }
      continue;
    }
    const spec::Point point{spec::cell_centre(cell.level, cell.ix),
                            spec::cell_centre(cell.level, cell.iy)};
    double f = 0.0;
    for (const spec::Hotspot& hotspot : hotspots) {
      f += spec::hotspot_term(point, hotspot);
    }
    leaves.push_back(Leaf{cell, f * spec::leaf_area(cell.level)});
  }
  return leaves;
}

/// c': the leaves added in leaf order, one by one, from +0.0.
inline double leaf_order_sum(const std::vector<Leaf>& leaves) {
  double total = 0.0;
  for (const Leaf& leaf : leaves) {
    total += leaf.value;
  }
  return total;
}

/// The same leaves added in the reverse order: what a thread that finishes
/// late would have produced.
inline double reverse_order_sum(const std::vector<Leaf>& leaves) {
  double total = 0.0;
  for (const Leaf& leaf : std::views::reverse(leaves)) {
    total += leaf.value;
  }
  return total;
}

/// The same leaves added pairwise.
// NOLINTNEXTLINE(misc-no-recursion): divide and conquer of bounded depth log2(n)
inline double pairwise_sum(const std::vector<double>& values, std::size_t from, std::size_t to) {
  if (to - from == 1) {
    return values[from];
  }
  const std::size_t middle = from + (to - from) / 2;
  return pairwise_sum(values, from, middle) + pairwise_sum(values, middle, to);
}

inline double pairwise_order_sum(const std::vector<Leaf>& leaves) {
  std::vector<double> values;
  values.reserve(leaves.size());
  for (const Leaf& leaf : leaves) {
    values.push_back(leaf.value);
  }
  return pairwise_sum(values, 0, values.size());
}

struct Model {
  std::array<std::vector<Leaf>, spec::kCoarseCells> leaves;
  std::array<double, spec::kCoarseCells> reference{};  ///< c' of every coarse cell
};

inline Model model_of(const Hotspots& hotspots) {
  Model model;
  for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
    model.leaves[c] = leaves_of(hotspots, c);
    model.reference[c] = leaf_order_sum(model.leaves[c]);
  }
  return model;
}

/// The output the specification gives for the model, with every integral c'.
inline Bytes render(const Model& model) {
  Bytes output(spec::kOutputBytes, std::byte{0});
  for (std::size_t c = 0; c < spec::kCoarseCells; ++c) {
    put_double(output, spec::integral_offset(c), model.reference[c]);
    put32(output, spec::count_offset(c), static_cast<std::uint32_t>(model.leaves[c].size()));
  }
  return output;
}

inline contracts::OracleCase unit_of(std::uint64_t index, const Hotspots& hotspots) {
  return contracts::OracleCase{index, bytes_of(hotspots), render(model_of(hotspots))};
}

/// 256 hotspots, one in every level-8 cell of coarse cell `c`: every cell under
/// it splits down to the finest level, and it has kMaxLeavesPerCell leaves.
inline Hotspots crowded(std::size_t c) {
  Hotspots hotspots{};
  const auto cx = static_cast<std::uint32_t>(c % spec::kBaseCells);
  const auto cy = static_cast<std::uint32_t>(c / spec::kBaseCells);
  for (std::uint32_t h = 0; h < spec::kHotspots; ++h) {
    hotspots[h] = spec::Hotspot{cx * 32 + (h % 16) * 2 + 1, cy * 32 + (h / 16) * 2};
  }
  return hotspots;
}

/// Every hotspot at one lattice position.
inline Hotspots all_at(std::uint32_t x, std::uint32_t y) {
  Hotspots hotspots{};
  hotspots.fill(spec::Hotspot{x, y});
  return hotspots;
}

}  // namespace loadforge::test::mesh

#endif
