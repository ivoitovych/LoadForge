// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 for the input-integrity bracket (verification.md §4a, §6): the memory a
// runner holds is changed at each moment the bracket exists to tell apart, and
// each test asserts WHICH record is made -- side, input integrity, offsets,
// popcount and bit classification -- not merely that one is.
//
//   * the slot's input changed after it was written and before it was read:
//     the opening check, Side::kInput, kCorruptBeforeConsumption, and the
//     unit is neither computed nor given to the oracle;
//   * the slot's input changed while the unit read it: the oracle agrees --
//     the output is right for the input it was computed from -- and only the
//     closing check sees it, kCorruptDuringConsumption;
//   * the output changed after it was computed: Side::kOutput, the bracket
//     intact; and for a bounded workload a NumericalVerificationFailure,
//     which has no bits.
//
// The changes are made to the runner's own memory, through the very span it
// wrote, not by a workload generating differently: what is caught is stored
// bytes that differ from their regeneration.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "platform/syscalls.hpp"
#include "unit/verification/integrity/integrity_support.hpp"
#include "verification/record.hpp"
#include "verification/runner.hpp"

namespace loadforge::integrity_testing {
namespace {

using verification::BitClass;
using verification::ExactBitCorruption;
using verification::InputIntegrity;
using verification::Side;

/// The rotor workload behind a tampering wrapper, its oracle behind a
/// recording one, over the system-call fake or the real kernel.
struct Rig {
  explicit Rig(verification::DutyCycle duty = {1, 1}, std::uint32_t line_bytes = 64)
      : workload(make_rotor()), tamper(*workload), oracle(inner_oracle) {
    setup = setup_for(tamper, oracle, executor, real, duty);
    setup.line_bytes = line_bytes;
  }
  std::unique_ptr<verification::SliceRunner> runner() const { return make_runner(setup); }

  std::unique_ptr<workloads::Workload> workload;
  TamperingWorkload tamper;
  RotorOracle inner_oracle;
  ScriptedOracle oracle;
  SerialExecutor executor;
  platform::RealSyscalls real;
  verification::RunnerSetup setup;
};

/// Flips the bits of `mask` at each byte offset given, for unit `index`.
TamperingWorkload::Hook flip(std::uint64_t index,
                             std::vector<std::pair<std::size_t, std::uint8_t>> flips) {
  return [index, flips = std::move(flips)](std::uint64_t unit, std::span<std::byte> bytes) {
    if (unit != index) {
      return;
    }
    for (const auto& [offset, mask] : flips) {
      bytes[offset] ^= static_cast<std::byte>(mask);
    }
  };
}

struct Pattern {
  const char* name;
  std::vector<std::pair<std::size_t, std::uint8_t>> flips;
  std::uint32_t line_bytes;
  std::uint64_t first;
  std::uint64_t last;
  std::uint64_t popcount;
  BitClass expected;
};

/// Every classification, and the line size changing the answer.
std::vector<Pattern> patterns() {
  return {
      {"one bit", {{10, 0x04}}, 64, 10, 10, 1, BitClass::kSingleBit},
      {"one bit in the last byte", {{199, 0x80}}, 64, 199, 199, 1, BitClass::kSingleBit},
      {"two bits in one byte", {{16, 0x81}}, 64, 16, 16, 2, BitClass::kMultiBitInWord},
      {"bits in two bytes of one word",
       {{16, 0x03}, {23, 0x80}},
       64,
       16,
       23,
       3,
       BitClass::kMultiBitInWord},
      {"two words of one line",
       {{70, 0x01}, {120, 0x01}},
       64,
       70,
       120,
       2,
       BitClass::kWithinCacheLine},
      {"two lines at 64 bytes",
       {{60, 0x01}, {70, 0x01}},
       64,
       60,
       70,
       2,
       BitClass::kAcrossCacheLines},
      {"the same bytes, one line at 128",
       {{60, 0x01}, {70, 0x01}},
       128,
       60,
       70,
       2,
       BitClass::kWithinCacheLine},
      {"first and last byte",
       {{0, 0xFF}, {199, 0xFF}},
       64,
       0,
       199,
       16,
       BitClass::kAcrossCacheLines},
  };
}

void expect_input_record(const verification::ErrorRecord& record, const Pattern& pattern,
                         InputIntegrity integrity, std::uint64_t index) {
  const auto* exact = std::get_if<ExactBitCorruption>(&record);
  ASSERT_NE(exact, nullptr) << verification::kind_name(record);
  EXPECT_EQ(exact->side, Side::kInput);
  EXPECT_EQ(exact->check, "input against its regeneration");
  EXPECT_EQ(exact->line_bytes, pattern.line_bytes);
  EXPECT_EQ(exact->provenance.input_integrity, integrity);
  EXPECT_EQ(exact->provenance.iteration, index);
  EXPECT_EQ(exact->provenance.seed, 7U);
  ASSERT_TRUE(exact->difference.has_value());
  const verification::Difference& difference = *exact->difference;
  EXPECT_EQ(difference.domain, verification::Domain::kStored);
  EXPECT_EQ(difference.first_offset, pattern.first);
  EXPECT_EQ(difference.last_offset, pattern.last);
  EXPECT_EQ(difference.popcount, pattern.popcount);
  EXPECT_EQ(verification::classify(*exact), pattern.expected);
  EXPECT_EQ(*exact->provenance.buffer_address % pattern.line_bytes, 0U);
  EXPECT_TRUE(verification::is_valid(record));
  EXPECT_TRUE(verification::is_wrong_result(record));

  // The window holds the regenerated truth and the stored bytes.
  std::vector<std::byte> truth(200);
  make_rotor()->generate(index, truth);
  for (std::size_t at = 0; at < difference.expected.size(); ++at) {
    EXPECT_EQ(difference.expected[at], truth[difference.window_offset + at]);
  }
  EXPECT_NE(difference.actual, difference.expected);
}

TEST(BracketTest, AnInputChangedBeforeItIsReadIsCaughtOpeningAndTheUnitStops) {
  for (const Pattern& pattern : patterns()) {
    SCOPED_TRACE(pattern.name);
    Rig rig({1, 1}, pattern.line_bytes);
    rig.tamper.after_write = flip(3, pattern.flips);
    const auto runner = rig.runner();
    ASSERT_NE(runner, nullptr);
    const verification::SliceReport report = slice(*runner, 6);
    ASSERT_EQ(report.records.size(), 1U);
    expect_input_record(report.records[0], pattern, InputIntegrity::kCorruptBeforeConsumption, 3);
    EXPECT_EQ(report.tally.units_computed, 5U) << "unit 3 was not computed";
    EXPECT_EQ(report.tally.integrity_checked, 6U);
    EXPECT_EQ(report.tally.integrity_failed, 1U);
    EXPECT_EQ(report.tally.oracle_checked, 5U);
    EXPECT_EQ(rig.oracle.checked, (std::vector<std::uint64_t>{0, 1, 2, 4, 5}))
        << "an input known wrong is never given to the oracle";
  }
}

TEST(BracketTest, AnInputChangedWhileItIsReadIsCaughtOnlyByTheClosingCheck) {
  for (const Pattern& pattern : patterns()) {
    SCOPED_TRACE(pattern.name);
    Rig rig({1, 1}, pattern.line_bytes);
    rig.tamper.during_compute = flip(2, pattern.flips);
    const auto runner = rig.runner();
    ASSERT_NE(runner, nullptr);
    const verification::SliceReport report = slice(*runner, 4);
    ASSERT_EQ(report.records.size(), 1U)
        << "the oracle agrees: the output is right for the input it was computed from";
    expect_input_record(report.records[0], pattern, InputIntegrity::kCorruptDuringConsumption, 2);
    EXPECT_EQ(report.tally.units_computed, 4U);
    EXPECT_EQ(report.tally.oracle_failed, 0U);
    EXPECT_EQ(report.tally.integrity_failed, 1U);
  }
}

TEST(BracketTest, AWrongOutputIsAnOutputRecordWithTheBracketIntact) {
  Rig rig({1, 1}, 128);
  rig.tamper.after_compute = flip(5, {{20, 0x01}});
  const auto runner = rig.runner();
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 7);
  ASSERT_EQ(report.records.size(), 1U);
  const auto* exact = std::get_if<ExactBitCorruption>(report.records.data());
  ASSERT_NE(exact, nullptr);
  EXPECT_EQ(exact->side, Side::kOutput);
  EXPECT_EQ(exact->line_bytes, 128U);
  EXPECT_EQ(exact->provenance.input_integrity, InputIntegrity::kIntact);
  EXPECT_EQ(exact->provenance.iteration, 5U);
  ASSERT_TRUE(exact->difference.has_value());
  EXPECT_EQ(exact->difference->first_offset, 20U);
  EXPECT_EQ(verification::classify(*exact), BitClass::kSingleBit);
  EXPECT_EQ(*exact->provenance.buffer_address % 128, 0U) << "the OUTPUT slot's address";
  EXPECT_TRUE(verification::is_valid(report.records[0]));
  EXPECT_TRUE(verification::is_wrong_result(report.records[0]));
  EXPECT_EQ(report.tally.oracle_failed, 1U);
  EXPECT_EQ(report.tally.integrity_failed, 0U);
}

TEST(BracketTest, InputAndOutputRecordsOfOneUnitAreTwoFindingsInStepOrder) {
  Rig rig;
  rig.tamper.during_compute = flip(1, {{40, 0x10}});
  rig.tamper.after_compute = flip(1, {{41, 0x10}});
  const auto runner = rig.runner();
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 3);
  ASSERT_EQ(report.records.size(), 2U);
  const auto* check = std::get_if<ExactBitCorruption>(report.records.data());
  const auto* close = std::get_if<ExactBitCorruption>(&report.records[1]);
  ASSERT_NE(check, nullptr);
  ASSERT_NE(close, nullptr);
  EXPECT_EQ(check->side, Side::kOutput);
  EXPECT_EQ(check->difference->first_offset, 41U);
  EXPECT_EQ(check->provenance.input_integrity, InputIntegrity::kCorruptDuringConsumption)
      << "the output's record says what the bracket found for its unit";
  EXPECT_EQ(close->side, Side::kInput);
  EXPECT_EQ(close->difference->first_offset, 40U);
  EXPECT_NE(check->provenance.buffer_address, close->provenance.buffer_address);
}

TEST(BracketTest, AUnitTheDutyCycleDidNotBracketSaysSoAndItsInputGoesUnseen) {
  Rig rig({1, 2});
  rig.tamper.during_compute = flip(3, {{7, 0x01}});
  rig.tamper.after_compute = flip(5, {{7, 0x01}});
  const auto runner = rig.runner();
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 6);
  ASSERT_EQ(report.records.size(), 1U)
      << "unit 3's input changed while read, and nothing bracketed it: the duty cycle's cost";
  const auto* exact = std::get_if<ExactBitCorruption>(report.records.data());
  ASSERT_NE(exact, nullptr);
  EXPECT_EQ(exact->provenance.iteration, 5U);
  EXPECT_EQ(exact->provenance.input_integrity, InputIntegrity::kNotChecked)
      << "stated, never assumed";
  EXPECT_EQ(report.tally.integrity_checked, 3U);
}

TEST(BracketTest, ABoundedWorkloadsWrongOutputHasNoBitsAndItsWrongInputDoes) {
  const auto inner = make_mean();
  TamperingWorkload tamper(*inner);
  const MeanOracle oracle;
  SerialExecutor executor;
  platform::RealSyscalls syscalls;
  tamper.after_compute = flip(1, {{7, 0x40}});  // the exponent's top bit
  tamper.after_write = flip(2, {{33, 0x02}});
  const auto runner = make_runner(setup_for(tamper, oracle, executor, syscalls));
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 3);
  ASSERT_EQ(report.records.size(), 2U);

  const auto* numerical =
      std::get_if<verification::NumericalVerificationFailure>(report.records.data());
  ASSERT_NE(numerical, nullptr) << verification::kind_name(report.records[0]);
  EXPECT_EQ(numerical->check, "|output - mean|");
  EXPECT_GT(verification::ratio(*numerical), 1.0);
  EXPECT_EQ(numerical->tolerance, MeanOracle::kTolerance);
  EXPECT_EQ(numerical->provenance.input_integrity, InputIntegrity::kIntact);
  EXPECT_TRUE(verification::is_valid(report.records[0]));
  EXPECT_TRUE(verification::is_wrong_result(report.records[0]));

  // The bracket is exact whatever the oracle's contract: input bits are known.
  const auto* input = std::get_if<ExactBitCorruption>(&report.records[1]);
  ASSERT_NE(input, nullptr);
  EXPECT_EQ(input->side, Side::kInput);
  EXPECT_EQ(input->provenance.input_integrity, InputIntegrity::kCorruptBeforeConsumption);
  EXPECT_EQ(verification::classify(*input), BitClass::kSingleBit);
  EXPECT_EQ(report.tally.oracle_failed, 1U);
  EXPECT_EQ(report.tally.integrity_failed, 1U);
}

TEST(BracketTest, ACorruptedSlotIsRewrittenWhenItsNextUnitComes) {
  Rig rig;
  rig.tamper.after_write = flip(1, {{0, 0x01}});
  const auto runner = rig.runner();
  ASSERT_NE(runner, nullptr);
  const verification::SliceReport report = slice(*runner, 17);
  ASSERT_EQ(report.records.size(), 1U) << "units 9 and 17 reuse slot 1, written afresh";
  EXPECT_EQ(verification::provenance_of(report.records[0]).iteration, 1U);
}

}  // namespace
}  // namespace loadforge::integrity_testing
