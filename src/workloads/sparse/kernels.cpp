// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/sparse/kernels.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace loadforge::workloads::sparse {

RowRange row_range(const Matrix& matrix, std::size_t row) noexcept {
  const std::size_t first = std::min<std::size_t>(load_u32(matrix.row_ptr, row), kNonZeros);
  const std::size_t last = std::min<std::size_t>(load_u32(matrix.row_ptr, row + 1), kNonZeros);
  const std::size_t length = last > first ? std::min(last - first, kMaxRowEntries) : 0;
  return RowRange{first, length};
}

double reduce_tree(std::span<double> partials) noexcept {
  for (std::size_t width = partials.size(); width > 1; width /= 2) {
    for (std::size_t pair = 0; pair < width / 2; ++pair) {
      partials[pair] = partials[2 * pair] + partials[2 * pair + 1];
    }
  }
  return partials[0];
}

namespace {

/// The stored value times the vector entry its column names, entry `entry`.
double product(const Matrix& matrix, std::span<const double> vector, std::size_t entry) noexcept {
  const std::size_t column = load_u32(matrix.columns, entry) & (kRows - 1);
  return load_f64(matrix.values, entry) * vector[column];
}

double scalar_init(const Work& work, Rows rows, double /*coefficient*/) noexcept {
  double sum = 0.0;
  for (std::size_t row = rows.first; row < rows.last; ++row) {
    const double b = load_f64(work.rhs, row);
    work.x[row] = 0.0;
    work.r[row] = b;
    work.p[row] = b;
    const double square = b * b;
    sum += square;
  }
  return sum;
}

double scalar_spmv_dot(const Work& work, Rows rows, double /*coefficient*/) noexcept {
  double sum = 0.0;
  for (std::size_t row = rows.first; row < rows.last; ++row) {
    const RowRange range = row_range(work.matrix, row);
    double dot = 0.0;
    for (std::size_t k = 0; k < range.length; ++k) {
      dot += product(work.matrix, work.p, range.first + k);
    }
    work.q[row] = dot;
    const double term = work.p[row] * dot;
    sum += term;
  }
  return sum;
}

double scalar_update_xr(const Work& work, Rows rows, double alpha) noexcept {
  double sum = 0.0;
  for (std::size_t row = rows.first; row < rows.last; ++row) {
    work.x[row] += alpha * work.p[row];
    work.r[row] -= alpha * work.q[row];
    const double square = work.r[row] * work.r[row];
    sum += square;
  }
  return sum;
}

double scalar_update_p(const Work& work, Rows rows, double beta) noexcept {
  for (std::size_t row = rows.first; row < rows.last; ++row) {
    work.p[row] = work.r[row] + beta * work.p[row];
  }
  return 0.0;
}

double scalar_finish(const Work& work, Rows rows, double /*coefficient*/) noexcept {
  for (std::size_t row = rows.first; row < rows.last; ++row) {
    const RowRange range = row_range(work.matrix, row);
    double residual = load_f64(work.rhs, row);
    for (std::size_t k = 0; k < range.length; ++k) {
      residual -= product(work.matrix, work.x, range.first + k);
    }
    store_f64(work.output, row, work.x[row]);
    store_f64(work.output, kRows + row, residual);
  }
  return 0.0;
}

}  // namespace

const Kernels& scalar_kernels() noexcept {
  static constexpr Kernels kScalar{scalar_init, scalar_spmv_dot, scalar_update_xr, scalar_update_p,
                                   scalar_finish};
  return kScalar;
}

}  // namespace loadforge::workloads::sparse
