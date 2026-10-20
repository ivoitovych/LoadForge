// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_CONTROL_HPP
#define LOADFORGE_WORKER_CONTROL_HPP

// CONTRACT (ADR 0004). What the controller can tell a running worker, for
// the mixed workload and for thermal and power cycling (IMPLEMENTATION M5).
// Frozen: changed only by a contract change, never inside a stream.
//
//   Provider of the behaviour, and of encode/decode: the worker-control
//     stream (src/worker/, the worker's loop).
//   Consumer: the mixed-workload and cycling stream (the controller side),
//     which schedules phases and rotations.
//
// ON THE WIRE
// -----------
// Up to M5 the control pipe carries one unframed byte, meaning stop. From the
// worker-control stream on, it carries ipc frames (worker/ipc/record.hpp) of
// type kControlFrameType, each payload encode(message), and the worker
// answers each on its record pipe with a frame of type kControlAckFrameType.
// The stop byte is retired in the same change, on both sides at once; there
// is no release in which both are spoken.
//
// SEMANTICS, AS THE WORKER KEEPS THEM
// -----------------------------------
// Messages are handled in the order they arrive, between slices, and each is
// acknowledged exactly once, in the same order, with its sequence number.
//
//   Stop       finish the runner (its final records are sent), report
//              finished, exit 0. Acknowledged before the finished record.
//   Pause      run no slice until Resume; keep beating, with progress
//              unchanged. The controller paused it and does not judge a paused
//              worker by its progress.
//   Resume     run slices again. A Resume while running is acknowledged kDone.
//   SetDuty    from now on, in every period of period_ns, run slices for
//              busy_permille thousandths of it and idle the rest, beating
//              throughout: the square wave of thermal and power cycling.
//              1000 is continuous load.
//   Rebind     move the worker's threads to exactly these CPUs. kRefused,
//              with the kernel's reason, when the cpuset does not permit
//              them; the worker then stays where it was.
//   Switch     finish the current runner (its records are sent) and start
//              the named workload with this configuration and duty cycle.
//              kRefused when the workload cannot be created; the worker then
//              keeps running the old one.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include "core/result.hpp"
#include "verification/record.hpp"
#include "workloads/workload.hpp"

namespace loadforge::worker {

inline constexpr std::uint16_t kControlFrameType = 32;
inline constexpr std::uint16_t kControlAckFrameType = 33;
inline constexpr std::uint8_t kControlFormatVersion = 1;

struct Stop {};
struct Pause {};
struct Resume {};

[[nodiscard]] constexpr bool operator==(Stop /*lhs*/, Stop /*rhs*/) noexcept { return true; }
[[nodiscard]] constexpr bool operator==(Pause /*lhs*/, Pause /*rhs*/) noexcept { return true; }
[[nodiscard]] constexpr bool operator==(Resume /*lhs*/, Resume /*rhs*/) noexcept { return true; }

/// busy_permille in [1, 1000]; period_ns > 0.
struct SetDuty {
  std::uint16_t busy_permille = 1000;
  std::int64_t period_ns = 1'000'000'000;
};

[[nodiscard]] inline bool operator==(const SetDuty& lhs, const SetDuty& rhs) noexcept {
  return std::tie(lhs.busy_permille, lhs.period_ns) == std::tie(rhs.busy_permille, rhs.period_ns);
}

/// cpus non-empty, strictly ascending (so without duplicates), each below
/// the affinity mask's 1024.
struct Rebind {
  std::vector<std::uint32_t> cpus;
};

[[nodiscard]] inline bool operator==(const Rebind& lhs, const Rebind& rhs) noexcept {
  return lhs.cpus == rhs.cpus;
}

/// workload a valid workload name (workloads/workload.hpp); footprint_bytes
/// above 0; both duty periods as verification::DutyCycle defines them.
struct Switch {
  std::string workload;
  std::uint64_t seed = 0;
  std::uint64_t footprint_bytes = 0;
  std::uint32_t decomposition = 0;
  verification::DutyCycle duty;
};

[[nodiscard]] inline bool operator==(const Switch& lhs, const Switch& rhs) noexcept {
  return std::tie(lhs.workload, lhs.seed, lhs.footprint_bytes, lhs.decomposition, lhs.duty) ==
         std::tie(rhs.workload, rhs.seed, rhs.footprint_bytes, rhs.decomposition, rhs.duty);
}

using Command = std::variant<Stop, Pause, Resume, SetDuty, Rebind, Switch>;

/// The command's name, for logs and acknowledgements.
[[nodiscard]] constexpr std::string_view command_name(const Command& command) noexcept {
  switch (command.index()) {
    case 0:
      return "stop";
    case 1:
      return "pause";
    case 2:
      return "resume";
    case 3:
      return "set duty";
    case 4:
      return "rebind";
    default:
      return "switch";
  }
}

struct ControlMessage {
  std::uint32_t sequence = 0;
  Command command;
};

/// Equal sequence and the same command, compared within its alternative.
[[nodiscard]] inline bool operator==(const ControlMessage& lhs,
                                     const ControlMessage& rhs) noexcept {
  if (lhs.sequence != rhs.sequence || lhs.command.index() != rhs.command.index()) {
    return false;
  }
  if (const auto* duty = std::get_if<SetDuty>(&lhs.command)) {
    return *duty == *std::get_if<SetDuty>(&rhs.command);
  }
  if (const auto* rebind = std::get_if<Rebind>(&lhs.command)) {
    return *rebind == *std::get_if<Rebind>(&rhs.command);
  }
  if (const auto* change = std::get_if<Switch>(&lhs.command)) {
    return *change == *std::get_if<Switch>(&rhs.command);
  }
  return true;  // Stop, Pause and Resume carry nothing.
}

/// Whether a message keeps the rules on its command.
[[nodiscard]] inline bool is_valid(const ControlMessage& message) noexcept {
  if (const auto* duty = std::get_if<SetDuty>(&message.command)) {
    return duty->busy_permille >= 1 && duty->busy_permille <= 1000 && duty->period_ns > 0;
  }
  if (const auto* rebind = std::get_if<Rebind>(&message.command)) {
    if (rebind->cpus.empty() || rebind->cpus.back() >= 1024) {
      return false;
    }
    for (std::size_t at = 1; at < rebind->cpus.size(); ++at) {
      if (rebind->cpus[at - 1] >= rebind->cpus[at]) {
        return false;
      }
    }
    return true;
  }
  if (const auto* change = std::get_if<Switch>(&message.command)) {
    return workloads::is_valid_name(change->workload) && change->footprint_bytes > 0;
  }
  return true;
}

enum class AckStatus : std::uint8_t { kDone = 0, kRefused = 1 };

struct ControlAck {
  std::uint32_t sequence = 0;
  AckStatus status = AckStatus::kDone;
  std::string detail;  ///< Why, for kRefused; at most verification::kMaxTextBytes.
};

[[nodiscard]] inline bool is_valid(const ControlAck& ack) noexcept {
  return ack.status <= AckStatus::kRefused && ack.detail.size() <= verification::kMaxTextBytes;
}

[[nodiscard]] inline bool operator==(const ControlAck& lhs, const ControlAck& rhs) noexcept {
  return std::tie(lhs.sequence, lhs.status, lhs.detail) ==
         std::tie(rhs.sequence, rhs.status, rhs.detail);
}

enum class ControlDecodeFault : std::uint8_t {
  kEmpty,
  kVersion,
  kKind,
  kTruncated,
  kTrailing,
  kInvalid,
};

// NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
struct ControlDecodeError {
  ControlDecodeFault fault = ControlDecodeFault::kEmpty;
  std::string detail;
};

/// The encodings, provided by the worker-control stream. As for error records
/// (verification/record.hpp): the first two bytes of a message are
/// kControlFormatVersion and the command's index plus one; of an
/// acknowledgement, kControlFormatVersion and 0x80. decode(encode(m)) == m
/// for every valid m, and likewise for acknowledgements; an invalid one, a strict prefix, trailing
/// bytes, an unknown version or kind are refused as such and never guessed at.
[[nodiscard]] std::vector<std::byte> encode(const ControlMessage& message);
[[nodiscard]] core::Result<ControlMessage, ControlDecodeError> decode_message(
    std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> encode(const ControlAck& ack);
[[nodiscard]] core::Result<ControlAck, ControlDecodeError> decode_ack(
    std::span<const std::byte> bytes);

}  // namespace loadforge::worker

#endif
