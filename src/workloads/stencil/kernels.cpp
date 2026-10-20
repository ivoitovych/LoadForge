// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/stencil/kernels.hpp"

#include <bit>
#include <cstddef>
#include <cstring>
#include <span>

#include "workloads/stencil/spec.hpp"

namespace loadforge::workloads::stencil {

namespace {

static_assert(std::endian::native == std::endian::little,
              "grids are little-endian doubles and the kernels read them in place");

constexpr double kHalf = 0.5;
constexpr double kEighth = 0.125;
constexpr std::size_t kCellBytes = sizeof(double);

double load(std::span<const std::byte> grid, std::size_t cell) noexcept {
  double value = 0.0;
  std::memcpy(&value, grid.subspan(cell * kCellBytes, kCellBytes).data(), kCellBytes);
  return value;
}

void store(std::span<std::byte> grid, std::size_t cell, double value) noexcept {
  std::memcpy(grid.subspan(cell * kCellBytes, kCellBytes).data(), &value, kCellBytes);
}

/// The expression every path evaluates, for one cell.
double update(std::span<const std::byte> src, std::size_t cell) noexcept {
  const double north = load(src, cell - kEdge);
  const double south = load(src, cell + kEdge);
  const double west = load(src, cell - 1);
  const double east = load(src, cell + 1);
  return load(src, cell) * kHalf + ((north + south) + (east + west)) * kEighth;
}

void scalar_kernel(std::span<const std::byte> src, std::span<std::byte> dst,
                   const Rect& rect) noexcept {
  for (std::size_t row = rect.row_begin; row < rect.row_end; ++row) {
    for (std::size_t column = rect.column_begin; column < rect.column_end; ++column) {
      const std::size_t cell = row * kEdge + column;
      store(dst, cell, update(src, cell));
    }
  }
}

#if !defined(LOADFORGE_SIMD_SCALAR) && (defined(__x86_64__) || defined(__aarch64__))

template <typename Vector>
void load_vector(std::span<const std::byte> grid, std::size_t cell, Vector& out) noexcept {
  std::memcpy(&out, grid.subspan(cell * kCellBytes, sizeof(Vector)).data(), sizeof(Vector));
}

template <typename Vector>
void store_vector(std::span<std::byte> grid, std::size_t cell, const Vector& value) noexcept {
  std::memcpy(grid.subspan(cell * kCellBytes, sizeof(Vector)).data(), &value, sizeof(Vector));
}

/// A vector of cells at a time, then the cells that do not fill one by the
/// scalar expression, which is the same expression.
template <typename Vector>
[[gnu::always_inline]] inline void vector_kernel(std::span<const std::byte> src,
                                                 std::span<std::byte> dst,
                                                 const Rect& rect) noexcept {
  constexpr std::size_t kLanes = sizeof(Vector) / kCellBytes;
  for (std::size_t row = rect.row_begin; row < rect.row_end; ++row) {
    std::size_t cell = row * kEdge + rect.column_begin;
    const std::size_t end = row * kEdge + rect.column_end;
    for (; cell + kLanes <= end; cell += kLanes) {
      Vector centre;
      Vector north;
      Vector south;
      Vector west;
      Vector east;
      load_vector(src, cell, centre);
      load_vector(src, cell - kEdge, north);
      load_vector(src, cell + kEdge, south);
      load_vector(src, cell - 1, west);
      load_vector(src, cell + 1, east);
      const Vector result = centre * kHalf + ((north + south) + (east + west)) * kEighth;
      store_vector(dst, cell, result);
    }
    for (; cell < end; ++cell) {
      store(dst, cell, update(src, cell));
    }
  }
}

#endif

#if !defined(LOADFORGE_SIMD_SCALAR) && defined(__x86_64__)

using Vector4 = double __attribute__((vector_size(32)));

[[gnu::target("avx2")]] void avx2_kernel(std::span<const std::byte> src, std::span<std::byte> dst,
                                         const Rect& rect) noexcept {
  vector_kernel<Vector4>(src, dst, rect);
}

constexpr SweepKernel kAvx2Kernel = avx2_kernel;
constexpr SweepKernel kNeonKernel = nullptr;

#elif !defined(LOADFORGE_SIMD_SCALAR) && defined(__aarch64__)

using Vector2 = double __attribute__((vector_size(16)));

void neon_kernel(std::span<const std::byte> src, std::span<std::byte> dst,
                 const Rect& rect) noexcept {
  vector_kernel<Vector2>(src, dst, rect);
}

constexpr SweepKernel kAvx2Kernel = nullptr;
constexpr SweepKernel kNeonKernel = neon_kernel;

#else

constexpr SweepKernel kAvx2Kernel = nullptr;
constexpr SweepKernel kNeonKernel = nullptr;

#endif

}  // namespace

simd::KernelTable<SweepKernel> kernel_table() noexcept {
  simd::KernelTable<SweepKernel> table;
  table.scalar = scalar_kernel;
  table.avx2 = kAvx2Kernel;
  table.neon = kNeonKernel;
  return table;
}

}  // namespace loadforge::workloads::stencil
