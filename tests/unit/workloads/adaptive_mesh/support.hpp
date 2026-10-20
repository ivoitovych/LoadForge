// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_ADAPTIVE_MESH_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_ADAPTIVE_MESH_SUPPORT_HPP

// What every tier of the adaptive mesh workload's tests shares: the byte
// layout of a unit, the executors the tests run units on (this module's own
// reading of worker/executor.hpp, run against ExecutorContract in
// executor_mock_test.cpp, ADR 0003), and an independent reference written from
// spec.hpp's prose: the mesh by plain recursion, the leaf values by a scalar
// formula of its own, and the coarse cells summed one by one in leaf order,
// which is what the specification says the oracle does.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "worker/executor.hpp"
#include "workloads/adaptive_mesh/adaptive_mesh.hpp"
#include "workloads/adaptive_mesh/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::adaptive_mesh::testing {

using Bytes = std::vector<std::byte>;
using Hotspots = std::vector<Hotspot>;

// ---- units as bytes ------------------------------------------------------------

inline std::uint32_t get_u32(const Bytes& bytes, std::size_t at) {
  std::uint32_t value = 0;
  std::memcpy(&value, &bytes.at(at), sizeof value);
  return value;
}

inline void put_u32(Bytes& bytes, std::size_t at, std::uint32_t value) {
  std::memcpy(&bytes.at(at), &value, sizeof value);
}

inline double get_double(const Bytes& bytes, std::size_t at) {
  double value = 0.0;
  std::memcpy(&value, &bytes.at(at), sizeof value);
  return value;
}

inline void put_double(Bytes& bytes, std::size_t at, double value) {
  std::memcpy(&bytes.at(at), &value, sizeof value);
}

inline double integral_of(const Bytes& output, std::size_t cell) {
  return get_double(output, integral_offset(cell));
}

inline std::uint32_t count_of(const Bytes& output, std::size_t cell) {
  return get_u32(output, count_offset(cell));
}

/// An input holding exactly `spots`, which must number kHotspots.
inline Bytes input_of(const Hotspots& spots) {
  Bytes input(kInputBytes);
  for (std::size_t h = 0; h < spots.size(); ++h) {
    put_u32(input, h * kHotspotBytes, spots.at(h).x);
    put_u32(input, (h * kHotspotBytes) + sizeof(std::uint32_t), spots.at(h).y);
  }
  return input;
}

inline Hotspot hotspot_in(const Bytes& input, std::size_t h) {
  return Hotspot{get_u32(input, h * kHotspotBytes),
                 get_u32(input, (h * kHotspotBytes) + sizeof(std::uint32_t))};
}

inline Hotspots hotspots_of(const Bytes& input) {
  Hotspots spots;
  for (std::size_t h = 0; h < kHotspots; ++h) {
    spots.push_back(hotspot_in(input, h));
  }
  return spots;
}

/// kHotspots copies of one hotspot, or `fill` of them at it and the rest at
/// the lattice corner (0, 0).
inline Hotspots crowd(Hotspot at, std::size_t count = kHotspots, Hotspot rest = {0, 0}) {
  Hotspots spots(kHotspots, rest);
  std::fill_n(spots.begin(), std::min(count, kHotspots), at);
  return spots;
}

// ---- the reference -------------------------------------------------------------

struct Leaf {
  std::uint32_t level;
  std::uint32_t ix;
  std::uint32_t iy;
};

inline bool inside(const Leaf& cell, const Hotspot& spot) {
  // The hotspot's lattice position, divided down to the cell's level.
  const std::uint32_t side = std::uint32_t{1} << (kMaxLevel - cell.level);
  return spot.x / side == cell.ix && spot.y / side == cell.iy;
}

/// The leaves under `cell`, depth first, children in the order q = 0..3, by
/// plain recursion.
inline void leaves_under(const Leaf& cell, const Hotspots& spots, std::vector<Leaf>& out) {
  const bool split = cell.level < kMaxLevel && std::any_of(spots.begin(), spots.end(),
                                                           [&](auto s) { return inside(cell, s); });
  if (!split) {
    out.push_back(cell);
    return;
  }
  for (std::uint32_t q = 0; q < 4; ++q) {
    leaves_under(Leaf{cell.level + 1, (2 * cell.ix) + (q & 1U), (2 * cell.iy) + (q >> 1U)}, spots,
                 out);
  }
}

inline std::vector<Leaf> leaves_of_cell(std::size_t cell, const Hotspots& spots) {
  std::vector<Leaf> out;
  leaves_under(Leaf{kBaseLevel, static_cast<std::uint32_t>(cell % kBaseCells),
                    static_cast<std::uint32_t>(cell / kBaseCells)},
               spots, out);
  return out;
}

/// A leaf's x_leaf by the formulas of the specification, written out here.
inline double leaf_value(const Leaf& leaf, const Hotspots& spots) {
  const double px =
      std::ldexp(static_cast<double>((2 * leaf.ix) + 1), -static_cast<int>(leaf.level + 1));
  const double py =
      std::ldexp(static_cast<double>((2 * leaf.iy) + 1), -static_cast<int>(leaf.level + 1));
  double f = 0.0;
  for (const Hotspot& spot : spots) {
    const double dx = px - std::ldexp(static_cast<double>((2 * std::uint64_t{spot.x}) + 1),
                                      -static_cast<int>(kMaxLevel + 1));
    const double dy = py - std::ldexp(static_cast<double>((2 * std::uint64_t{spot.y}) + 1),
                                      -static_cast<int>(kMaxLevel + 1));
    const double d2 = (dx * dx) + (dy * dy);
    f = f + (1.0 / (1.0 + (4096.0 * d2)));
  }
  return f * std::ldexp(1.0, -static_cast<int>(2 * leaf.level));
}

/// Coarse cell `cell`: its leaf count, c' (the leaves added one by one in
/// leaf order from +0.0), and E's neighbours' sum via long double for the
/// property tests that want a second summation.
struct CellReference {
  std::uint32_t leaves = 0;
  double value = 0.0;
  double long_double_value = 0.0;
};

inline CellReference reference_cell(std::size_t cell, const Hotspots& spots) {
  CellReference r;
  long double wide = 0.0L;
  for (const Leaf& leaf : leaves_of_cell(cell, spots)) {
    const double x = leaf_value(leaf, spots);
    r.value = r.value + x;
    wide += static_cast<long double>(x);
    ++r.leaves;
  }
  r.long_double_value = static_cast<double>(wide);
  return r;
}

/// Whether the stored integral of `cell` is within the derived tolerance of c'.
inline bool integral_within(const Bytes& output, std::size_t cell, const CellReference& r) {
  const double stored = integral_of(output, cell);
  return std::fabs(stored - r.value) <= cell_tolerance(r.leaves, r.value);
}

/// The specification's items in its order -- every leaf count, then every
/// integral -- as a message naming the first that fails ("" when none does).
inline std::string first_disagreement(const Bytes& output, const Hotspots& spots) {
  std::vector<CellReference> references;
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    references.push_back(reference_cell(cell, spots));
    if (count_of(output, cell) != references.back().leaves) {
      return "leaf count of cell " + std::to_string(cell);
    }
  }
  for (std::size_t cell = 0; cell < kCoarseCells; ++cell) {
    if (!integral_within(output, cell, references[cell])) {
      return "integral of cell " + std::to_string(cell);
    }
  }
  return {};
}

// ---- executors: this module's reading of worker/executor.hpp -------------------

/// Every index on the calling thread, in increasing order, all in slot 0.
class Serial final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// `threads` slots, run one after another on the calling thread, each taking
/// the indices a seeded shuffle deals it. Deterministic, so a test can name
/// the exact grouping of leaves into partial sums it ran, and the result
/// differing from another seed's is a fact about grouping and not about luck.
class Dealt final : public worker::Executor {
 public:
  Dealt(std::size_t threads, std::uint64_t seed) : threads_(threads), seed_(seed) {}

  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }

  void run(std::size_t count, Task task, void* context) noexcept override {
    const std::vector<std::size_t> order = permutation(count, seed_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      for (std::size_t at = slot; at < order.size(); at += threads_) {
        task(context, order[at], slot);
      }
    }
  }

  /// Fisher-Yates over [0, count), drawing from Philox stream `seed`.
  static std::vector<std::size_t> permutation(std::size_t count, std::uint64_t seed) {
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    for (std::size_t at = count; at > 1; --at) {
      const std::uint64_t draw = rng::u64_of(rng::block_at(0x5EED, seed, at));
      std::swap(order[at - 1], order[draw % at]);
    }
    return order;
  }

 private:
  std::size_t threads_;
  std::uint64_t seed_;
};

/// `threads` real threads per run, each with a slot of its own, running the
/// indices a seeded shuffle deals it. Threads are joined before run()
/// returns, which is what makes their writes visible to the caller.
class Shuffled final : public worker::Executor {
 public:
  Shuffled(std::size_t threads, std::uint64_t seed) : threads_(threads), seed_(seed) {}

  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }

  void run(std::size_t count, Task task, void* context) noexcept override {
    const std::vector<std::size_t> order = Dealt::permutation(count, seed_);
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([this, slot, &order, task, context] {
        for (std::size_t at = slot; at < order.size(); at += threads_) {
          task(context, order[at], slot);
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  std::size_t threads_;
  std::uint64_t seed_;
};

/// `threads` threads per run claiming indices from one counter: the dynamic
/// schedule, where which thread runs which index is the scheduler's choice.
/// The counter is seq_cst; its only job is that each index is claimed once.
class Claiming final : public worker::Executor {
 public:
  explicit Claiming(std::size_t threads) : threads_(threads) {}

  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }

  void run(std::size_t count, Task task, void* context) noexcept override {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([slot, count, task, context, &next] {
        for (std::size_t index = next.fetch_add(1, std::memory_order_seq_cst); index < count;
             index = next.fetch_add(1, std::memory_order_seq_cst)) {
          task(context, index, slot);
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  std::size_t threads_;
};

// ---- workloads -------------------------------------------------------------------

/// Room for `units` units.
inline std::uint64_t footprint_for(std::uint64_t units) {
  return units * (std::uint64_t{kInputBytes} + kOutputBytes);
}

inline WorkloadConfig config_for(std::uint64_t seed = 1, std::uint64_t units = 2) {
  return WorkloadConfig{seed, footprint_for(units), 0, simd::IsaPath::kScalar};
}

/// A workload that must be creatable; the test stops if it is not.
inline std::unique_ptr<Workload> make(const WorkloadConfig& config) {
  auto created = create(config);
  if (!created.has_value()) {
    ADD_FAILURE() << created.error().detail;
    return nullptr;
  }
  return std::move(created).value();
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

}  // namespace loadforge::workloads::adaptive_mesh::testing

#endif
