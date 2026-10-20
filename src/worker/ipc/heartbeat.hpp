// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_IPC_HEARTBEAT_HPP
#define LOADFORGE_WORKER_IPC_HEARTBEAT_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::ipc {

/// One beat: when, how far, in what phase, and from whom.
///
/// `timestamp_ns` is CLOCK_MONOTONIC ("running" in platform::Clock), because
/// it is the one clock every process on the machine shares the same reading
/// of and that does not jump. `progress` is a counter the worker advances
/// whenever it has done work; it is how "working slowly" is told from "not
/// progressing" -- a beat with the same progress as the last one is alive and
/// stuck, which is a different fact from alive and busy.
struct Heartbeat {
  std::int64_t timestamp_ns = 0;
  std::uint64_t progress = 0;
  std::uint32_t phase = 0;
  std::uint32_t pid = 0;

  [[nodiscard]] friend bool operator==(const Heartbeat&, const Heartbeat&) = default;
};

/// What a region refused to do, and why.
///
///   kSyscall         the kernel said no; `cause` says what.
///   kMagic ..        the memory is not a heartbeat region of this version:
///   kSlotBytes       the header disagrees. Control state crossing the process
///                    boundary is re-validated, never trusted (F18).
///   kTooManyWorkers  more slots than the region format allows.
///   kSlotOutOfRange  a slot index the region does not have.
///   kTorn            a reader could not get a consistent sample within the
///                    retry bound: the writer is mid-update and not finishing,
///                    which is what a worker that died inside publish() looks
///                    like. Reported, never spun on.
enum class RegionFault : std::uint8_t {
  kSyscall,
  kMagic,
  kVersion,
  kSlotCount,
  kSlotBytes,
  kTooManyWorkers,
  kSlotOutOfRange,
  kTorn,
};

[[nodiscard]] std::string_view describe(RegionFault fault) noexcept;

struct RegionError {
  RegionFault fault = RegionFault::kSyscall;
  std::string detail;            ///< For a layout fault: what was found.
  platform::SyscallError cause;  ///< Valid when fault == kSyscall.

  [[nodiscard]] friend bool operator==(const RegionError&, const RegionError&) = default;
};

[[nodiscard]] std::string describe(const RegionError& error);

/// Format constants. Named so the tests can corrupt a header on purpose.
inline constexpr std::uint32_t kRegionMagic = 0x4C464842;  // "LFHB"
inline constexpr std::uint32_t kRegionVersion = 1;

/// The most workers a region holds. One per CPU is the natural ceiling and an
/// affinity mask addresses 1024 of them; a region for more would be a request
/// nothing can use.
inline constexpr std::size_t kMaxWorkers = 1024;

/// Slot 0 is the controller's own beat, which the workers watch: a controller
/// that is alive but stuck is detectable only this way (docs/PLAN.md F9).
inline constexpr std::size_t kControllerSlot = 0;
[[nodiscard]] constexpr std::size_t worker_slot(std::size_t worker) noexcept { return worker + 1; }

/// A shared-memory region of heartbeat slots, one writer per slot, any number
/// of readers, and no lock anywhere.
///
/// WHY NO LOCK
/// -----------
/// docs/PLAN.md §4.6: a worker can die holding a process-shared lock, and
/// unless every such lock is robust and every acquirer handles EOWNERDEAD, one
/// dead worker deadlocks every survivor. The plan's preference is a design with
/// no shared lock at all, and a single-writer slot with a sequence counter is
/// that design. Each slot is a seqlock: the writer bumps the sequence to odd,
/// writes the fields, bumps it to even; a reader takes the sequence, reads the
/// fields, and accepts the sample only if the sequence is unchanged and even.
/// A writer that dies mid-update leaves the sequence odd, and every reader
/// reports kTorn after a bounded number of retries rather than waiting --
/// which is the EOWNERDEAD rule with nothing to recover, because no reader
/// ever needed the writer to finish.
///
/// The synchronisation argument, in full, is in the tests (P5), next to the
/// test of its consequence. It is checked by reading it: TSan finds races, not
/// a missing fence, and x86 hides the one this design needs.
///
/// HOW IT CROSSES EXEC
/// -------------------
/// The region is a memfd. The controller creates and maps it; each worker
/// inherits the descriptor (WorkerLauncher::become_worker clears close-on-exec
/// for exactly that descriptor) and maps the same file after exec. The header
/// is validated on attach because a worker that mapped the wrong descriptor
/// would otherwise read somebody else's bytes as heartbeats.
class HeartbeatRegion {
 public:
  static constexpr std::size_t kHeaderBytes = 64;
  static constexpr std::size_t kSlotBytes = 64;  ///< A cache line each: writers never share one.
  static constexpr int kMaxTornRetries = 64;

  /// The region size for this many workers: the header, the controller slot,
  /// and one slot per worker.
  [[nodiscard]] static constexpr std::size_t bytes_for(std::size_t workers) noexcept {
    return kHeaderBytes + (workers + 1) * kSlotBytes;
  }

  /// Creates, sizes, maps and initialises a region. The controller's side.
  [[nodiscard]] static core::Result<HeartbeatRegion, RegionError> create(
      platform::Syscalls& syscalls, std::size_t workers,
      const std::string& name = "loadforge-heartbeat");

  /// Maps an inherited region and checks it is what it claims to be. The
  /// worker's side. `workers` is what the worker was told to expect; the
  /// header must agree.
  [[nodiscard]] static core::Result<HeartbeatRegion, RegionError> attach(
      platform::Syscalls& syscalls, int fd, std::size_t workers);

  ~HeartbeatRegion();
  HeartbeatRegion(HeartbeatRegion&& other) noexcept;
  HeartbeatRegion& operator=(HeartbeatRegion&& other) noexcept;
  HeartbeatRegion(const HeartbeatRegion&) = delete;
  HeartbeatRegion& operator=(const HeartbeatRegion&) = delete;

  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t workers() const noexcept { return workers_; }
  [[nodiscard]] std::size_t slots() const noexcept { return workers_ + 1; }

  /// Writes a beat into `slot`. One writer per slot, by construction: the
  /// controller writes slot 0 and worker i writes worker_slot(i).
  [[nodiscard]] core::Result<core::Ok, RegionError> publish(std::size_t slot,
                                                            const Heartbeat& beat);

  /// The latest consistent beat in `slot`, or nothing if none was ever
  /// published there, or kTorn if the writer is stuck mid-update.
  [[nodiscard]] core::Result<std::optional<Heartbeat>, RegionError> observe(std::size_t slot) const;

  /// Unmaps and closes now, reporting the answer. The destructor does the same
  /// silently; this is for a caller who must know.
  [[nodiscard]] core::Result<core::Ok, platform::SyscallError> release();

 private:
  HeartbeatRegion(platform::Syscalls& syscalls, int fd, void* base, std::size_t workers) noexcept;

  platform::Syscalls* syscalls_;
  int fd_;
  void* base_;
  void* slots_;  ///< One header past base_: where the slot array begins.
  std::size_t bytes_;
  std::size_t workers_;
};

/// Whether a beat says its writer is alive.
///
///   kNeverBeat  nothing was ever published: a worker that has not started,
///               or never reached its first beat.
///   kFresh      the beat is younger than `stale_after_ns`.
///   kStale      older. The writer is wedged or dead; which one, the reaper
///               says.
///
/// Age is `now_ns - timestamp_ns`. A beat from the "future" -- a timestamp
/// later than now -- is fresh: both come from CLOCK_MONOTONIC, so it can only
/// mean the reader's own clock reading is the older one, and that is not
/// staleness.
enum class Liveness : std::uint8_t { kNeverBeat, kFresh, kStale };

[[nodiscard]] std::string_view describe(Liveness liveness) noexcept;

[[nodiscard]] Liveness liveness(const std::optional<Heartbeat>& beat, std::int64_t now_ns,
                                std::int64_t stale_after_ns) noexcept;

/// Whether `later` shows work done since `earlier`. Equal progress is "alive
/// and not progressing", the state that distinguishes a spin from a slow
/// worker; a counter that went backwards is treated as no progress, because
/// a writer whose counter runs backwards is not one to credit.
[[nodiscard]] bool progressed(const Heartbeat& earlier, const Heartbeat& later) noexcept;

/// The seqlock reader's verdict on one sample: the sequence read before the
/// fields and the one read after. Accepted only when the first was even, no
/// write in progress, and the second is the same, no write began meanwhile.
///
/// A function of its own so that each outcome is a test rather than a race.
/// The third outcome, even before and changed after, happens in observe() only
/// when a writer lands between two loads, and a concurrency test meets that by
/// luck or not at all; coverage that depends on luck is a gate that passes or
/// fails by the run.
[[nodiscard]] bool sample_is_consistent(std::uint64_t before, std::uint64_t after) noexcept;

}  // namespace loadforge::ipc

#endif
