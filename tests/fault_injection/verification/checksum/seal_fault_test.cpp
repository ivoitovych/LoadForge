// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the seal (verification.md §4a rule 2, §6): input corrupted after it
// was generated and sealed is caught before it is used; a corrupted seal
// raises a false alarm rather than hiding anything; and T5b, a seal check
// that cannot be made is VerificationNotPerformed, never a pass.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "verification/checksum/seal.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

using Bytes = std::vector<std::byte>;

constexpr std::size_t kInputBytes = 4096;

/// Input generated in pieces and sealed as it is written, as a producer that
/// cannot regenerate it would: the seal is taken at generation, not later.
struct Generated {
  Bytes input;
  Seal sealed;
};

Generated generate() {
  Generated generated{Bytes(kInputBytes), {}};
  RunningChecksum running;
  for (std::size_t at = 0; at < kInputBytes; at += 1000) {
    const auto piece =
        std::span(generated.input).subspan(at, std::min<std::size_t>(1000, kInputBytes - at));
    rng::fill_stream(0xF18, at, piece);
    running.update(piece);
  }
  generated.sealed = running.seal();
  return generated;
}

Subject subject() {
  Subject subject;
  subject.side = Side::kInput;
  subject.check = "input against its checksum";
  return subject;
}

/// The check made before use, and what it reports.
std::optional<ErrorRecord> check_before_use(std::span<const std::byte> input,
                                            const std::optional<Seal>& sealed) {
  return to_record(verify_seal(input, sealed), subject());
}

void expect_alarm(const std::optional<ErrorRecord>& record, const char* what) {
  ASSERT_TRUE(record.has_value()) << what << ": went unnoticed";
  const auto* exact = std::get_if<ExactBitCorruption>(&*record);
  ASSERT_NE(exact, nullptr) << what;
  EXPECT_EQ(exact->side, Side::kInput) << what;
  EXPECT_FALSE(exact->difference.has_value()) << what << ": a checksum knows no bits";
  EXPECT_TRUE(is_valid(*record)) << what;
}

TEST(SealFaultInjection, AnUntouchedInputPassesItsCheck) {
  const Generated generated = generate();
  EXPECT_EQ(check_before_use(generated.input, generated.sealed), std::nullopt);
}

TEST(SealFaultInjection, EverySingleBitFlipAfterGenerationIsCaughtBeforeUse) {
  const Generated generated = generate();
  for (std::size_t byte = 0; byte < kInputBytes; ++byte) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      Bytes corrupted = generated.input;
      corrupted[byte] ^= static_cast<std::byte>(1U << bit);
      const SealCheck check = verify_seal(corrupted, generated.sealed);
      ASSERT_EQ(check.outcome, SealOutcome::kCorrupt) << "byte " << byte << " bit " << bit;
    }
  }
  Bytes corrupted = generated.input;
  corrupted[1234] ^= std::byte{0x10};
  expect_alarm(check_before_use(corrupted, generated.sealed), "one flipped bit");
}

TEST(SealFaultInjection, AWholeLineOverwrittenAndAByteLostAreCaught) {
  const Generated generated = generate();
  Bytes overwritten = generated.input;
  for (std::size_t byte = 128; byte < 192; ++byte) {
    overwritten[byte] = std::byte{0};
  }
  expect_alarm(check_before_use(overwritten, generated.sealed), "a line of zeros");
  const Bytes shorter(generated.input.begin(), generated.input.end() - 1);
  expect_alarm(check_before_use(shorter, generated.sealed), "one byte short");
}

TEST(SealFaultInjection, ACorruptedSealRaisesAFalseAlarmNeverSilence) {
  // §4a rule 2: the seal's own corruption fails safe. Every single-bit flip of
  // either field of the seal, over input that is intact, is an alarm.
  const Generated generated = generate();
  for (unsigned bit = 0; bit < 64; ++bit) {
    Seal crc_flipped = generated.sealed;
    crc_flipped.crc ^= std::uint64_t{1} << bit;
    expect_alarm(check_before_use(generated.input, crc_flipped), "crc bit flipped");
    Seal length_flipped = generated.sealed;
    length_flipped.bytes ^= std::uint64_t{1} << bit;
    expect_alarm(check_before_use(generated.input, length_flipped), "length bit flipped");
  }
  // And the values a "not computed" sentinel would have been: none is one.
  for (const Seal sentinel : {Seal{0, 0}, Seal{kInputBytes, 0}, Seal{0, generated.sealed.crc}}) {
    expect_alarm(check_before_use(generated.input, sentinel), "a sentinel-shaped seal");
  }
}

TEST(SealFaultInjection, CorruptInputUnderACorruptSealIsStillAnAlarm) {
  const Generated generated = generate();
  for (unsigned bit = 0; bit < 64; ++bit) {
    Bytes corrupted = generated.input;
    corrupted[std::size_t{bit} * 61] ^= std::byte{0x01};
    Seal sealed = generated.sealed;
    sealed.crc ^= std::uint64_t{1} << bit;
    expect_alarm(check_before_use(corrupted, sealed), "both corrupted");
  }
}

TEST(SealFaultInjection, NoSealIsNotAPass) {
  // T5b: the seal was never taken.
  const Generated generated = generate();
  const std::optional<ErrorRecord> record = check_before_use(generated.input, std::nullopt);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(std::holds_alternative<VerificationNotPerformed>(*record));
  EXPECT_FALSE(is_wrong_result(*record));
}

TEST(SealFaultInjection, NoInputIsNotAPass) {
  // T5b: the input to check is missing.
  const Generated generated = generate();
  const std::optional<ErrorRecord> record = check_before_use({}, generated.sealed);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(std::holds_alternative<VerificationNotPerformed>(*record));
  EXPECT_FALSE(is_wrong_result(*record));
}

}  // namespace
}  // namespace loadforge::verification
