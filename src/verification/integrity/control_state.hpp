// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_INTEGRITY_CONTROL_STATE_HPP
#define LOADFORGE_VERIFICATION_INTEGRITY_CONTROL_STATE_HPP

// F18 rule 3 (PLAN.md F18, verification.md §4a): the values that verification
// itself depends on -- the seed a record names, the index a unit is
// regenerated from, the duty cycle that chooses which units are checked, the
// line size a classification uses -- are small, and catastrophic to lose
// silently. A corrupted index regenerates a different unit's input, the
// bracket then finds a "difference", and the tool reports a hardware fault
// that never happened. So they are kept three times and voted on every use.
//
// HOW THE COPIES ARE KEPT APART
// -----------------------------
//   * Each copy is its own mapping, so no two share a cache line or a page,
//     and each is populated and locked like the working set.
//   * Where nodes are given, copy i is bound to node i mod (number of nodes),
//     so on a machine with several the copies sit on different nodes.
//   * Each copy carries its own CRC-32 over its words. A copy whose checksum
//     fails is EXCLUDED, never voted with: a 2-of-3 vote among two good copies
//     and one silently bad one is much weaker than a 2-of-2 among two
//     verified ones.
//   * Every copy is encoded and stored by its own pass, from the value, with
//     its own checksum computation: never one buffer copied three times, so
//     one faulty store cannot corrupt all three.
//   * Reads go through volatile, so every vote reads the memory and not a
//     value the compiler remembered from the last write.
//
// THE VOTE
// --------
// The value is decided when at least two VERIFIED copies hold it; every copy
// that does not -- excluded by its checksum, or verified but outvoted -- is
// rewritten from the decided value and counted as a repair. Anything else is
// a failure, and the caller stops rather than picks:
//   kDisagreement    two or three verified copies, no two of them equal;
//   kTooFewVerified  fewer than two copies pass their checksums, so there is
//                    no vote to hold. One verified copy among two known-bad
//                    ones is a winner picked, not a majority found.
//
// THE HONEST LIMIT (PLAN F18): the vote runs on the machine under suspicion.
// Separate mappings are separate pages to the process; which DRAM rows they
// land in is the kernel's choice. This makes silently trusting a corrupted
// value less likely; it cannot make it impossible.
//
// Thread-safety: one thread at a time, like the runner that owns it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"
#include "platform/memory.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::verification::integrity {

inline constexpr std::size_t kCopies = 3;
inline constexpr std::size_t kControlWords = 6;
/// The words, then a little-endian CRC-32 over their little-endian bytes.
inline constexpr std::size_t kCopyBytes = kControlWords * 8 + 4;

/// What the runner keeps protected, one 64-bit word each.
enum class ControlWord : std::uint8_t {
  kSeed = 0,             ///< The run's seed, as every record names it.
  kSeedPresent = 1,      ///< 1 when the setup gave a seed, 0 when it left it unknown.
  kIndex = 2,            ///< The next unit to run: the one every step regenerates.
  kOraclePeriod = 3,     ///< DutyCycle::oracle_period.
  kIntegrityPeriod = 4,  ///< DutyCycle::integrity_period.
  kLineBytes = 5,        ///< The line size a classification uses.
};

using ControlWords = std::array<std::uint64_t, kControlWords>;

[[nodiscard]] constexpr std::uint64_t word(const ControlWords& words, ControlWord which) noexcept {
  return words.at(static_cast<std::size_t>(which));
}

constexpr void set_word(ControlWords& words, ControlWord which, std::uint64_t value) noexcept {
  words.at(static_cast<std::size_t>(which)) = value;
}

enum class VoteFault : std::uint8_t { kDisagreement, kTooFewVerified };

[[nodiscard]] std::string_view describe(VoteFault fault) noexcept;

/// A vote that decided nothing: why, and which copies passed their checksums.
struct VoteFailure {
  VoteFault fault = VoteFault::kTooFewVerified;
  std::array<bool, kCopies> verified{};
};

/// "three copies disagree (copy 0 verified, copy 1 verified, copy 2 verified)".
[[nodiscard]] std::string describe(const VoteFailure& failure);

/// A copy that could not be mapped: which one, and the platform's refusal.
struct ControlRefused {
  std::size_t copy = 0;
  platform::MappingRefused refused;
};

class ControlState {
 public:
  /// Maps the three copies -- populated, locked, bound to `nodes` round-robin
  /// when any are given -- and writes `initial` into each. A refusal at copy
  /// i unmaps copies 0 to i-1 before it returns.
  [[nodiscard]] static core::Result<ControlState, ControlRefused> create(
      platform::Syscalls& syscalls, const ControlWords& initial,
      std::span<const std::uint32_t> nodes);

  /// Votes, repairs every copy that did not hold the decided value, and
  /// returns it; or says why nothing was decided, and changes nothing.
  [[nodiscard]] core::Result<ControlWords, VoteFailure> read();

  /// Stores `words` into each copy in turn, each encoded and checksummed by
  /// its own pass.
  void write(const ControlWords& words) noexcept;

  /// Copies rewritten by votes so far.
  [[nodiscard]] std::uint64_t repairs() const noexcept { return repairs_; }

 private:
  explicit ControlState(std::vector<platform::Mapping> copies) noexcept;

  void write_copy(std::size_t copy, const ControlWords& words) noexcept;

  std::vector<platform::Mapping> copies_;
  std::uint64_t repairs_ = 0;
};

}  // namespace loadforge::verification::integrity

#endif
