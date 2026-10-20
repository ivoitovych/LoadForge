// SPDX-License-Identifier: GPL-3.0-or-later
#include "primitives/atomic/atomic.hpp"

#include <algorithm>
#include <array>
#include <bit>

#include "primitives/rng/philox.hpp"

namespace loadforge::primitives::atomic {
namespace {

// --- contention ----------------------------------------------------------------

struct ContendContext {
  std::span<std::atomic<std::uint64_t>> storage;
  std::span<std::uint64_t> partials;
  std::uint64_t increments;
  std::uint64_t stride;
};

// The orders the argument in atomic.hpp allows are the ones named Order; the
// template keeps the weakest and the strongest spelled out where they are used.
template <std::memory_order Order>
void shared_unit(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  auto& run = *static_cast<ContendContext*>(context);
  std::atomic<std::uint64_t>& counter = run.storage[0];
  std::uint64_t tickets = 0;
  for (std::uint64_t step = 0; step < run.increments; ++step) {
    tickets += counter.fetch_add(1, Order);
  }
  run.partials[index] = tickets;
}

template <std::memory_order Order>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the signature is Executor::Task's.
void sharded_unit(void* context, std::size_t index, std::size_t slot) noexcept {
  auto& run = *static_cast<ContendContext*>(context);
  std::atomic<std::uint64_t>& counter = run.storage[slot * run.stride];
  for (std::uint64_t step = 0; step < run.increments; ++step) {
    counter.fetch_add(1, Order);
  }
  run.partials[index] = 0;
}

/// Indexed by mode * 2 + ordering.
constexpr std::array<worker::Executor::Task, 4> kContendTasks = {
    shared_unit<std::memory_order_relaxed>, shared_unit<std::memory_order_seq_cst>,
    sharded_unit<std::memory_order_relaxed>, sharded_unit<std::memory_order_seq_cst>};

// --- handoff -------------------------------------------------------------------

struct HandoffContext {
  std::uint64_t seed;
  std::span<Cell> cells;
  std::span<Seen> seen;
};

template <std::memory_order Store, std::memory_order Load>
void handoff_unit(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  auto& run = *static_cast<HandoffContext*>(context);
  run.cells[index].payload = payload_of(run.seed, index);
  run.cells[index].ready.store(1, Store);
  Seen seen;
  for (std::size_t other = 0; other < run.cells.size(); ++other) {
    if (other == index || run.cells[other].ready.load(Load) != 1) {
      continue;
    }
    record(seen, run.cells[other].payload, payload_of(run.seed, other));
  }
  run.seen[index] = seen;
}

/// Indexed by ordering.
constexpr std::array<worker::Executor::Task, 2> kHandoffTasks = {
    handoff_unit<std::memory_order_release, std::memory_order_acquire>,
    handoff_unit<std::memory_order_seq_cst, std::memory_order_seq_cst>};

bool line_is_valid(std::uint32_t line_bytes) noexcept {
  return line_bytes != 0 && line_bytes % kCounterBytes == 0 && line_bytes <= kMaxLineBytes;
}

}  // namespace

core::Result<std::uint64_t, ConfigError> storage_elements(std::uint32_t line_bytes,
                                                          std::size_t slots) noexcept {
  if (!line_is_valid(line_bytes)) {
    return ConfigError::kLineBytesInvalid;
  }
  return std::uint64_t{slots} * (line_bytes / kCounterBytes);
}

core::Result<std::span<std::atomic<std::uint64_t>>, ConfigError> aligned_storage(
    std::span<std::atomic<std::uint64_t>> storage, std::uint32_t line_bytes) noexcept {
  if (!line_is_valid(line_bytes)) {
    return ConfigError::kLineBytesInvalid;
  }
  const auto address = std::bit_cast<std::uintptr_t>(storage.data());
  const std::uintptr_t gap = (line_bytes - address % line_bytes) % line_bytes;
  const std::size_t skip = std::min<std::size_t>(gap / kCounterBytes, storage.size());
  return storage.subspan(skip);
}

std::uint64_t expected_ticket_sum(std::uint64_t total) noexcept {
  const std::uint64_t below = total - 1;
  return total % 2 == 0 ? (total / 2) * below : total * (below / 2);
}

core::Result<Outcome, ConfigError> contend(worker::Executor& executor, const Config& config,
                                           std::span<std::atomic<std::uint64_t>> storage,
                                           std::span<std::uint64_t> partials) noexcept {
  const std::size_t slots = executor.threads();
  if (!line_is_valid(config.line_bytes)) {
    return ConfigError::kLineBytesInvalid;
  }
  if (storage.size() < std::uint64_t{slots} * (config.line_bytes / kCounterBytes)) {
    return ConfigError::kStorageTooSmall;
  }
  if (std::bit_cast<std::uintptr_t>(storage.data()) % config.line_bytes != 0) {
    return ConfigError::kStorageMisaligned;
  }
  const std::uint64_t stride = config.line_bytes / kCounterBytes;
  for (std::size_t slot = 0; slot < slots; ++slot) {
    storage[slot * stride].store(0, std::memory_order_relaxed);
  }
  ContendContext context{storage, partials, config.increments_per_unit, stride};
  const std::size_t task =
      static_cast<std::size_t>(config.mode) * 2 + static_cast<std::size_t>(config.ordering);
  executor.run(partials.size(), kContendTasks.at(task), &context);
  Outcome outcome;
  for (std::size_t slot = 0; slot < slots; ++slot) {
    outcome.total += storage[slot * stride].load(std::memory_order_relaxed);
  }
  for (const std::uint64_t partial : partials) {
    outcome.ticket_sum += partial;
  }
  return outcome;
}

void record(Seen& seen, std::uint64_t payload, std::uint64_t expected) noexcept {
  ++seen.observed;
  seen.mismatches += static_cast<std::uint64_t>(payload != expected);
}

std::uint64_t payload_of(std::uint64_t seed, std::uint64_t index) noexcept {
  return rng::u64_of(rng::block_at(seed, index, 0)) | 1U;
}

HandoffOutcome handoff(worker::Executor& executor, Ordering ordering, std::uint64_t seed,
                       std::span<Cell> cells, std::span<Seen> seen) noexcept {
  for (Cell& cell : cells) {
    cell.ready.store(0, std::memory_order_relaxed);
    cell.payload = 0;
  }
  HandoffContext context{seed, cells, seen};
  executor.run(cells.size(), kHandoffTasks.at(static_cast<std::size_t>(ordering)), &context);
  HandoffOutcome outcome;
  for (const Seen& unit : seen) {
    outcome.observed += unit.observed;
    outcome.mismatches += unit.mismatches;
  }
  for (const Cell& cell : cells) {
    outcome.published += cell.ready.load(std::memory_order_relaxed);
  }
  return outcome;
}

// No short-circuit and no boolean arithmetic: the fields differ in some bit or none,
// so there is no branch for a test to miss.
[[nodiscard]] bool operator==(const Outcome& left, const Outcome& right) noexcept {
  return ((left.total ^ right.total) | (left.ticket_sum ^ right.ticket_sum)) == 0;
}

}  // namespace loadforge::primitives::atomic
