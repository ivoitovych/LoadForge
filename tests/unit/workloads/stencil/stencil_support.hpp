// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_STENCIL_STENCIL_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_STENCIL_STENCIL_SUPPORT_HPP

// What every tier of the stencil workload's tests shares: executors, grid
// access, an independent generator of the unit's input, an exact reference
// and the spec's sampled-window check. All of it is written from
// src/workloads/stencil/spec.hpp and never from the kernels (verification.md
// §3): the reference is integer arithmetic on the exact rationals the sweeps
// produce, the window check is the specification's own procedure.
//
// This header is shared by the tests of this one module only (ADR 0003 is
// about sharing between components).

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/oracle.hpp"
#include "worker/executor.hpp"
#include "workloads/stencil/spec.hpp"
#include "workloads/stencil/stencil.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::stencil::testing {

using Bytes = std::vector<std::byte>;

// ---- executors --------------------------------------------------------------

class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// One thread, tasks in reverse order: the order differs, the thread does not.
class ReverseExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = count; index-- > 0;) {
      task(context, index, 0);
    }
  }
};

/// `threads` threads started per run; slot s takes indices s, s + n, ... when
/// `interleaved`, and the s-th contiguous block otherwise, so which thread
/// runs a tile is not the same in the two.
class ThreadedExecutor final : public worker::Executor {
 public:
  explicit ThreadedExecutor(std::size_t threads, bool interleaved = true)
      : threads_(threads), interleaved_(interleaved) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([this, slot, count, task, context] {
        if (interleaved_) {
          for (std::size_t index = slot; index < count; index += threads_) {
            task(context, index, slot);
          }
        } else {
          const std::size_t begin = slot * count / threads_;
          const std::size_t end = (slot + 1) * count / threads_;
          for (std::size_t index = begin; index < end; ++index) {
            task(context, index, slot);
          }
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  std::size_t threads_;
  bool interleaved_;
};

// ---- creating and computing --------------------------------------------------

inline WorkloadConfig config_for(simd::IsaPath path, std::uint32_t tile = 0,
                                 std::uint64_t units = 4, std::uint64_t seed = 0x5EED) {
  WorkloadConfig config;
  config.seed = seed;
  config.footprint_bytes = units * (kInputBytes + kOutputBytes);
  config.decomposition = tile;
  config.isa = path;
  return config;
}

inline std::unique_ptr<Workload> make(const WorkloadConfig& config) {
  auto created = create(config);
  return created.has_value() ? std::move(created).value() : nullptr;
}

inline Bytes generated(const Workload& workload, std::uint64_t index) {
  Bytes input(workload.plan().input_bytes);
  workload.generate(index, input);
  return input;
}

inline Bytes computed(const Workload& workload, std::uint64_t index, const Bytes& input,
                      worker::Executor& executor, std::byte fill = std::byte{0}) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(index, input, output, scratch, executor);
  return output;
}

/// The paths this build has a kernel for that this machine can run.
inline std::vector<simd::IsaPath> runnable_paths() {
  std::vector<simd::IsaPath> paths = {simd::IsaPath::kScalar};
#if !defined(LOADFORGE_SIMD_SCALAR) && defined(__x86_64__)
  if (__builtin_cpu_supports("avx2")) {
    paths.push_back(simd::IsaPath::kAvx2);
  }
#elif !defined(LOADFORGE_SIMD_SCALAR) && defined(__aarch64__)
  paths.push_back(simd::IsaPath::kNeon);
#endif
  return paths;
}

inline simd::IsaPath widest_runnable_path() { return runnable_paths().back(); }

// ---- grid access ----------------------------------------------------------------

inline double cell_of(std::span<const std::byte> grid, std::size_t row, std::size_t column) {
  double value = 0.0;
  std::memcpy(&value, grid.data() + (row * kEdge + column) * sizeof(double), sizeof value);
  return value;
}

inline void set_cell(std::span<std::byte> grid, std::size_t row, std::size_t column, double value) {
  std::memcpy(grid.data() + (row * kEdge + column) * sizeof(double), &value, sizeof value);
}

inline Bytes grid_of(double value) {
  Bytes grid(kGridBytes);
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    set_cell(grid, cell / kEdge, cell % kEdge, value);
  }
  return grid;
}

/// The unit's input as the specification defines it, from the generator and
/// entry_value alone.
inline Bytes spec_input(std::uint64_t seed, std::uint64_t index) {
  Bytes grid(kGridBytes);
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    set_cell(grid, cell / kEdge, cell % kEdge,
             entry_value(rng::u64_of(rng::block_at(seed, index, cell))));
  }
  return grid;
}

/// A grid of entries drawn as the specification draws them, from a stream of
/// the caller's choosing, optionally scaled to a magnitude by an exact power
/// of two.
inline Bytes random_grid(std::uint64_t seed, std::uint64_t stream, int exponent = 0) {
  Bytes grid = spec_input(seed, stream);
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    set_cell(grid, cell / kEdge, cell % kEdge,
             std::ldexp(cell_of(grid, cell / kEdge, cell % kEdge), exponent));
  }
  return grid;
}

inline double max_magnitude(std::span<const std::byte> grid) {
  double largest = 0.0;
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    largest = std::max(largest, std::fabs(cell_of(grid, cell / kEdge, cell % kEdge)));
  }
  return largest;
}

// ---- the exact reference ---------------------------------------------------------

__extension__ using Exact = __int128;

/// Every cell of the grid after kSteps sweeps as an integer multiple of 2^-76.
/// The input is a multiple of 2^-52, each sweep (4 u + N + S + E + W) / 8
/// divides by 8 exactly while the numerator keeps three more trailing zero
/// bits than the last, and 52 + 3 * 8 = 76: no rounding anywhere. Valid for an
/// input whose cells are multiples of 2^-52 with magnitude at most 1.
inline std::vector<Exact> exact_after_sweeps(std::span<const std::byte> input,
                                             std::size_t sweeps = kSteps) {
  std::vector<Exact> grid(kEdge * kEdge);
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    const double scaled = std::ldexp(cell_of(input, cell / kEdge, cell % kEdge), 52);
    grid[cell] = static_cast<Exact>(static_cast<std::int64_t>(scaled)) * (Exact{1} << 24);
  }
  std::vector<Exact> next(grid);
  for (std::size_t sweep = 0; sweep < sweeps; ++sweep) {
    for (std::size_t row = 1; row + 1 < kEdge; ++row) {
      for (std::size_t column = 1; column + 1 < kEdge; ++column) {
        const std::size_t cell = row * kEdge + column;
        next[cell] = (4 * grid[cell] + grid[cell - kEdge] + grid[cell + kEdge] + grid[cell - 1] +
                      grid[cell + 1]) /
                     8;
      }
    }
    grid.swap(next);
  }
  return grid;
}

inline long double exact_value(Exact scaled) {
  return std::ldexp(static_cast<long double>(scaled), -76);
}

/// The largest |output - exact| over every cell, and whether the ring is the
/// input's ring bit for bit.
struct Deviation {
  long double largest = 0.0L;
  std::size_t cell = 0;
};

inline Deviation deviation_from_exact(std::span<const std::byte> output,
                                      const std::vector<Exact>& exact) {
  Deviation worst;
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    const long double difference =
        std::fabs(static_cast<long double>(cell_of(output, cell / kEdge, cell % kEdge)) -
                  exact_value(exact[cell]));
    if (difference > worst.largest) {
      worst = {difference, cell};
    }
  }
  return worst;
}

// ---- the sampled-window check of the specification ---------------------------------

/// The origin of window `window` (0..3) for unit `index`: the corners, then
/// two positions that are a function of the index alone.
inline std::pair<std::size_t, std::size_t> window_origin(std::uint64_t index, std::size_t window) {
  const std::size_t span = kEdge - kRegionEdge + 1;
  if (window == 0) {
    return {0, 0};
  }
  if (window == 1) {
    return {kEdge - kRegionEdge, kEdge - kRegionEdge};
  }
  const std::uint64_t mixed = rng::u64_of(rng::block_at(0xC0FFEE, index, window));
  return {static_cast<std::size_t>(mixed % span), static_cast<std::size_t>((mixed >> 32U) % span)};
}

/// The evaluated tolerance of a verdict for an input whose largest cell is `m0`.
inline double window_tolerance(double m0) {
  return cell_tolerance_factor() * m0 * (1.0 + kEvaluationSlack);
}

/// The specification's check, written from the specification: each window is
/// swept by plain scalar code from the input, over the window grown by kSteps
/// cells and clipped to the grid, shrinking one cell per side per sweep,
/// holding the ring fixed; and compared with the output cell by cell.
inline verification::Verdict window_check(const verification::UnitEvidence& evidence) {
  using verification::Agreement;
  using verification::BoundedMismatch;
  using verification::NotChecked;
  if (evidence.input.size() != kGridBytes || evidence.output.size() != kGridBytes) {
    return NotChecked{"stencil windows", "evidence is not of the sizes the specification has", 0};
  }
  double m0 = 0.0;
  for (std::size_t cell = 0; cell < kEdge * kEdge; ++cell) {
    const double value = cell_of(evidence.input, cell / kEdge, cell % kEdge);
    if (!std::isfinite(value) || std::fabs(value) > 1.0) {
      return NotChecked{"stencil windows", "an input cell is not finite or exceeds 1 in magnitude",
                        0};
    }
    m0 = std::max(m0, std::fabs(value));
  }
  if (m0 == 0.0) {
    return NotChecked{"stencil windows", "every input cell is zero, so no tolerance exists", 0};
  }
  const double tolerance = window_tolerance(m0);

  std::uint64_t compared = 0;
  for (std::size_t window = 0; window < kRegions; ++window) {
    const auto [origin_row, origin_column] = window_origin(evidence.index, window);
    const std::size_t lo_row = origin_row > kSteps ? origin_row - kSteps : 0;
    const std::size_t lo_column = origin_column > kSteps ? origin_column - kSteps : 0;
    const std::size_t hi_row = std::min(origin_row + kRegionEdge + kSteps, kEdge);
    const std::size_t hi_column = std::min(origin_column + kRegionEdge + kSteps, kEdge);
    const std::size_t rows = hi_row - lo_row;
    const std::size_t columns = hi_column - lo_column;

    std::vector<double> now(rows * columns);
    for (std::size_t row = 0; row < rows; ++row) {
      for (std::size_t column = 0; column < columns; ++column) {
        now[row * columns + column] = cell_of(evidence.input, lo_row + row, lo_column + column);
      }
    }
    std::vector<double> next(now);
    for (std::size_t sweep = 0; sweep < kSteps; ++sweep) {
      for (std::size_t row = 1; row + 1 < rows; ++row) {
        for (std::size_t column = 1; column + 1 < columns; ++column) {
          const std::size_t grid_row = lo_row + row;
          const std::size_t grid_column = lo_column + column;
          if (is_ring(grid_row, grid_column)) {
            continue;
          }
          const std::size_t at = row * columns + column;
          next[at] = 0.5 * now[at] + 0.125 * now[at - columns] + 0.125 * now[at + columns] +
                     0.125 * now[at - 1] + 0.125 * now[at + 1];
        }
      }
      now.swap(next);
    }
    for (std::size_t row = origin_row; row < origin_row + kRegionEdge; ++row) {
      for (std::size_t column = origin_column; column < origin_column + kRegionEdge; ++column) {
        ++compared;
        const double expected = now[(row - lo_row) * columns + (column - lo_column)];
        const double residual = std::fabs(cell_of(evidence.output, row, column) - expected);
        if (!(residual <= tolerance)) {
          BoundedMismatch mismatch;
          mismatch.check = "stencil-cell";
          mismatch.residual = residual;
          mismatch.tolerance = tolerance;
          mismatch.element = row * kEdge + column;
          mismatch.blocks_compared = compared;
          return mismatch;
        }
      }
    }
  }
  return Agreement{compared};
}

}  // namespace loadforge::workloads::stencil::testing

#endif
