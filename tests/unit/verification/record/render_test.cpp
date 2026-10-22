// SPDX-License-Identifier: GPL-3.0-or-later
//
// A record as a person reads it: every kind, every field present and absent,
// and the three things the text must never do -- print an absent field as a
// value, present derived bits as stored ones, or let an unrun check read as
// anything but a failure of the run.

#include "verification/record/render.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

#include "contracts/verification/record_contract.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

bool contains(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

TEST(RecordRender, AnExactCorruptionWithEveryFieldReadsInFull) {
  EXPECT_EQ(render(contracts::record_samples::exact()),
            "ExactBitCorruption: the input was found corrupt\n"
            "  check: input against its regeneration\n"
            "  difference in stored bytes: 1 bit(s) differ, from byte 4099 to byte 4099 of the "
            "compared bytes\n"
            "  classification: single bit, with 128-byte cache lines\n"
            "  window from byte 4096:\n"
            "    expected 00 00 00 00 00 00 00 00\n"
            "    actual   00 00 00 10 00 00 00 00\n"
            "    xor      00 00 00 10 00 00 00 00\n"
            "  provenance:\n"
            "    logical CPU: 11\n"
            "    physical package: 1\n"
            "    physical core (within its package): 5\n"
            "    NUMA node: 1\n"
            "    buffer address: 0x7f0012340000\n"
            "    buffer offset: 0x40000\n"
            "    iteration: 123456789\n"
            "    workload: compression\n"
            "    workload version: 3\n"
            "    seed: 0xfeedfacecafebeef\n"
            "    ISA path: avx2\n"
            "    input integrity: corrupt during consumption\n"
            "    duty cycle: oracle on 1 unit in 4, input integrity on 1 unit in 16\n"
            "    decomposition: 64\n"
            "    footprint bytes: 33554432\n");
}

TEST(RecordRender, AbsentProvenanceIsNotRecordedNeverZero) {
  const std::string text = render(contracts::record_samples::not_performed());
  for (const char* label :
       {"logical CPU", "physical package", "physical core (within its package)", "NUMA node",
        "buffer address", "buffer offset", "iteration", "workload", "workload version", "seed",
        "ISA path", "input integrity", "duty cycle", "decomposition", "footprint bytes"}) {
    EXPECT_TRUE(contains(text, std::string("    ") + label + ": not recorded\n")) << label;
  }
  EXPECT_FALSE(contains(text, "0x0")) << "no manufactured address";
}

TEST(RecordRender, AnExactCorruptionWithoutBitsSaysItHasNone) {
  ExactBitCorruption record = contracts::record_samples::exact();
  record.side = Side::kOutput;
  record.check = "compressed stream malformed at byte 517";
  record.difference.reset();
  const std::string text = render(record);
  EXPECT_TRUE(contains(text, "ExactBitCorruption: the output was found corrupt\n"));
  EXPECT_TRUE(contains(text,
                       "  difference: none -- the bytes could not be compared one for one, "
                       "so there is no bit evidence to classify\n"));
  EXPECT_FALSE(contains(text, "classification"));
  EXPECT_FALSE(contains(text, "popcount"));
}

TEST(RecordRender, ADerivedClassificationSaysWhoseBitsItDescribes) {
  ExactBitCorruption record = contracts::record_samples::exact();
  record.difference->domain = Domain::kDerived;
  record.difference->popcount = 3;
  record.difference->last_offset = 0x1100;
  EXPECT_TRUE(contains(render(record),
                       "  difference in derived bytes: 3 bit(s) differ, from byte 4099 to byte "
                       "4352 of the compared bytes\n"
                       "  classification: across cache lines, with 128-byte cache lines -- of "
                       "bytes the oracle derived, not bits the hardware stored\n"));
}

TEST(RecordRender, ABoundedFailureHasNoBitsAndSaysSo) {
  EXPECT_TRUE(
      render(contracts::record_samples::numerical())
          .starts_with("NumericalVerificationFailure: a bounded check exceeded its derived "
                       "tolerance\n"
                       "  check: componentwise |C - AB|\n"
                       "  residual: 3.5e-12\n"
                       "  tolerance: 1.25e-13\n"
                       "  ratio: 28 (residual over tolerance)\n"
                       "  condition number: 42\n"
                       "  element: 9001\n"
                       "  bit fields: not applicable -- a bounded check has no expected bit "
                       "pattern\n"
                       "  provenance:\n"));
  NumericalVerificationFailure bare = contracts::record_samples::numerical();
  bare.residual = std::numeric_limits<double>::quiet_NaN();
  bare.condition_number.reset();
  bare.element.reset();
  const std::string text = render(bare);
  EXPECT_TRUE(contains(text, "  residual: nan\n  tolerance: 1.25e-13\n  ratio: nan "));
  EXPECT_TRUE(contains(text, "  condition number: not recorded\n  element: not recorded\n"));
  EXPECT_FALSE(contains(text, "classification"));
}

TEST(RecordRender, AnUnrunCheckIsAFailureOfTheRunNotAPass) {
  VerificationNotPerformed record = contracts::record_samples::not_performed();
  EXPECT_TRUE(render(record).starts_with(
      "VerificationNotPerformed: the check could not be made -- a failure of "
      "the run, not a pass, and no finding about the hardware\n"
      "  what: oracle over compression units\n"
      "  why: no unit was sampled\n"
      "  blocks compared: 0\n  provenance:\n"));
  record.why.clear();
  record.blocks_compared = std::numeric_limits<std::uint64_t>::max();
  const std::string text = render(record);
  EXPECT_TRUE(contains(text, "  why: not given\n  blocks compared: 18446744073709551615\n"));
}

TEST(RecordRender, TextFromTheChannelIsEscaped) {
  VerificationNotPerformed record = contracts::record_samples::not_performed();
  record.what = "\x1b[2Jall clear";
  record.why = "line\nbreak";
  record.provenance.workload = "comp\rression";
  const std::string text = render(record);
  EXPECT_TRUE(contains(text, "  what: \\x1b[2Jall clear\n"));
  EXPECT_TRUE(contains(text, "  why: line\\x0abreak\n"));
  EXPECT_TRUE(contains(text, "    workload: comp\\x0dression\n"));
  ExactBitCorruption exact = contracts::record_samples::exact();
  exact.check = "\x07";
  EXPECT_TRUE(contains(render(exact), "  check: \\x07\n"));
  NumericalVerificationFailure bounded = contracts::record_samples::numerical();
  bounded.check = "\x07";
  EXPECT_TRUE(contains(render(bounded), "  check: \\x07\n"));
}

TEST(RecordRender, TheDutyCycleAtItsEdges) {
  VerificationNotPerformed record = contracts::record_samples::not_performed();
  record.provenance.duty = DutyCycle{0, 1};
  EXPECT_TRUE(contains(render(record),
                       "    duty cycle: oracle on no unit, input integrity on every unit\n"));
}

TEST(RecordRender, AWindowAtItsWidestAndTheOtherEnumerators) {
  ExactBitCorruption record = contracts::record_samples::exact(kMaxWindowBytes);
  record.provenance.isa = simd::IsaPath::kScalar;
  record.provenance.input_integrity = InputIntegrity::kNotChecked;
  record.difference->popcount = 2;
  record.difference->last_offset = 0x1007;
  const std::string text = render(record);
  EXPECT_TRUE(contains(text, "  classification: multiple bits in one word, with 128-byte"));
  EXPECT_TRUE(contains(text, "    ISA path: scalar\n    input integrity: not checked\n"));
  std::string zeros;
  for (std::size_t byte = 0; byte < kMaxWindowBytes; ++byte) {
    zeros += " 00";
  }
  EXPECT_TRUE(contains(text, "    expected" + zeros + "\n"));
}

}  // namespace
}  // namespace loadforge::verification
