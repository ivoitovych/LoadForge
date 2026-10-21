// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/selftest.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "core/byte_size.hpp"
#include "core/duration.hpp"
#include "worker/executor.hpp"
#include "workloads/registry/registry.hpp"
#include "workloads/workload.hpp"

namespace loadforge::selftest {
namespace {

constexpr std::array<std::string_view, 16> kCorpus{
    "500ms", "30s", "20m",  "5h",     "0s", "  20m  ", "2562047788015h", "1h30m",
    "8GiB",  "70%", "1024", "512MiB", "0%", "8GB",     "-1GiB",          "",
};

/// Runs every task on the calling thread, in order: the digest is a function
/// of the scalar kernels and must not depend on any thread's schedule.
class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// The workloads whose scalar paths the digest covers, and the units of each
/// it folds in: the first and a distant one.
constexpr std::array<std::string_view, 2> kDigestedWorkloads{"compression", "dense_linear"};
constexpr std::array<std::uint64_t, 2> kDigestedUnits{0, 4097};
constexpr std::uint64_t kDigestSeed = 0x5E1F7E57ULL;
constexpr std::uint64_t kDigestFootprint = std::uint64_t{8} << 20U;

/// FNV-1a over bytes: the same function as fnv1a over the characters of a
/// string, for the bytes of an output.
std::uint64_t fold(const std::vector<std::byte>& bytes, std::uint64_t basis) noexcept {
  std::uint64_t hash = basis;
  for (const std::byte byte : bytes) {
    hash ^= std::to_integer<std::uint64_t>(byte);
    hash *= kFnvPrime;
  }
  return hash;
}

}  // namespace

// FNV-1a. Chosen because it is exactly specified, has no tuning parameters and
// no endianness dependence at this width, so any difference between two runs is
// a difference in the inputs rather than in the hash.
std::uint64_t fnv1a(std::string_view text, std::uint64_t basis) noexcept {
  std::uint64_t hash = basis;
  for (const char c : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint64_t core_digest() {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const std::string_view input : kCorpus) {
    hash = fnv1a(input, hash);

    // Explicit if/else rather than a ternary: the ternary form produced a
    // never-entered branch pair in the coverage data, and an unreachable branch
    // that nobody can explain is exactly what the 100% gate exists to surface.
    const auto duration = core::parse_duration(input);
    if (duration) {
      hash = fnv1a(core::to_string(duration.value()), hash);
    } else {
      hash = fnv1a(core::describe(duration.error()), hash);
    }

    const auto size = core::parse_byte_size(input);
    if (size) {
      hash = fnv1a(core::to_string(size.value()), hash);
    } else {
      hash = fnv1a(core::describe(size.error()), hash);
    }
  }
  return workloads_digest(hash);
}

std::uint64_t workloads_digest(std::uint64_t basis) {
  const workloads::BuiltinRegistry registry;
  SerialExecutor executor;
  std::uint64_t hash = basis;
  const workloads::WorkloadConfig config{kDigestSeed, kDigestFootprint, 0, simd::IsaPath::kScalar};
  for (const std::string_view name : kDigestedWorkloads) {
    // The scalar path exists for every workload and this configuration is one
    // every workload accepts (the registry's tests create each one), so a
    // refusal here is a defect and value() says so by throwing.
    const auto created = registry.create(name, config);
    const workloads::Workload& workload = *created.value();
    const workloads::Plan& plan = workload.plan();
    std::vector<std::byte> input(plan.input_bytes);
    std::vector<std::byte> output(plan.output_bytes);
    std::vector<std::byte> scratch(plan.scratch_bytes * executor.threads());
    hash = fnv1a(name, hash);
    for (const std::uint64_t index : kDigestedUnits) {
      workload.generate(index, input);
      workload.compute(index, input, output, scratch, executor);
      hash = fold(output, hash);
    }
  }
  return hash;
}

std::string core_digest_hex() {
  constexpr std::string_view kDigits = "0123456789abcdef";
  const std::uint64_t hash = core_digest();
  std::string out(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    out[15 - i] = kDigits[(hash >> (i * 4)) & 0xF];
  }
  return out;
}

}  // namespace loadforge::selftest
