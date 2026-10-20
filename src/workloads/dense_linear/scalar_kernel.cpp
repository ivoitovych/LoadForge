// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scalar kernel (panel.hpp): the register-blocked loop every vector kernel
// has, written in portable C++. The block is kTileRows by kBlockColumns sums
// held in locals; each B row segment and each A entry is copied out of the
// byte buffer once per use, so the inner loop touches nothing but doubles.

#include <array>
#include <cstddef>
#include <cstring>
#include <span>

#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"

namespace loadforge::workloads::dense_linear {
namespace {

using Segment = std::array<double, kBlockColumns>;

/// Element `index` of a matrix held as bytes.
[[nodiscard]] double element(std::span<const std::byte> matrix, std::size_t index) noexcept {
  double value = 0.0;
  std::memcpy(&value, &matrix[index * sizeof(double)], sizeof value);
  return value;
}

/// kBlockColumns consecutive elements from `index`.
[[nodiscard]] Segment segment(std::span<const std::byte> matrix, std::size_t index) noexcept {
  Segment values{};
  std::memcpy(values.data(), &matrix[index * sizeof(double)], sizeof values);
  return values;
}

}  // namespace

void panel_scalar(const Panel& panel) noexcept {
  for (std::size_t column = 0; column < kOrder; column += kBlockColumns) {
    std::array<Segment, kTileRows> sums{};
    for (std::size_t k = panel.k0; k < panel.k0 + panel.depth; ++k) {
      const Segment b = segment(panel.b, k * kOrder + column);
      std::size_t row = panel.row;
      for (Segment& sum : sums) {
        const double a = element(panel.a, row * kOrder + k);
        for (std::size_t lane = 0; lane < kBlockColumns; ++lane) {
          // Two statements, two roundings: the product, then the sum.
          const double product = a * b.at(lane);
          sum.at(lane) = sum.at(lane) + product;
        }
        ++row;
      }
    }
    std::size_t row = panel.row;
    for (const Segment& sum : sums) {
      const std::size_t at = (row * kOrder + column) * sizeof(double);
      Segment c{};
      std::memcpy(c.data(), &panel.c[at], sizeof c);
      for (std::size_t lane = 0; lane < kBlockColumns; ++lane) {
        c.at(lane) = c.at(lane) + sum.at(lane);
      }
      std::memcpy(&panel.c[at], c.data(), sizeof c);
      ++row;
    }
  }
}

}  // namespace loadforge::workloads::dense_linear
