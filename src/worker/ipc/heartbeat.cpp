// SPDX-License-Identifier: GPL-3.0-or-later
#include "worker/ipc/heartbeat.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::ipc {

std::string_view describe(RegionFault fault) noexcept {
  switch (fault) {
    case RegionFault::kSyscall:
      return "the kernel refused";
    case RegionFault::kMagic:
      return "the memory is not a heartbeat region";
    case RegionFault::kVersion:
      return "the region is of a version this build does not read";
    case RegionFault::kSlotCount:
      return "the region holds a different number of slots than expected";
    case RegionFault::kSlotBytes:
      return "the region's slots are a different size than this build's";
    case RegionFault::kTooManyWorkers:
      return "more workers than a region can hold";
    case RegionFault::kSlotOutOfRange:
      return "no such slot";
    case RegionFault::kTorn:
      return "the writer is stuck mid-update";
  }
  return "unrecognised region fault";
}

std::string describe(const RegionError& error) {
  if (error.fault == RegionFault::kSyscall) {
    return describe(error.cause);
  }
  std::string text{describe(error.fault)};
  if (!error.detail.empty()) {
    text += ": ";
    text += error.detail;
  }
  return text;
}

std::string_view describe(Liveness liveness) noexcept {
  switch (liveness) {
    case Liveness::kNeverBeat:
      return "never beat";
    case Liveness::kFresh:
      return "fresh";
    case Liveness::kStale:
      return "stale";
  }
  return "unrecognised liveness";
}

// Two nanosecond counts in a row: the check is right that they could be
// swapped, and (now, allowance) is the order the question is asked in.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Liveness liveness(const std::optional<Heartbeat>& beat, std::int64_t now_ns,
                  std::int64_t stale_after_ns) noexcept {
  if (!beat.has_value()) {
    return Liveness::kNeverBeat;
  }
  const std::int64_t age_ns = now_ns - beat->timestamp_ns;
  if (age_ns > stale_after_ns) {
    return Liveness::kStale;
  }
  return Liveness::kFresh;
}

bool progressed(const Heartbeat& earlier, const Heartbeat& later) noexcept {
  return later.progress > earlier.progress;
}

namespace {

// The on-memory format. Plain integers, so that the mapping's bytes are the
// objects (implicit-lifetime types) and every access goes through atomic_ref
// with the ordering the seqlock needs. Each struct is one cache line so that
// no two writers ever share one.
struct alignas(HeartbeatRegion::kHeaderBytes) Header {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t slot_count;
  std::uint32_t slot_bytes;
};
static_assert(sizeof(Header) == HeartbeatRegion::kHeaderBytes);

struct alignas(HeartbeatRegion::kSlotBytes) Slot {
  std::uint64_t sequence;  ///< Even: at rest. Odd: a write is in progress.
  std::int64_t timestamp_ns;
  std::uint64_t progress;
  std::uint32_t phase;
  std::uint32_t pid;
};
static_assert(sizeof(Slot) == HeartbeatRegion::kSlotBytes);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "a seqlock across processes needs lock-free 64-bit atomics");
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);

// The field offsets, pinned. The controller and its workers are the same binary,
// so they cannot disagree about this layout; the pins are so that changing it is
// a deliberate edit, made together with a new kRegionVersion, and never a side
// effect of reordering a struct.
static_assert(offsetof(Header, magic) == 0 && offsetof(Header, version) == 4 &&
              offsetof(Header, slot_count) == 8 && offsetof(Header, slot_bytes) == 12);
static_assert(offsetof(Slot, sequence) == 0 && offsetof(Slot, timestamp_ns) == 8 &&
              offsetof(Slot, progress) == 16 && offsetof(Slot, phase) == 24 &&
              offsetof(Slot, pid) == 28);

RegionError syscall_error(platform::SyscallError cause) {
  return RegionError{RegionFault::kSyscall, std::string{}, std::move(cause)};
}

RegionError layout_error(RegionFault fault, std::string detail) {
  return RegionError{fault, std::move(detail), platform::SyscallError{}};
}

Header* header_at(void* base) { return static_cast<Header*>(base); }

/// The slot array begins one header past the base. Computed once, as bytes,
/// and kept as a void* so the per-slot access is a plain static_cast from
/// void*, which is the cast the mapping's type-free bytes call for.
void* slots_base(void* base, std::size_t bytes) {
  const std::span<std::byte> region{static_cast<std::byte*>(base), bytes};
  return region.subspan(HeartbeatRegion::kHeaderBytes).data();
}

/// Every slot the region has, as a span, so that one is reached by subspan
/// and no raw pointer arithmetic is needed; the caller bounds the index.
std::span<Slot> slot_cells(void* slots, std::size_t count) {
  return {static_cast<Slot*>(slots), count};
}

std::string count_detail(std::string_view what, std::uint64_t found, std::uint64_t expected) {
  std::string text{what};
  text += " ";
  text += std::to_string(found);
  text += ", expected ";
  text += std::to_string(expected);
  return text;
}

}  // namespace

HeartbeatRegion::HeartbeatRegion(platform::Syscalls& syscalls, int fd, void* base,
                                 std::size_t workers) noexcept
    : syscalls_(&syscalls),
      fd_(fd),
      base_(base),
      slots_(slots_base(base, bytes_for(workers))),
      bytes_(bytes_for(workers)),
      workers_(workers) {}

HeartbeatRegion::~HeartbeatRegion() {
  if (syscalls_ != nullptr) {
    (void)syscalls_->unmap(base_, bytes_);
    (void)syscalls_->close(fd_);
  }
}

HeartbeatRegion::HeartbeatRegion(HeartbeatRegion&& other) noexcept
    : syscalls_(std::exchange(other.syscalls_, nullptr)),
      fd_(std::exchange(other.fd_, -1)),
      base_(std::exchange(other.base_, nullptr)),
      slots_(std::exchange(other.slots_, nullptr)),
      bytes_(std::exchange(other.bytes_, 0)),
      workers_(std::exchange(other.workers_, 0)) {}

HeartbeatRegion& HeartbeatRegion::operator=(HeartbeatRegion&& other) noexcept {
  if (this != &other) {
    if (syscalls_ != nullptr) {
      (void)syscalls_->unmap(base_, bytes_);
      (void)syscalls_->close(fd_);
    }
    syscalls_ = std::exchange(other.syscalls_, nullptr);
    fd_ = std::exchange(other.fd_, -1);
    base_ = std::exchange(other.base_, nullptr);
    slots_ = std::exchange(other.slots_, nullptr);
    bytes_ = std::exchange(other.bytes_, 0);
    workers_ = std::exchange(other.workers_, 0);
  }
  return *this;
}

core::Result<core::Ok, platform::SyscallError> HeartbeatRegion::release() {
  platform::Syscalls* const syscalls = std::exchange(syscalls_, nullptr);
  if (syscalls == nullptr) {
    return core::Ok{};
  }
  void* const base = std::exchange(base_, nullptr);
  slots_ = nullptr;
  const std::size_t bytes = std::exchange(bytes_, 0);
  const int fd = std::exchange(fd_, -1);
  // Both are attempted whatever the first says: a mapping that would not
  // unmap is no reason to keep the descriptor too. The first failure is the
  // one reported.
  auto unmapped = syscalls->unmap(base, bytes);
  auto closed = syscalls->close(fd);
  if (!unmapped.has_value()) {
    return unmapped.error();
  }
  return closed;
}

core::Result<HeartbeatRegion, RegionError> HeartbeatRegion::create(platform::Syscalls& syscalls,
                                                                   std::size_t workers,
                                                                   const std::string& name) {
  if (workers > kMaxWorkers) {
    return layout_error(RegionFault::kTooManyWorkers,
                        count_detail("workers", workers, kMaxWorkers));
  }
  const std::size_t bytes = bytes_for(workers);
  auto fd = syscalls.create_memory_file(name, bytes);
  if (!fd.has_value()) {
    return syscall_error(fd.error());
  }
  auto base = syscalls.map_shared(fd.value(), bytes);
  if (!base.has_value()) {
    (void)syscalls.close(fd.value());
    return syscall_error(base.error());
  }
  // A fresh memfd is zero-filled, so every slot already reads as "never
  // published" (sequence 0). Only the header needs writing. The release
  // fence orders it before anything a worker could observe after attach.
  Header* const header = header_at(base.value());
  header->magic = kRegionMagic;
  header->version = kRegionVersion;
  header->slot_count = static_cast<std::uint32_t>(workers + 1);
  header->slot_bytes = static_cast<std::uint32_t>(kSlotBytes);
  std::atomic_thread_fence(std::memory_order_release);
  return HeartbeatRegion(syscalls, fd.value(), base.value(), workers);
}

core::Result<HeartbeatRegion, RegionError> HeartbeatRegion::attach(platform::Syscalls& syscalls,
                                                                   int fd, std::size_t workers) {
  if (workers > kMaxWorkers) {
    return layout_error(RegionFault::kTooManyWorkers,
                        count_detail("workers", workers, kMaxWorkers));
  }
  const std::size_t bytes = bytes_for(workers);
  auto base = syscalls.map_shared(fd, bytes);
  if (!base.has_value()) {
    return syscall_error(base.error());
  }
  std::atomic_thread_fence(std::memory_order_acquire);
  const Header* const header = header_at(base.value());
  std::optional<RegionError> refusal;
  if (header->magic != kRegionMagic) {
    refusal = layout_error(RegionFault::kMagic, count_detail("magic", header->magic, kRegionMagic));
  } else if (header->version != kRegionVersion) {
    refusal = layout_error(RegionFault::kVersion,
                           count_detail("version", header->version, kRegionVersion));
  } else if (header->slot_count != workers + 1) {
    refusal = layout_error(RegionFault::kSlotCount,
                           count_detail("slots", header->slot_count, workers + 1));
  } else if (header->slot_bytes != kSlotBytes) {
    refusal = layout_error(RegionFault::kSlotBytes,
                           count_detail("slot bytes", header->slot_bytes, kSlotBytes));
  }
  if (refusal.has_value()) {
    // Unmapped, but the descriptor is left alone: a region owns a descriptor
    // only once it has attached, and this one was handed in by the caller.
    (void)syscalls.unmap(base.value(), bytes);
    return std::move(refusal).value();
  }
  return HeartbeatRegion(syscalls, fd, base.value(), workers);
}

core::Result<core::Ok, RegionError> HeartbeatRegion::publish(std::size_t slot,
                                                             const Heartbeat& beat) {
  if (slot >= slots()) {
    return layout_error(RegionFault::kSlotOutOfRange, count_detail("slot", slot, slots()));
  }
  Slot* const cell = slot_cells(slots_, slots()).subspan(slot, 1).data();
  const std::atomic_ref<std::uint64_t> sequence{cell->sequence};
  // Seqlock writer. The sequence goes odd before the fields change and even
  // after. The two transitions need different orderings, and a release store
  // gives only one of them.
  //
  // Going even must not be seen before the fields: a release store orders
  // every earlier write before it, which is exactly that.
  //
  // Going odd must not be seen AFTER the fields, and a release store does not
  // promise that: it orders what came before it, not what follows, so the
  // field stores below may become visible first. On x86 they never do, because
  // stores are not reordered with each other; on arm64 they may, and a reader
  // would then read new fields between two loads of the old even value and
  // accept a sample that is half one beat and half the next. The release
  // fence after the odd store is what forbids it: if a reader's field load
  // sees a store made after this fence, the reader's acquire fence
  // synchronises with this one, and its second sequence load sees at least
  // the odd value, so the sample is retried.
  //
  // Both stores derive from the odd value, so the even one a publish leaves
  // is always the next after the odd one it wrote, and never the value it
  // found: a reader that saw the slot at rest before the publish cannot see
  // the same number after it.
  const std::uint64_t writing = sequence.load(std::memory_order_relaxed) + 1;
  sequence.store(writing, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  std::atomic_ref<std::int64_t>{cell->timestamp_ns}.store(beat.timestamp_ns,
                                                          std::memory_order_relaxed);
  std::atomic_ref<std::uint64_t>{cell->progress}.store(beat.progress, std::memory_order_relaxed);
  std::atomic_ref<std::uint32_t>{cell->phase}.store(beat.phase, std::memory_order_relaxed);
  std::atomic_ref<std::uint32_t>{cell->pid}.store(beat.pid, std::memory_order_relaxed);
  sequence.store(writing + 1, std::memory_order_release);
  return core::Ok{};
}

core::Result<std::optional<Heartbeat>, RegionError> HeartbeatRegion::observe(
    std::size_t slot) const {
  if (slot >= slots()) {
    return layout_error(RegionFault::kSlotOutOfRange, count_detail("slot", slot, slots()));
  }
  Slot* const cell = slot_cells(slots_, slots()).subspan(slot, 1).data();
  const std::atomic_ref<std::uint64_t> sequence{cell->sequence};
  int attempts = 0;
  for (; attempts < kMaxTornRetries; ++attempts) {
    // Seqlock reader. Acquire the sequence, read the fields, then re-read the
    // sequence; the fence keeps the field reads from being reordered after
    // the second sequence read. Odd means a write is in progress; a changed
    // value means one happened underneath us. Either way: try again, a
    // bounded number of times.
    const std::uint64_t before = sequence.load(std::memory_order_acquire);
    if (before == 0) {
      return std::optional<Heartbeat>{};
    }
    Heartbeat beat;
    beat.timestamp_ns =
        std::atomic_ref<std::int64_t>{cell->timestamp_ns}.load(std::memory_order_relaxed);
    beat.progress = std::atomic_ref<std::uint64_t>{cell->progress}.load(std::memory_order_relaxed);
    beat.phase = std::atomic_ref<std::uint32_t>{cell->phase}.load(std::memory_order_relaxed);
    beat.pid = std::atomic_ref<std::uint32_t>{cell->pid}.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t after = sequence.load(std::memory_order_relaxed);
    if (sample_is_consistent(before, after)) {
      return std::optional<Heartbeat>{beat};
    }
  }
  // Not count_detail: there is no expected count here, only attempts that all
  // found a write in progress, and "expected 0" read as if retries were a
  // budget overrun rather than a slot never seen at rest. The number is the
  // attempts actually made, counted, not the bound they were meant to meet.
  std::string detail = "no consistent sample in ";
  detail += std::to_string(attempts);
  detail += " attempts";
  return layout_error(RegionFault::kTorn, std::move(detail));
}

bool sample_is_consistent(std::uint64_t before, std::uint64_t after) noexcept {
  return (before & 1U) == 0 && before == after;
}

}  // namespace loadforge::ipc
