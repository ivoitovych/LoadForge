// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_SPARSE_KERNELS_HPP
#define LOADFORGE_WORKLOADS_SPARSE_KERNELS_HPP

// The sparse workload's kernels: what runs on one chunk of rows, per ISA path
// (spec.hpp, "DECOMPOSITION"). The solver (sparse.cpp) owns the shape -- the
// chunks, the reduction tree, the iteration -- and a kernel owns only the
// order of the sums inside one chunk and inside one row, which the
// specification leaves to the ISA path.
//
// Every kernel is total over its input bytes (workload.hpp: compute() is
// total). The matrix is read from the unit's input, which may be corrupt, so
// the structure is made safe where it is read and never trusted:
//   * a column is reduced modulo kRows, which is a power of two, so a gather
//     stays inside the vector it reads for any 32-bit value, and for a valid
//     column is the column itself;
//   * a row's entries are [row_ptr[i], row_ptr[i + 1]) clamped into
//     [0, kNonZeros) and to at most kMaxRowEntries, so a row_ptr that is out of
//     order, out of range or huge costs at most the work of a row of the
//     specification, never more, and for a valid row_ptr is the row.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "workloads/sparse/spec.hpp"

namespace loadforge::workloads::sparse {

static_assert(std::endian::native == std::endian::little,
              "the unit's format is little-endian and is read in place");
static_assert((kRows & (kRows - 1)) == 0, "a column is made safe by masking, which needs 2^k rows");

/// The most entries a row has: the diagonal and each offset on both sides.
inline constexpr std::size_t kMaxRowEntries = 2 * kOffsets.size() + 1;

/// The chunks a unit has at the smallest chunk size: the reduction's widest
/// partial-sum array.
inline constexpr std::size_t kMaxChunks = kRows / kChunkRows.front();

/// The 64-bit-or-narrower scalar at index `index` of an array of T held in
/// bytes (a memory copy, so no alignment is assumed of the input).
[[nodiscard]] inline double load_f64(std::span<const std::byte> bytes, std::size_t index) noexcept {
  double value = 0.0;
  std::memcpy(&value, bytes.subspan(index * sizeof(double), sizeof(double)).data(),
              sizeof(double));
  return value;
}

[[nodiscard]] inline std::uint32_t load_u32(std::span<const std::byte> bytes,
                                            std::size_t index) noexcept {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.subspan(index * sizeof(std::uint32_t), sizeof(std::uint32_t)).data(),
              sizeof(std::uint32_t));
  return value;
}

inline void store_f64(std::span<std::byte> bytes, std::size_t index, double value) noexcept {
  std::memcpy(bytes.subspan(index * sizeof(double), sizeof(double)).data(), &value,
              sizeof(double));
}

inline void store_u32(std::span<std::byte> bytes, std::size_t index, std::uint32_t value) noexcept {
  std::memcpy(bytes.subspan(index * sizeof(std::uint32_t), sizeof(std::uint32_t)).data(), &value,
              sizeof(std::uint32_t));
}

/// The matrix, as the three parts of the input that hold it.
struct Matrix {
  std::span<const std::byte> values;   ///< kNonZeros doubles
  std::span<const std::byte> columns;  ///< kNonZeros u32
  std::span<const std::byte> row_ptr;  ///< kRows + 1 u32
};

/// Where a row's stored entries are: `length` entries from `first`, made safe
/// as above.
struct RowRange {
  std::size_t first = 0;
  std::size_t length = 0;
};

/// Precondition: row < kRows.
[[nodiscard]] RowRange row_range(const Matrix& matrix, std::size_t row) noexcept;

/// What a kernel works on: the matrix, the right-hand side, the four vectors of
/// the recurrence (each kRows doubles) and the output, as bytes.
struct Work {
  Matrix matrix;
  std::span<const std::byte> rhs;  ///< kRows doubles
  std::span<double> x;
  std::span<double> r;
  std::span<double> p;
  std::span<double> q;
  std::span<std::byte> output;  ///< kOutputBytes
};

/// The rows [first, last) of one chunk.
struct Rows {
  std::size_t first = 0;
  std::size_t last = 0;
};

/// A kernel does one step of the recurrence on one chunk's rows and returns
/// the chunk's partial sum, in the kernel's order, or 0.0 for a step that has
/// none. `coefficient` is alpha or beta where the step takes one.
using Kernel = double (*)(const Work& work, Rows rows, double coefficient) noexcept;

/// The steps of one unit, for one ISA path.
///
///   init       x = 0, r = p = b; the partial of r . r (the first rr).
///   spmv_dot   q = A p; the partial of p . q.
///   update_xr  x += alpha p, r -= alpha q; the partial of r . r.
///   update_p   p = r + beta p.
///   finish     the output: x, then the explicit residual b - A x.
struct Kernels {
  Kernel init = nullptr;
  Kernel spmv_dot = nullptr;
  Kernel update_xr = nullptr;
  Kernel update_p = nullptr;
  Kernel finish = nullptr;
};

/// The scalar kernels: portable C++, each sum in ascending index order, a
/// product rounded and then added (never fused), so the same bits on every
/// architecture (determinism.md §3).
[[nodiscard]] const Kernels& scalar_kernels() noexcept;

/// The reduction tree of the specification: adjacent pairs first, p_0 + p_1,
/// p_2 + p_3, ..., then adjacent pairs of those sums, to the root, each sum
/// "left + right". `partials` holds a power of two of values (it is
/// overwritten). The shape depends on the count and on nothing else.
[[nodiscard]] double reduce_tree(std::span<double> partials) noexcept;

}  // namespace loadforge::workloads::sparse

#endif
