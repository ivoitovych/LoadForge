// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_GOLDEN_MOCK_WORKLOAD_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_GOLDEN_MOCK_WORKLOAD_HPP

// This module's own reading of workloads/workload.hpp (ADR 0003): a small
// BitExact workload to record golden vectors from, and executors to run it on.
// It is checked against WorkloadContract in workload_contract_test.cpp, so a
// reading of the contract that differs from the suite's fails there, not in a
// golden test that would then be testing the mock.
//
// Unit i's input is 1024 bytes of Philox stream i under the seed. Its output
// is 16 bytes: the sum modulo 2^64 and the XOR of the input's 128
// little-endian words, each over `decomposition` chunks reduced in chunk
// order -- integer arithmetic, so the bits are the same at any thread count,
// in any order, on any machine. Flavor::kChanged rotates the sum by one bit:
// the "intended change" a golden vector exists to notice.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/result.hpp"
#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/record.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::golden_test {

enum class Flavor : std::uint8_t { kOriginal, kChanged };

class MockWorkload final : public workloads::Workload {
 public:
  static constexpr std::uint64_t kInputBytes = 1024;
  static constexpr std::uint64_t kOutputBytes = 16;
  static constexpr std::uint32_t kDefaultDecomposition = 8;
  static constexpr std::uint32_t kMaxDecomposition = 64;

  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config, std::uint32_t version = 1,
      Flavor flavor = Flavor::kOriginal) {
    if (config.isa != simd::IsaPath::kScalar) {
      return workloads::WorkloadError{workloads::WorkloadFault::kConfiguration,
                                      "no kernel for " + std::string(simd::describe(config.isa))};
    }
    const std::uint32_t decomposition =
        config.decomposition == 0 ? kDefaultDecomposition : config.decomposition;
    if (decomposition > kMaxDecomposition) {
      return workloads::WorkloadError{workloads::WorkloadFault::kConfiguration,
                                      "decomposition " + std::to_string(decomposition) + " above " +
                                          std::to_string(kMaxDecomposition)};
    }
    const std::uint64_t units = config.footprint_bytes / (kInputBytes + kOutputBytes);
    if (units == 0) {
      return workloads::WorkloadError{
          workloads::WorkloadFault::kFootprintTooSmall,
          std::to_string(config.footprint_bytes) + " bytes hold no unit"};
    }
    const workloads::Plan plan{kInputBytes,    kOutputBytes,  0,
                               units,          decomposition, kInputBytes + kOutputBytes,
                               kInputBytes / 8};
    return std::unique_ptr<workloads::Workload>(
        new MockWorkload(config.seed, version, flavor, plan));
  }

  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    rng::fill_stream(seed_, index, input);
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> /*scratch*/,
               worker::Executor& executor) const noexcept override {
    const std::size_t words = input.size() / 8;
    const std::size_t chunks = plan_.decomposition;
    std::array<std::uint64_t, kMaxDecomposition> sums{};
    std::array<std::uint64_t, kMaxDecomposition> xors{};
    auto chunk_body = [&](std::size_t chunk, std::size_t /*slot*/) noexcept {
      for (std::size_t word = chunk * words / chunks; word < (chunk + 1) * words / chunks; ++word) {
        std::uint64_t value = 0;
        for (std::size_t byte = 0; byte < 8; ++byte) {
          value |= std::to_integer<std::uint64_t>(input[word * 8 + byte]) << (8 * byte);
        }
        sums.at(chunk) += value;
        xors.at(chunk) ^= value;
      }
    };
    worker::for_each_index(executor, chunks, chunk_body);
    std::uint64_t sum = 0;
    std::uint64_t mixed = 0;
    for (std::size_t chunk = 0; chunk < chunks; ++chunk) {
      sum += sums.at(chunk);
      mixed ^= xors.at(chunk);
    }
    if (flavor_ == Flavor::kChanged) {
      sum = (sum << 1U) | (sum >> 63U);
    }
    for (std::size_t byte = 0; byte < 8; ++byte) {
      output[byte] = static_cast<std::byte>(sum >> (8 * byte));
      output[8 + byte] = static_cast<std::byte>(mixed >> (8 * byte));
    }
  }

 private:
  MockWorkload(std::uint64_t seed, std::uint32_t version, Flavor flavor,
               const workloads::Plan& plan)
      : descriptor_{"golden_mock", version, workloads::DeterminismClass::kBitExact,
                    Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_(plan),
        seed_(seed),
        flavor_(flavor) {}

  workloads::Descriptor descriptor_;
  workloads::Plan plan_;
  std::uint64_t seed_;
  Flavor flavor_;
};

/// Runs every index on the calling thread, in order.
class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// Runs the indices on `threads` real threads, slot s taking every index
/// congruent to s, last first: a different thread and a different order from
/// the serial executor for every index but the trivial cases.
class ThreadedExecutor final : public worker::Executor {
 public:
  explicit ThreadedExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([this, slot, count, task, context] {
        for (std::size_t index = count; index-- > 0;) {
          if (index % threads_ == slot) {
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
};

/// The configuration the committed fixture was recorded with.
inline workloads::WorkloadConfig fixture_config() {
  return workloads::WorkloadConfig{0x601DE11ULL,
                                   4 * (MockWorkload::kInputBytes + MockWorkload::kOutputBytes), 0,
                                   simd::IsaPath::kScalar};
}

inline constexpr std::uint64_t kFixtureUnits = 6;

}  // namespace loadforge::verification::golden_test

#endif
