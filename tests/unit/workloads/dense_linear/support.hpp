// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_WORKLOADS_DENSE_LINEAR_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_WORKLOADS_DENSE_LINEAR_SUPPORT_HPP

// What every tier of the dense-linear workload's tests shares: reading and
// writing the byte matrices, the executors the tests run units on (this
// module's own reading of worker/executor.hpp, run against ExecutorContract in
// executor_mock_test.cpp, ADR 0003), the paths this machine can run, and the
// bound the tests check results against, derived below from spec.hpp's.
//
// THE BOUND
// ---------
// spec.hpp: a dot product of n terms in any order, with or without FMA, is
// within gamma_n * s of the exact value x, s = sum |a_k| |b_k| exactly, with
// gamma_n = n u / (1 - n u). The workload's c is such a dot product. A term
// is rounded once as a product (not at all on its own under FMA), at most
// kc - 1 times inside its panel (the first addition, to +0.0, is exact) and
// at most n/kc - 1 times as its panel joins c (the first, to the zeroed c, is
// exact): kc + n/kc - 1 <= kc * (n/kc) = n roundings, whatever the depth. The
// plain loop below is one too. Two of them therefore differ by
//
//     |c1 - c2| <= |c1 - x| + |x - c2| <= 2 gamma_n s.
//
// The tests have s only as computed, s~ = fl(sum |a_k||b_k|), a sum of
// non-negative terms, so s~ = s (1 + theta) with |theta| <= gamma_n and
// s <= s~ / (1 - gamma_n). Hence
//
//     |c1 - c2| <= 2 gamma_n s~ / (1 - gamma_n).
//
// Computing that right-hand side takes three roundings, and the left-hand
// subtraction one more, each a factor (1 + u) at worst; the tolerance is
// multiplied by (1 + 8u), which covers all four with room, and nothing in
// it is near underflow (a nonzero product of generated entries is at least
// 2^-104). That is tolerance(); within_bound() applies it.

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
#include <thread>
#include <vector>

#include "core/crc32.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"
#include "workloads/dense_linear/dense_linear.hpp"
#include "workloads/dense_linear/panel.hpp"
#include "workloads/dense_linear/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::dense_linear::testing {

using Bytes = std::vector<std::byte>;

inline constexpr std::size_t kN = kOrder;
inline constexpr std::size_t kMatrix = kN * kN;

// ---- matrices as bytes -------------------------------------------------------

/// Element `index` of a buffer of doubles.
inline double get(const Bytes& bytes, std::size_t index) {
  double value = 0.0;
  std::memcpy(&value, &bytes.at(index * sizeof(double)), sizeof value);
  return value;
}

inline void put(Bytes& bytes, std::size_t index, double value) {
  std::memcpy(&bytes.at(index * sizeof(double)), &value, sizeof value);
}

/// a_ik of a unit's input, and b_kj.
inline double a_of(const Bytes& input, std::size_t i, std::size_t k) {
  return get(input, i * kN + k);
}
inline double b_of(const Bytes& input, std::size_t k, std::size_t j) {
  return get(input, kMatrix + k * kN + j);
}
inline double c_of(const Bytes& output, std::size_t i, std::size_t j) {
  return get(output, i * kN + j);
}

/// An input built from two functions of (row, column).
template <typename AFn, typename BFn>
Bytes input_of(AFn a, BFn b) {
  Bytes input(kInputBytes);
  for (std::size_t i = 0; i < kN; ++i) {
    for (std::size_t j = 0; j < kN; ++j) {
      put(input, i * kN + j, a(i, j));
      put(input, kMatrix + i * kN + j, b(i, j));
    }
  }
  return input;
}

/// The bits of a double, for messages and for flipping.
inline std::uint64_t bits_of(double value) { return std::bit_cast<std::uint64_t>(value); }

/// Flips bit `bit` of element `index`.
inline void flip(Bytes& bytes, std::size_t index, unsigned bit) {
  put(bytes, index, std::bit_cast<double>(bits_of(get(bytes, index)) ^ (std::uint64_t{1} << bit)));
}

inline std::uint32_t digest(const Bytes& bytes) { return core::crc32(bytes); }

// ---- the bound ---------------------------------------------------------------

/// The plain dot product for c_ij and its absolute sum, by the definition: one
/// loop in increasing k, each product rounded and then added. Not the kernel's
/// structure: no panels, no tiles, no registers.
struct Reference {
  double c = 0.0;
  double s = 0.0;
};

inline Reference reference(const Bytes& input, std::size_t i, std::size_t j) {
  Reference r;
  for (std::size_t k = 0; k < kN; ++k) {
    const double product = a_of(input, i, k) * b_of(input, k, j);
    r.c = r.c + product;
    const double magnitude = std::fabs(a_of(input, i, k)) * std::fabs(b_of(input, k, j));
    r.s = r.s + magnitude;
  }
  return r;
}

/// The derived tolerance for two n-term dot products whose computed absolute
/// sum is `s` (the header says how).
inline double tolerance(double s) {
  const double g = gamma(kN);
  return 2.0 * g * s / (1.0 - g) * (1.0 + 8.0 * kUnitRoundoff);
}

/// Whether two results for one entry are within the bound of each other. A
/// NaN or an infinity on either side never is.
inline bool within_bound(double lhs, double rhs, double s) {
  return std::isfinite(lhs) && std::isfinite(rhs) && std::fabs(lhs - rhs) <= tolerance(s);
}

// ---- executors: this module's reading of worker/executor.hpp ------------------

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

/// `threads` threads per run, each with a slot of its own, running the indices
/// a seeded shuffle deals it, in the dealt order. A different seed gives a
/// different thread, slot and order to each index -- deterministically, so a
/// test can say which mapping it ran rather than hoping the scheduler varied
/// it. Threads are joined before run() returns, which is what makes their
/// writes visible to the caller.
class Shuffled final : public worker::Executor {
 public:
  Shuffled(std::size_t threads, std::uint64_t seed) : threads_(threads), seed_(seed) {}

  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }

  void run(std::size_t count, Task task, void* context) noexcept override {
    const std::vector<std::size_t> order = permutation(count, seed_);
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

// ---- workloads and paths -----------------------------------------------------

/// Whether this CPU and kernel run AVX2 with FMA3 (always false off x86-64).
inline bool avx2_runs_here() {
#if defined(__x86_64__)
  return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
  return false;
#endif
}

/// The paths this build has kernels for and this machine can execute. On an
/// x86-64 machine without AVX2 the AVX2 kernel is built and cannot be run;
/// the tests that need it say so and fail rather than skip, because a path
/// that was never run has not been tested.
inline std::vector<simd::IsaPath> runnable_paths() {
  std::vector<simd::IsaPath> paths{simd::IsaPath::kScalar};
  const simd::IsaSet built = simd::kernel_paths(built_kernels());
  if (built.contains(simd::IsaPath::kAvx2) && avx2_runs_here()) {
    paths.push_back(simd::IsaPath::kAvx2);
  }
  if (built.contains(simd::IsaPath::kNeon)) {
    paths.push_back(simd::IsaPath::kNeon);
  }
  return paths;
}

/// Whether a path's kernel fuses its multiply-add.
inline bool fused(simd::IsaPath path) { return path != simd::IsaPath::kScalar; }

/// Room for `units` units.
inline std::uint64_t footprint_for(std::uint64_t units) {
  return units * (std::uint64_t{kInputBytes} + kOutputBytes);
}

inline WorkloadConfig config_for(simd::IsaPath path, std::uint32_t depth = 0,
                                 std::uint64_t seed = 1) {
  return WorkloadConfig{seed, footprint_for(2), depth, path};
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

}  // namespace loadforge::workloads::dense_linear::testing

#endif
