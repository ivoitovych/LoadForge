// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_FRAMES_HPP
#define LOADFORGE_WORKER_FRAMES_HPP

// What a verifying worker puts on its record channel, beyond the text records
// of M1 (ipc::RecordType) and the error records of verification/record.hpp
// (frame type 16).
//
//   frame type 17  a tally: the worker's running total, cumulative, sent with
//                  every heartbeat tick that did work. Cumulative rather than
//                  a delta, so a frame the controller never read (a channel
//                  that went bad) costs the controller a stale figure and not
//                  a wrong one, and so the last frame is the answer.
//
//   frame type 18  the working set's memory lock: what the runner did about
//                  locking it (verification::MemoryLock), sent once, before the
//                  first slice. The policy is the controller's choice and the
//                  outcome is the worker's fact: a try that fell back is
//                  known only here. (M2 contract corrections, 2.)
//
// The numbers are the frame types of the contracts' ranges: 1-3 the transport's
// own, 16 error records, 17 tallies and 18 memory locks (here), 32 and 33
// control (worker/control.hpp).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/result.hpp"
#include "verification/runner.hpp"

namespace loadforge::worker {

inline constexpr std::uint16_t kTallyFrameType = 17;
inline constexpr std::uint8_t kTallyFormatVersion = 1;

/// Seven counters, each eight bytes little-endian, after the version byte.
inline constexpr std::size_t kTallyFrameBytes = 1 + 7 * 8;

inline constexpr std::uint16_t kMemoryFrameType = 18;
inline constexpr std::uint8_t kMemoryFormatVersion = 1;

/// The longest detail a memory frame carries: a refusal in words, cut here
/// rather than refused by the reader.
inline constexpr std::size_t kMaxMemoryDetailBytes = 1024;

/// u8 version, u8 policy, u8 outcome, then the detail's bytes to the end.
[[nodiscard]] std::vector<std::byte> encode_memory(const verification::MemoryLock& lock);

/// The lock the bytes describe, or why they describe none: fewer than three
/// bytes, an unknown version, policy or outcome, a detail longer than
/// kMaxMemoryDetailBytes, or a detail on an outcome that has none to give
/// (anything but kFellBack). Never a guess.
[[nodiscard]] core::Result<verification::MemoryLock, std::string> decode_memory(
    std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> encode_tally(const verification::Tally& tally);

/// The tally the bytes describe, or why they describe none: the wrong length
/// or an unknown version. Never a guess.
[[nodiscard]] core::Result<verification::Tally, std::string> decode_tally(
    std::span<const std::byte> bytes);

}  // namespace loadforge::worker

#endif
