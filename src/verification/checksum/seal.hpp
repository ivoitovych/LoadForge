// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_CHECKSUM_SEAL_HPP
#define LOADFORGE_VERIFICATION_CHECKSUM_SEAL_HPP

// A running checksum computed as input is generated and verified before it is
// used: verification.md §4a rule 2, for input that cannot be regenerated.
//
// WHAT A SEAL CAN AND CANNOT SAY
// ------------------------------
// Regeneration (rule 1) knows the expected bytes and can say which bits
// changed. A seal knows a length and 64 bits of CRC, so it can say only THAT
// the bytes changed between sealing and checking: the record it calls for is
// an ExactBitCorruption with no difference, never a fabricated bit pattern.
// It narrows the window in which a corruption can go unseen to the time
// between the two checks rather than closing it, and the record says that it
// is a checksum's finding.
//
// WHICH WAY A CORRUPTED SEAL FAILS
// --------------------------------
// The seal is stored data too, and memory can corrupt it. The rule (§4a
// rule 2) is that a corrupted checksum raises a false alarm rather than hides
// a fault, and this design keeps it by having no value that means "skip":
// every Seal is a length and a CRC, a check compares both, and any change to
// either -- or to the bytes -- is a mismatch. There is no zero CRC that reads
// as "not computed", no length that reads as "unknown". A seal that was never
// taken is std::nullopt, which is a different type of answer: the check was
// not made, VerificationNotPerformed. The only way a corrupted seal agrees
// with corrupted bytes is a CRC collision, one chance in 2^64.
//
// A mismatch cannot say whether the bytes or the stored seal changed. The
// record says "the bytes or the checksum sealed at their generation", which
// is what was observed; protecting the stored seal itself is control state
// (rule 3), the runner's.
//
// THREAD-SAFETY: RunningChecksum is a value; one per thread, or one guarded by
// the caller. The free functions are pure.

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "verification/checksum/compare.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {

/// What a checksum records at generation: how many bytes, and their CRC-64
/// (crc64.hpp).
struct Seal {
  std::uint64_t bytes = 0;
  std::uint64_t crc = 0;
};

[[nodiscard]] bool operator==(const Seal& lhs, const Seal& rhs) noexcept;

/// The seal of `bytes`, all at once.
[[nodiscard]] Seal seal(std::span<const std::byte> bytes) noexcept;

/// A seal taken as the bytes are written, in pieces of any size: the seal of
/// the concatenation of every update(), whatever the split.
class RunningChecksum {
 public:
  void update(std::span<const std::byte> bytes) noexcept;
  [[nodiscard]] Seal seal() const noexcept;

 private:
  std::uint64_t bytes_ = 0;
  std::uint64_t crc_ = 0;
};

///   kIntact           the bytes are the length and the CRC sealed.
///   kCorrupt          they are not: the bytes or the seal changed.
///   kNothingCompared  no bytes were given to check. Not a pass.
///   kNotSealed        no seal was taken, so there is nothing to check
///                     against. Not a pass.
enum class SealOutcome : std::uint8_t {
  kIntact = 0,
  kCorrupt = 1,
  kNothingCompared = 2,
  kNotSealed = 3,
};

[[nodiscard]] std::string_view describe(SealOutcome outcome) noexcept;

/// What verify_seal() found. `sealed` is the seal checked against (absent
/// for kNotSealed); `found` is the seal of the bytes as they are now (absent
/// unless the bytes were sealed and non-empty).
struct SealCheck {
  SealOutcome outcome = SealOutcome::kNotSealed;
  std::optional<Seal> sealed;
  std::optional<Seal> found;
};

[[nodiscard]] bool operator==(const SealCheck& lhs, const SealCheck& rhs) noexcept;

/// Checks `bytes` against the seal taken when they were generated. Empty bytes
/// are kNothingCompared whatever the seal says: a check over nothing has
/// checked nothing.
[[nodiscard]] SealCheck verify_seal(std::span<const std::byte> bytes,
                                    const std::optional<Seal>& sealed) noexcept;

/// The record a seal check calls for, or nothing for kIntact:
///   kCorrupt          ExactBitCorruption, no difference, its check naming
///                     both seals.
///   kNothingCompared, kNotSealed   VerificationNotPerformed, saying which.
/// The Subject's preconditions are compare.hpp's.
[[nodiscard]] std::optional<ErrorRecord> to_record(const SealCheck& check, const Subject& subject);

}  // namespace loadforge::verification

#endif
