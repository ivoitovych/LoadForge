// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions src/verification/record.hpp carries: the names, the bit
// classification, the derived fields and the validity rules decode() applies.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "contracts/equality.hpp"
#include "contracts/verification/record_contract.hpp"
#include "verification/record.hpp"

namespace loadforge::verification {
namespace {

TEST(RecordDefinition, EveryEnumeratorHasItsWords) {
  EXPECT_EQ(describe(Contract::kExact), "exact");
  EXPECT_EQ(describe(Contract::kBounded), "bounded");
  EXPECT_EQ(describe(static_cast<Contract>(9)), "unrecognised contract");
  EXPECT_EQ(describe(Side::kOutput), "output");
  EXPECT_EQ(describe(Side::kInput), "input");
  EXPECT_EQ(describe(static_cast<Side>(9)), "unrecognised side");
  EXPECT_EQ(describe(InputIntegrity::kIntact), "intact");
  EXPECT_EQ(describe(InputIntegrity::kCorruptBeforeConsumption), "corrupt before consumption");
  EXPECT_EQ(describe(InputIntegrity::kCorruptDuringConsumption), "corrupt during consumption");
  EXPECT_EQ(describe(InputIntegrity::kNotChecked), "not checked");
  EXPECT_EQ(describe(static_cast<InputIntegrity>(9)), "unrecognised input integrity");
  EXPECT_EQ(describe(BitClass::kSingleBit), "single bit");
  EXPECT_EQ(describe(BitClass::kMultiBitInWord), "multiple bits in one word");
  EXPECT_EQ(describe(BitClass::kWithinCacheLine), "within one cache line");
  EXPECT_EQ(describe(BitClass::kAcrossCacheLines), "across cache lines");
  EXPECT_EQ(describe(static_cast<BitClass>(9)), "unrecognised bit classification");
  EXPECT_EQ(describe(Domain::kStored), "stored bytes");
  EXPECT_EQ(describe(Domain::kDerived), "derived bytes");
  EXPECT_EQ(describe(static_cast<Domain>(9)), "unrecognised domain");
  EXPECT_EQ(describe(DecodeFault::kEmpty), "empty");
  EXPECT_EQ(describe(DecodeFault::kVersion), "unknown format version");
  EXPECT_EQ(describe(DecodeFault::kKind), "unknown record kind");
  EXPECT_EQ(describe(DecodeFault::kTruncated), "truncated");
  EXPECT_EQ(describe(DecodeFault::kTrailing), "trailing bytes");
  EXPECT_EQ(describe(DecodeFault::kInvalid), "invalid record");
  EXPECT_EQ(describe(static_cast<DecodeFault>(9)), "unrecognised decode fault");
}

TEST(RecordDefinition, TheBitClassificationAtEachBoundary) {
  EXPECT_EQ(classify_bits(1, 5, 5, 64), BitClass::kSingleBit);
  EXPECT_EQ(classify_bits(2, 5, 5, 64), BitClass::kMultiBitInWord) << "two bits, one byte";
  EXPECT_EQ(classify_bits(2, 0, 7, 64), BitClass::kMultiBitInWord)
      << "first and last byte of a word";
  EXPECT_EQ(classify_bits(2, 7, 8, 64), BitClass::kWithinCacheLine) << "adjacent bytes, two words";
  EXPECT_EQ(classify_bits(2, 0, 63, 64), BitClass::kWithinCacheLine);
  EXPECT_EQ(classify_bits(2, 63, 64, 64), BitClass::kAcrossCacheLines)
      << "adjacent bytes, two lines";
  EXPECT_EQ(classify_bits(2, 63, 64, 128), BitClass::kWithinCacheLine)
      << "the line size is the topology's, and changes the answer";
}

TEST(RecordDefinition, DerivedFieldsComeFromTheRecordsOwnBytes) {
  ExactBitCorruption record = contracts::record_samples::exact();
  EXPECT_EQ(classify(record), BitClass::kSingleBit);
  record.difference->popcount = 3;
  record.difference->last_offset = 0x1040;
  EXPECT_EQ(classify(record), BitClass::kWithinCacheLine) << "the record's own line size, 128";
  const std::vector<std::byte> delta = xor_delta(*record.difference);
  ASSERT_EQ(delta.size(), 8U);
  EXPECT_EQ(delta[3], std::byte{0x10});
  EXPECT_EQ(delta[0], std::byte{0x00});
  Difference mismatched = *record.difference;
  mismatched.actual.pop_back();
  EXPECT_TRUE(xor_delta(mismatched).empty()) << "no delta for windows of different lengths";
  record.difference.reset();
  EXPECT_EQ(classify(record), std::nullopt) << "no difference, no classification -- not a zero";

  NumericalVerificationFailure bounded = contracts::record_samples::numerical();
  bounded.residual = 3.0;
  bounded.tolerance = 2.0;
  EXPECT_EQ(ratio(bounded), 1.5);
}

TEST(RecordDefinition, KindNamesProvenanceAndWhichKindsAreWrongResults) {
  const ErrorRecord exact = contracts::record_samples::exact();
  const ErrorRecord bounded = contracts::record_samples::numerical();
  const ErrorRecord unrun = contracts::record_samples::not_performed();
  EXPECT_EQ(kind_name(exact), "ExactBitCorruption");
  EXPECT_EQ(kind_name(bounded), "NumericalVerificationFailure");
  EXPECT_EQ(kind_name(unrun), "VerificationNotPerformed");
  EXPECT_EQ(provenance_of(exact).logical_cpu, 11U);
  EXPECT_EQ(provenance_of(bounded).seed, 0xFEEDFACECAFEBEEFULL);
  EXPECT_FALSE(provenance_of(unrun).buffer_address.has_value())
      << "a check that never ran has no buffer, and says so";
  EXPECT_TRUE(is_wrong_result(exact));
  EXPECT_TRUE(is_wrong_result(bounded));
  EXPECT_FALSE(is_wrong_result(unrun));
}

TEST(RecordDefinition, ValidityRulesOneAtATime) {
  for (const ErrorRecord& record : contracts::record_samples::valid_samples()) {
    EXPECT_TRUE(is_valid(record)) << kind_name(record);
  }
  for (const ErrorRecord& record : contracts::record_samples::invalid_samples()) {
    EXPECT_FALSE(is_valid(record)) << kind_name(record);
  }
}

TEST(RecordEquality, EveryMemberIsCompared) {
  using contracts::expect_memberwise_equality;
  expect_memberwise_equality(DutyCycle{1, 2}, {[](DutyCycle& d) { d.oracle_period = 3; },
                                               [](DutyCycle& d) { d.integrity_period = 3; }});
  expect_memberwise_equality(
      contracts::record_samples::full_provenance(),
      {[](Provenance& p) { p.logical_cpu = 1; }, [](Provenance& p) { p.physical_core = 1; },
       [](Provenance& p) { p.numa_node = 0; }, [](Provenance& p) { p.buffer_address = 1; },
       [](Provenance& p) { p.buffer_offset = 1; }, [](Provenance& p) { p.iteration = 1; },
       [](Provenance& p) { p.workload = "sort"; }, [](Provenance& p) { p.workload_version = 1; },
       [](Provenance& p) { p.seed = 1; }, [](Provenance& p) { p.isa = simd::IsaPath::kScalar; },
       [](Provenance& p) { p.input_integrity = InputIntegrity::kIntact; },
       [](Provenance& p) { p.duty = DutyCycle{1, 1}; },
       [](Provenance& p) { p.physical_package = 0; }, [](Provenance& p) { p.decomposition = 1; },
       [](Provenance& p) { p.footprint_bytes = 1; }});
  expect_memberwise_equality(
      contracts::record_samples::difference(),
      {[](Difference& d) { d.domain = Domain::kDerived; },
       [](Difference& d) { d.first_offset = 0; }, [](Difference& d) { d.last_offset = 0x2000; },
       [](Difference& d) { d.popcount = 2; }, [](Difference& d) { d.window_offset = 0; },
       [](Difference& d) { d.expected[0] = std::byte{1}; },
       [](Difference& d) { d.actual[0] = std::byte{1}; }});
  expect_memberwise_equality(contracts::record_samples::exact(),
                             {[](ExactBitCorruption& r) { r.provenance.seed.reset(); },
                              [](ExactBitCorruption& r) { r.side = Side::kOutput; },
                              [](ExactBitCorruption& r) { r.check = "other"; },
                              [](ExactBitCorruption& r) { r.difference.reset(); },
                              [](ExactBitCorruption& r) { r.line_bytes = 64; }});
  expect_memberwise_equality(contracts::record_samples::numerical(),
                             {[](NumericalVerificationFailure& r) { r.provenance.seed.reset(); },
                              [](NumericalVerificationFailure& r) { r.check = "other"; },
                              [](NumericalVerificationFailure& r) { r.residual = 1.0; },
                              [](NumericalVerificationFailure& r) { r.tolerance = 1.0; },
                              [](NumericalVerificationFailure& r) { r.condition_number.reset(); },
                              [](NumericalVerificationFailure& r) { r.element = 1; }});
  expect_memberwise_equality(contracts::record_samples::not_performed(),
                             {[](VerificationNotPerformed& r) { r.provenance.seed = 1; },
                              [](VerificationNotPerformed& r) { r.what = "other"; },
                              [](VerificationNotPerformed& r) { r.why = "other"; },
                              [](VerificationNotPerformed& r) { r.blocks_compared = 1; }});
  expect_memberwise_equality(DecodeError{DecodeFault::kEmpty, "detail"},
                             {[](DecodeError& e) { e.fault = DecodeFault::kKind; },
                              [](DecodeError& e) { e.detail = "other"; }});
}

}  // namespace
}  // namespace loadforge::verification
