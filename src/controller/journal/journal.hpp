// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_CONTROLLER_JOURNAL_JOURNAL_HPP
#define LOADFORGE_CONTROLLER_JOURNAL_JOURNAL_HPP

// CONTRACT (ADR 0004). The crash journal of PLAN F16: the evidence that
// survives the machine. Frozen: changed only by a contract change, never
// inside a stream.
//
//   Provider: the journal stream (src/controller/journal/): the format, the
//     writer, the replay, and a Storage over a real file.
//   Consumers: the controller, which appends an entry every few seconds and
//     syncs it, and at startup replays the previous run's journal and
//     reports an unterminated one as a finding in its own right; the mixed
//     workload and cycling (M5), whose phases the entries name.
//
// WHAT IT MUST SURVIVE
// --------------------
// A kernel panic, a hard lock, a power cut: the machine stops between any two
// bytes. So the journal is append-only and never rewritten in place -- the
// Storage below has no way to rewrite -- and replay never reads a torn or
// corrupt record as an entry: it returns the entries written before the
// damage, and says how the journal ended. A machine dying mid-write loses at
// most the record being written.
//
// The Storage is the seam: the journal's provider implements it over a file
// (open with O_APPEND, write, fsync), and every test of the format runs over
// memory, cutting and corrupting it at every byte.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "core/result.hpp"

namespace loadforge::journal {

/// One moment of the run, in about 200 bytes (F16).
///
///   elapsed_ns        CLOCK_MONOTONIC since the run started.
///   phase, workload   what the machine was doing; at most kMaxLabelBytes each.
///   last_verified_unit  the highest unit index whose checks had completed.
///   temperature_c, package_power_w, frequency_hz   the latest readings, each
///                     absent when the metric is.
///   ecc_corrected, ecc_uncorrected   machine-wide totals so far, absent
///                     without EDAC.
///   workers_alive, workers_stale    heartbeat state at the moment.
struct Entry {
  std::int64_t elapsed_ns = 0;
  std::string phase;
  std::string workload;
  std::optional<std::uint64_t> last_verified_unit;
  std::optional<double> temperature_c;
  std::optional<double> package_power_w;
  std::optional<double> frequency_hz;
  std::optional<std::uint64_t> ecc_corrected;
  std::optional<std::uint64_t> ecc_uncorrected;
  std::uint32_t workers_alive = 0;
  std::uint32_t workers_stale = 0;
};

[[nodiscard]] inline bool operator==(const Entry& lhs, const Entry& rhs) noexcept {
  return std::tie(lhs.elapsed_ns, lhs.phase, lhs.workload, lhs.last_verified_unit,
                  lhs.temperature_c, lhs.package_power_w, lhs.frequency_hz, lhs.ecc_corrected,
                  lhs.ecc_uncorrected, lhs.workers_alive, lhs.workers_stale) ==
         std::tie(rhs.elapsed_ns, rhs.phase, rhs.workload, rhs.last_verified_unit,
                  rhs.temperature_c, rhs.package_power_w, rhs.frequency_hz, rhs.ecc_corrected,
                  rhs.ecc_uncorrected, rhs.workers_alive, rhs.workers_stale);
}

inline constexpr std::size_t kMaxLabelBytes = 64;

/// Why the journal refused.
///
///   kStorage   the storage failed: the detail carries its words (the errno
///              of write or fsync, for the file provider).
///   kTooLarge  an entry's phase or workload is longer than kMaxLabelBytes.
///   kNotEmpty  a writer was opened on storage that already holds a journal:
///              a journal is written by one run, never continued by another.
///   kClosed    the writer was closed.
enum class JournalFault : std::uint8_t { kStorage, kTooLarge, kNotEmpty, kClosed };

[[nodiscard]] constexpr std::string_view describe(JournalFault fault) noexcept {
  switch (fault) {
    case JournalFault::kStorage:
      return "storage failed";
    case JournalFault::kTooLarge:
      return "entry too large";
    case JournalFault::kNotEmpty:
      return "journal already written";
    case JournalFault::kClosed:
      return "journal closed";
  }
  return "unrecognised journal fault";
}

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct JournalError {
  JournalFault fault = JournalFault::kStorage;
  std::string detail;
};

[[nodiscard]] inline bool operator==(const JournalError& lhs, const JournalError& rhs) noexcept {
  return std::tie(lhs.fault, lhs.detail) == std::tie(rhs.fault, rhs.detail);
}

/// Where a journal's bytes live. Append-only by construction.
///
/// append(bytes) adds bytes at the end; on failure some prefix of them may
/// have been added (a real file's short write). sync() returns once every
/// byte appended so far is durable. read_all() answers every byte, in order.
/// Each failure is kStorage with the reason.
///
/// Thread-safety: one thread at a time.
class Storage {
 public:
  Storage() = default;
  Storage(const Storage&) = delete;
  Storage(Storage&&) = delete;
  Storage& operator=(const Storage&) = delete;
  Storage& operator=(Storage&&) = delete;
  virtual ~Storage() = default;

  [[nodiscard]] virtual core::Result<core::Ok, JournalError> append(
      std::span<const std::byte> bytes) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, JournalError> sync() = 0;
  [[nodiscard]] virtual core::Result<std::vector<std::byte>, JournalError> read_all() const = 0;
};

/// Writes one run's journal.
///
/// append(entry) adds one record -- kTooLarge, leaving the storage untouched,
/// for a label past kMaxLabelBytes; kStorage when the storage fails. An
/// appended entry is durable once sync() returns. close() appends the
/// terminator and syncs: a journal that ends with it is a run that ended
/// cleanly. After close(), append, sync and close answer kClosed.
///
/// Thread-safety: one thread at a time.
class Writer {
 public:
  Writer() = default;
  Writer(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer& operator=(Writer&&) = delete;
  virtual ~Writer() = default;

  [[nodiscard]] virtual core::Result<core::Ok, JournalError> append(const Entry& entry) = 0;
  [[nodiscard]] virtual core::Result<core::Ok, JournalError> sync() = 0;
  [[nodiscard]] virtual core::Result<core::Ok, JournalError> close() = 0;
};

/// How a journal ended, which is the finding a replay reports.
///
///   kEmpty         no bytes: the run never wrote, or never started.
///   kTerminated    the terminator was the last record: the run ended cleanly.
///   kUnterminated  the bytes end at a record boundary with no terminator:
///                  the run stopped between two records -- the machine died,
///                  or the controller was killed.
///   kTornTail      the bytes end inside a record: the machine died while it
///                  was being written.
///   kCorrupt       a complete record failed its check, or bytes follow the
///                  terminator: the storage itself changed what was written.
///                  Nothing after it is read.
enum class Ending : std::uint8_t { kEmpty, kTerminated, kUnterminated, kTornTail, kCorrupt };

[[nodiscard]] constexpr std::string_view describe(Ending ending) noexcept {
  switch (ending) {
    case Ending::kEmpty:
      return "empty";
    case Ending::kTerminated:
      return "ended cleanly";
    case Ending::kUnterminated:
      return "ended abnormally between records";
    case Ending::kTornTail:
      return "ended abnormally inside a record";
    case Ending::kCorrupt:
      return "corrupt";
  }
  return "unrecognised ending";
}

/// What a replay recovered: the entries before any damage, in order, and how
/// the journal ended.
struct Replay {
  std::vector<Entry> entries;
  Ending ending = Ending::kEmpty;
};

[[nodiscard]] inline bool operator==(const Replay& lhs, const Replay& rhs) noexcept {
  return std::tie(lhs.entries, lhs.ending) == std::tie(rhs.entries, rhs.ending);
}

/// A writer over empty storage, or kNotEmpty. Provided by the journal stream.
[[nodiscard]] core::Result<std::unique_ptr<Writer>, JournalError> open_writer(Storage& storage);

/// The journal in `storage`, replayed. Provided by the journal stream.
///
/// Fails only when read_all() does. Otherwise, for storage holding a valid
/// journal cut at ANY byte, the entries are the ones whose records lie wholly
/// before the cut, and the ending is kEmpty, kUnterminated or kTornTail --
/// never kCorrupt, which a cut alone cannot cause. For storage with ANY byte
/// changed, the entries are a prefix of the ones written, each exactly as
/// written, and the ending is never kTerminated with every entry intact: a
/// record's check covers every byte of it, including its length.
[[nodiscard]] core::Result<Replay, JournalError> replay(const Storage& storage);

}  // namespace loadforge::journal

#endif
