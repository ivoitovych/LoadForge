// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_ATOMIC_ATOMIC_HPP
#define LOADFORGE_PRIMITIVES_ATOMIC_ATOMIC_HPP

// The atomic calibration loads (PLAN §6, layer 1; PLAN §4.5): contention on
// purpose. Two loads, each with the memory orders it uses written out and
// argued, and each runnable at seq_cst for comparison.
//
// 1. CONTENTION (contend). Every unit performs `increments_per_unit`
//    fetch_adds on a counter.
//      kShared   one counter for all slots: every increment is a coherence
//                transfer of one cache line. Contention by design.
//      kSharded  one counter per slot, `line_bytes` apart: the same number of
//                atomic operations with no line shared between slots, which is
//                what separates the cost of atomics from the cost of sharing.
//    The line size is an argument -- the caller reads it from the topology
//    (PLAN §4.5, topology::Cache::line_bytes); nothing here says 64.
//
//    ORDERING ARGUMENT. Ordering::kWeakest uses memory_order_relaxed for the
//    increments and for the reads that follow the run. Why that is enough:
//      (a) Atomicity does not depend on ordering. Every fetch_add is a
//          read-modify-write of one location, and the RMWs on one location are
//          totally ordered by its modification order, each reading the value
//          written by its predecessor in that order. So no increment is lost
//          and no two get the same ticket, whatever the order (C++20
//          [atomics.order]). That is the whole invariant of this load.
//      (b) Nothing else is published through the counter. The only other data
//          the units write is their own partials[index], read after run()
//          returns; the Executor contract makes every write in a task
//          happen-before run's return, so the caller's read of partials, and
//          our relaxed load of the counters after the run, need no ordering of
//          their own.
//      (c) The counters are zeroed before run() by plain relaxed stores. They
//          are visible to the tasks because the executor must publish the task
//          and its context to its threads, and whatever publishes the context
//          publishes what was written before it (see "Contract questions" in
//          docs/journal/20-primitives.md).
//    Tested by: the exact total and the ticket sum at every thread count, at
//    both orderings, under ThreadSanitizer in CI.
//
// 2. HANDOFF (handoff). Unit i writes a plain (non-atomic) payload, then
//    publishes it with a store to its ready flag; then looks at every other
//    unit's flag and, for each it finds set, reads that unit's payload and
//    checks it. Nothing waits: a flag not yet set is skipped, so a serial
//    executor, which runs units one at a time, cannot hang it.
//
//    ORDERING ARGUMENT. Ordering::kWeakest stores the flag with
//    memory_order_release and loads it with memory_order_acquire.
//      * The release store is sequenced after the payload write, and the
//        acquire load that reads the value 1 reads from that store, so the
//        store synchronizes-with the load ([atomics.order]). The payload write
//        therefore happens-before the payload read, which is why the plain
//        read is not a data race and sees the value written.
//      * No stronger order is needed: nothing here depends on units agreeing
//        on a single order of the flags (no store-buffering or IRIW pattern),
//        so the total order seq_cst adds is not used.
//      * Weaken the store or the load to relaxed and the read of the payload
//        is a data race (undefined behaviour) which ThreadSanitizer reports,
//        and on a weakly ordered machine such as AArch64 may read a stale
//        payload. That is what the test is aimed at; PLAN §4.5 and
//        IMPLEMENTATION §2.4 are honest that a test cannot prove the absence
//        of a legal-but-rare reordering, so CI's sanitizer and ARM64 jobs are
//        corroboration, not proof.
//    Ordering::kSeqCst stores and loads at seq_cst: the baseline the argument
//    is measured against, and the project's default for anything not argued.
//
// DETERMINISM: BitExact for what is deterministic, and it is stated which is
// which. Counter totals and the sum of tickets are order-independent
// (wrapping addition), so they are identical at every thread count and on every
// architecture. Which unit received which ticket is NOT deterministic and is
// not part of any result: the per-unit ticket sums vary from run to run and are
// exposed only as the executor's scratch-like `partials`. Likewise handoff's
// `observed` count depends on timing; its `mismatches` (zero) and `published`
// (all of them) do not.
// THREAD-SAFETY: run-style calls follow the Executor's one-caller-at-a-time rule.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/result.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::primitives::atomic {

inline constexpr workloads::DeterminismClass kDeterminism =
    workloads::DeterminismClass::kBitExact;

/// Largest line size accepted; real lines are 32 to 256 bytes.
inline constexpr std::uint32_t kMaxLineBytes = 4096;
inline constexpr std::uint32_t kCounterBytes = 8;

enum class Ordering : std::uint8_t {
  kWeakest,  ///< the orders argued above: relaxed counters, release/acquire handoff
  kSeqCst,   ///< sequentially consistent throughout, the comparison baseline
};

enum class Mode : std::uint8_t { kShared, kSharded };

struct Config {
  Mode mode = Mode::kShared;
  Ordering ordering = Ordering::kWeakest;
  std::uint64_t increments_per_unit = 0;
  /// The machine's cache line size, from the topology. A multiple of eight.
  std::uint32_t line_bytes = 0;
};

enum class ConfigError : std::uint8_t {
  kLineBytesInvalid,   ///< zero, not a multiple of eight, or above kMaxLineBytes
  kStorageTooSmall,    ///< fewer counters than one line per executor thread
  kStorageMisaligned,  ///< the first counter is not on a line boundary
};

/// Counters (64-bit atomics) of storage needed for `slots` slots: one line each.
[[nodiscard]] core::Result<std::uint64_t, ConfigError> storage_elements(
    std::uint32_t line_bytes, std::size_t slots) noexcept;

/// The part of `storage` that starts on a `line_bytes` boundary: the leading
/// counters before the first boundary are dropped. Allocate
/// storage_elements() plus line_bytes / 8 - 1 counters to be sure of enough.
[[nodiscard]] core::Result<std::span<std::atomic<std::uint64_t>>, ConfigError> aligned_storage(
    std::span<std::atomic<std::uint64_t>> storage, std::uint32_t line_bytes) noexcept;

struct Outcome {
  /// The counters' sum after the run: units * increments_per_unit.
  std::uint64_t total = 0;
  /// The wrapping sum of every ticket handed out (kShared), which for tickets
  /// 0 .. total-1 is total * (total - 1) / 2; zero for kSharded.
  std::uint64_t ticket_sum = 0;

  /// Defined in atomic.cpp, not defaulted here: one definition, so coverage
  /// is not split across translation units (JOURNAL §1.22).
  [[nodiscard]] bool operator==(const Outcome& other) const noexcept;
};

/// The ticket sum a correct kShared run of `total` increments gives, modulo
/// 2^64: 0 + 1 + ... + (total - 1).
[[nodiscard]] std::uint64_t expected_ticket_sum(std::uint64_t total) noexcept;

/// Runs partials.size() units of the contention load. `storage` must be
/// line-aligned and hold storage_elements(line_bytes, executor.threads())
/// counters; partials[i] receives the sum of the tickets unit i drew.
[[nodiscard]] core::Result<Outcome, ConfigError> contend(
    worker::Executor& executor, const Config& config,
    std::span<std::atomic<std::uint64_t>> storage, std::span<std::uint64_t> partials) noexcept;

/// One unit's publication slot. The payload is deliberately NOT atomic: it is
/// the data the flag's ordering protects.
struct Cell {
  std::atomic<std::uint32_t> ready{0};
  std::uint64_t payload = 0;
};

struct Seen {
  std::uint64_t observed = 0;    ///< other units' flags found set
  std::uint64_t mismatches = 0;  ///< of those, payloads that were not the published value
};

struct HandoffOutcome {
  std::uint64_t observed = 0;    ///< timing-dependent; not a result
  std::uint64_t mismatches = 0;  ///< must be zero
  std::uint64_t published = 0;   ///< flags set after the run: must be cells.size()
};

/// The reader's half, as its own function so a stale read can be shown to be
/// counted: notes one observed flag and, if `payload` is not the `expected`
/// value, one mismatch. Branch-free on purpose.
void record(Seen& seen, std::uint64_t payload, std::uint64_t expected) noexcept;

/// The payload unit `index` publishes: a pure function of (seed, index), and
/// never zero, so an unwritten cell cannot pass for a written one.
[[nodiscard]] std::uint64_t payload_of(std::uint64_t seed, std::uint64_t index) noexcept;

/// Runs cells.size() units of the handoff; seen[i] is what unit i observed
/// (seen.size() must be cells.size()).
[[nodiscard]] HandoffOutcome handoff(worker::Executor& executor, Ordering ordering,
                                     std::uint64_t seed, std::span<Cell> cells,
                                     std::span<Seen> seen) noexcept;

}  // namespace loadforge::primitives::atomic

#endif
