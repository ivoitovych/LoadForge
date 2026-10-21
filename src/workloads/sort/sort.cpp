// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/sort/sort.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "core/result.hpp"
#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"
#include "workloads/sort/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads::sort {
namespace {

// Eight passes of one byte each, least significant first. Every pass is a
// stable counting sort, so after the last one the keys are in order and equal
// keys are in the order the first pass saw them: input position. The passes
// alternate between the scratch buffer and the output, and an even count ends
// in the output.
constexpr unsigned kDigitBits = 8;
constexpr std::size_t kRadix = std::size_t{1} << kDigitBits;
constexpr unsigned kPasses = 64 / kDigitBits;
static_assert(kPasses % 2 == 0, "the last pass must write the output, not the scratch");

constexpr std::size_t kCounterBytes = 4;

// What a unit needs besides the output: a second copy of the keys and of the
// indices, and one histogram of kRadix counters per range.
constexpr std::size_t kSideKeysBytes = kKeys * kKeyBytes;
constexpr std::size_t kSideIndicesBytes = kKeys * kIndexBytes;
constexpr std::size_t kSideBytes = kSideKeysBytes + kSideIndicesBytes;

[[nodiscard]] constexpr std::size_t counters_bytes(std::size_t chunks) noexcept {
  return chunks * kRadix * kCounterBytes;
}

constexpr std::uint64_t kUnitBytes = kInputBytes + kOutputBytes;

// Memory traffic of one pass per key: the histogram reads the key (8); the
// scatter reads the key and, after the first pass, its index (12), and writes
// both (12).
constexpr std::uint64_t kFirstPassBytes = kKeyBytes + kKeyBytes + (kKeyBytes + kIndexBytes);
constexpr std::uint64_t kLaterPassBytes =
    kKeyBytes + (kKeyBytes + kIndexBytes) + (kKeyBytes + kIndexBytes);
constexpr std::uint64_t kLogicalBytes = kKeys * (kFirstPassBytes + (kPasses - 1) * kLaterPassBytes);
// Two digit operations per key per pass: a count and a move.
constexpr std::uint64_t kOperations = std::uint64_t{kKeys} * kPasses * 2;

void store_le64(std::span<std::byte> bytes, std::size_t at, std::uint64_t value) noexcept {
  for (std::size_t byte = 0; byte < kKeyBytes; ++byte) {
    bytes[at + byte] = static_cast<std::byte>((value >> (8U * byte)) & 0xFFU);
  }
}

void store_le32(std::span<std::byte> bytes, std::size_t at, std::uint32_t value) noexcept {
  for (std::size_t byte = 0; byte < kIndexBytes; ++byte) {
    bytes[at + byte] = static_cast<std::byte>((value >> (8U * byte)) & 0xFFU);
  }
}

/// One pass of the sort and the buffers it moves between. `src_indices` is
/// empty in the first pass, whose indices are the positions themselves.
struct Pass {
  std::span<const std::byte> src_keys;
  std::span<const std::byte> src_indices;
  std::span<std::byte> dst_keys;
  std::span<std::byte> dst_indices;
  std::span<std::byte> counters;
  std::size_t range_keys = 0;
  unsigned shift = 0;
};

[[nodiscard]] std::size_t digit_of(std::uint64_t key, unsigned shift) noexcept {
  return (key >> shift) & (kRadix - 1);
}

/// Range `range` counts the digits of its keys.
void count_range(void* context, std::size_t range, std::size_t /*slot*/) noexcept {
  const auto& pass = *static_cast<const Pass*>(context);
  std::array<std::uint32_t, kRadix> counts{};
  const std::size_t first = range * pass.range_keys;
  for (std::size_t at = first; at < first + pass.range_keys; ++at) {
    ++counts.at(digit_of(load_le64(pass.src_keys, at * kKeyBytes), pass.shift));
  }
  for (std::size_t digit = 0; digit < kRadix; ++digit) {
    store_le32(pass.counters, (range * kRadix + digit) * kCounterBytes, counts.at(digit));
  }
}

/// Turns the counts into where each range starts writing each digit: all of
/// digit 0 first, in range order, then digit 1, and so on. That order is what
/// makes the pass stable, and it depends on the ranges and not on the threads.
void place_ranges(const Pass& pass, std::size_t chunks) noexcept {
  std::uint32_t next = 0;
  for (std::size_t digit = 0; digit < kRadix; ++digit) {
    for (std::size_t range = 0; range < chunks; ++range) {
      const std::size_t at = (range * kRadix + digit) * kCounterBytes;
      const std::uint32_t count = load_le32(pass.counters, at);
      store_le32(pass.counters, at, next);
      next += count;
    }
  }
}

/// Range `range` moves its keys, and their indices, to their places.
void move_range(void* context, std::size_t range, std::size_t /*slot*/) noexcept {
  const auto& pass = *static_cast<const Pass*>(context);
  std::array<std::uint32_t, kRadix> places{};
  for (std::size_t digit = 0; digit < kRadix; ++digit) {
    places.at(digit) = load_le32(pass.counters, (range * kRadix + digit) * kCounterBytes);
  }
  const std::size_t first = range * pass.range_keys;
  const bool first_pass = pass.src_indices.empty();
  for (std::size_t at = first; at < first + pass.range_keys; ++at) {
    const std::uint64_t key = load_le64(pass.src_keys, at * kKeyBytes);
    const std::uint32_t index =
        first_pass ? static_cast<std::uint32_t>(at) : load_le32(pass.src_indices, at * kIndexBytes);
    const std::uint32_t place = places.at(digit_of(key, pass.shift))++;
    store_le64(pass.dst_keys, place * kKeyBytes, key);
    store_le32(pass.dst_indices, place * kIndexBytes, index);
  }
}

[[nodiscard]] std::string refusal_of_decomposition(std::uint32_t decomposition) noexcept {
  std::string detail = "decomposition ";
  detail += std::to_string(decomposition);
  detail += " is neither 0 (the default) nor one of 1, 2, 4, 8, 16, 32, 64";
  return detail;
}

[[nodiscard]] std::string refusal_of_path(simd::IsaPath path) noexcept {
  std::string detail = "the sort has no kernel for the ";
  detail += simd::describe(path);
  detail += " path; it has the scalar one only";
  return detail;
}

[[nodiscard]] std::string refusal_of_footprint(std::uint64_t footprint) noexcept {
  std::string detail = "a footprint of ";
  detail += std::to_string(footprint);
  detail += " bytes is smaller than one unit of ";
  detail += std::to_string(kUnitBytes);
  detail += " bytes";
  return detail;
}

/// What a configuration resolves to.
struct Resolved {
  std::uint64_t seed = 0;
  std::uint32_t chunks = 0;
  std::uint64_t units = 0;
};

class SortWorkload final : public Workload {
 public:
  explicit SortWorkload(const Resolved& resolved) noexcept
      : seed_(resolved.seed),
        descriptor_(sort::workload_descriptor()),
        plan_{kInputBytes,    kOutputBytes,    kSideBytes + counters_bytes(resolved.chunks),
              resolved.units, resolved.chunks, kLogicalBytes,
              kOperations} {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::size_t position = 0; position < kKeys; ++position) {
      store_le64(input, position * kKeyBytes, key_at(seed_, index, position));
    }
  }

  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> scratch,
               worker::Executor& executor) const noexcept override {
    const std::size_t chunks = plan_.decomposition;
    const std::span<std::byte> side_keys = scratch.first(kSideKeysBytes);
    const std::span<std::byte> side_indices = scratch.subspan(kSideKeysBytes, kSideIndicesBytes);
    const std::span<std::byte> counters = scratch.subspan(kSideBytes, counters_bytes(chunks));
    const std::span<std::byte> out_keys = output.first(kIndexRegionOffset);
    const std::span<std::byte> out_indices = output.subspan(kIndexRegionOffset);

    for (unsigned number = 0; number < kPasses; ++number) {
      const bool to_output = number % 2 == 1;
      Pass pass;
      if (number == 0) {
        pass.src_keys = input;
      } else if (to_output) {
        pass.src_keys = side_keys;
        pass.src_indices = side_indices;
      } else {
        pass.src_keys = out_keys;
        pass.src_indices = out_indices;
      }
      pass.dst_keys = to_output ? out_keys : side_keys;
      pass.dst_indices = to_output ? out_indices : side_indices;
      pass.counters = counters;
      pass.range_keys = kKeys / chunks;
      pass.shift = number * kDigitBits;
      executor.run(chunks, count_range, &pass);
      place_ranges(pass, chunks);
      executor.run(chunks, move_range, &pass);
    }
  }

 private:
  std::uint64_t seed_;
  Descriptor descriptor_;
  Plan plan_;
};

}  // namespace

Descriptor workload_descriptor() {
  return Descriptor{std::string(kName), kVersion, DeterminismClass::kBitExact,
                    verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}};
}

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const std::uint32_t chunks =
      config.decomposition == 0 ? kDefaultChunkCount : config.decomposition;
  if (!is_chunk_count(chunks)) {
    return WorkloadError{WorkloadFault::kConfiguration, refusal_of_decomposition(chunks)};
  }
  if (config.isa != simd::IsaPath::kScalar) {
    return WorkloadError{WorkloadFault::kConfiguration, refusal_of_path(config.isa)};
  }
  const std::uint64_t units = config.footprint_bytes / kUnitBytes;
  if (units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         refusal_of_footprint(config.footprint_bytes)};
  }
  return std::unique_ptr<Workload>(
      std::make_unique<SortWorkload>(Resolved{config.seed, chunks, units}));
}

}  // namespace loadforge::workloads::sort
