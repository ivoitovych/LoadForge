// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b for the graph oracle. T5: corruption is introduced into the
// output of a unit and the verdict is asserted for which section it names and
// for the difference it carries -- first and last differing byte, the bits
// over the whole comparison, the window -- so that the record model classifies
// the corruption rightly. T5b: the check is made impossible, and the verdict
// must be NotChecked with nothing compared, never an agreement.

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "oracle/verification/oracle/graph/graph_evidence.hpp"
#include "verification/oracle/graph/graph_oracle.hpp"
#include "workloads/graph/spec.hpp"

namespace loadforge::verification::oracle::graph {
namespace {

namespace spec = workloads::graph;
using evidence::Unit;

constexpr std::uint32_t kAccumulators = 4;

const Unit& good_unit() {
  static const Unit unit = evidence::real_tool_unit(kAccumulators);
  return unit;
}

std::unique_ptr<Oracle> make_oracle() {
  auto created =
      create(workloads::WorkloadConfig{},
             workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, kAccumulators, 0, 0});
  return std::move(created).value();
}

Verdict check_unit(const Unit& unit) {
  const auto oracle = make_oracle();
  return oracle->check(UnitEvidence{unit.index, unit.input, unit.output});
}

ExactMismatch mismatch_of(const Verdict& verdict) {
  EXPECT_TRUE(std::holds_alternative<ExactMismatch>(verdict));
  EXPECT_TRUE(is_valid(verdict, Contract::kExact));
  return std::get<ExactMismatch>(verdict);
}

BitClass class_of(const ExactMismatch& mismatch) {
  EXPECT_TRUE(mismatch.difference.has_value());
  const Difference& difference = *mismatch.difference;
  return classify_bits(difference.popcount, difference.first_offset, difference.last_offset, 64);
}

TEST(GraphFaultInjection, TheUncorruptedUnitIsAgreedWith) {
  const Verdict verdict = check_unit(good_unit());
  ASSERT_TRUE(std::holds_alternative<Agreement>(verdict));
  EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kSections);
}

TEST(GraphFaultInjection, ASingleBitIsClassifiedAsASingleBitInTheSectionItIsIn) {
  struct Place {
    std::size_t offset;
    const char* section;
  };
  for (const Place place :
       {Place{0, "bfs-distances"}, Place{77, "bfs-distances"},
        Place{spec::kRanksOffset - 1, "bfs-distances"}, Place{spec::kRanksOffset, "pagerank"},
        Place{spec::kRanksOffset + 4097, "pagerank"}, Place{spec::kOutputBytes - 1, "pagerank"}}) {
    for (const unsigned bit : {0U, 7U}) {
      Unit unit = good_unit();
      unit.output[place.offset] ^= static_cast<std::byte>(1U << bit);
      const ExactMismatch found = mismatch_of(check_unit(unit));
      EXPECT_EQ(found.check, place.section) << "offset " << place.offset;
      EXPECT_EQ(found.blocks_compared, spec::kSections);
      ASSERT_TRUE(found.difference.has_value());
      EXPECT_EQ(found.difference->domain, Domain::kStored);
      EXPECT_EQ(found.difference->first_offset, place.offset);
      EXPECT_EQ(found.difference->last_offset, place.offset);
      EXPECT_EQ(found.difference->popcount, 1U);
      EXPECT_EQ(class_of(found), BitClass::kSingleBit);
    }
  }
}

TEST(GraphFaultInjection, ABitInTheLastByteGetsAWindowOfTheEightBytesThatRemain) {
  Unit unit = good_unit();
  unit.output[spec::kOutputBytes - 1] ^= std::byte{0x10};
  const ExactMismatch found = mismatch_of(check_unit(unit));
  ASSERT_TRUE(found.difference.has_value());
  EXPECT_EQ(found.difference->window_offset, spec::kOutputBytes - 8);
  EXPECT_EQ(found.difference->expected.size(), 8U);
  EXPECT_EQ(found.difference->actual.size(), 8U);
  EXPECT_EQ(std::to_integer<unsigned>(found.difference->expected.back() ^
                                      found.difference->actual.back()),
            0x10U);
}

TEST(GraphFaultInjection, SeveralBitsInOneWordAreMultiBitInWord) {
  Unit unit = good_unit();
  unit.output[spec::kRanksOffset + 800] ^= std::byte{0x03};
  unit.output[spec::kRanksOffset + 803] ^= std::byte{0x80};
  const ExactMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "pagerank");
  EXPECT_EQ(found.difference->popcount, 3U);
  EXPECT_EQ(class_of(found), BitClass::kMultiBitInWord);
}

TEST(GraphFaultInjection, BitsInTwoWordsOfOneLineAreWithinACacheLine) {
  Unit unit = good_unit();
  unit.output[spec::kRanksOffset + 64] ^= std::byte{0x01};
  unit.output[spec::kRanksOffset + 64 + 40] ^= std::byte{0x01};
  EXPECT_EQ(class_of(mismatch_of(check_unit(unit))), BitClass::kWithinCacheLine);
}

TEST(GraphFaultInjection, BitsInTwoLinesAreAcrossCacheLinesAndTheWindowCoversTheFirstOnly) {
  Unit unit = good_unit();
  unit.output[100] ^= std::byte{0x01};
  unit.output[100 + 1000] ^= std::byte{0xFF};
  const ExactMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(class_of(found), BitClass::kAcrossCacheLines);
  EXPECT_EQ(found.difference->first_offset, 100U);
  EXPECT_EQ(found.difference->last_offset, 1100U);
  EXPECT_EQ(found.difference->popcount, 9U) << "over the whole comparison, not the window";
  EXPECT_EQ(found.difference->window_offset, 96U);
  EXPECT_EQ(found.difference->expected.size(), kMaxWindowBytes);
  EXPECT_EQ(found.difference->actual.size(), kMaxWindowBytes);
}

TEST(GraphFaultInjection, WhenBothSectionsAreWrongTheDistancesAreNamed) {
  Unit unit = good_unit();
  unit.output[spec::kRanksOffset + 10] ^= std::byte{0x01};
  unit.output[40] ^= std::byte{0x01};
  const ExactMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "bfs-distances");
  EXPECT_EQ(found.difference->first_offset, 40U);
  EXPECT_EQ(found.difference->last_offset, spec::kRanksOffset + 10);
  EXPECT_EQ(found.difference->popcount, 2U);
}

TEST(GraphFaultInjection, ADistanceOneTooLargeAndAOneUlpRankAreCaught) {
  // Exact is exact: the smallest change a value can have is a mismatch, where
  // a bounded check would let it pass.
  Unit unit = good_unit();
  const std::uint32_t distance = load_u32(unit.output, 500);
  unit.output[std::size_t{500} * 4] = static_cast<std::byte>(distance + 1);
  EXPECT_EQ(mismatch_of(check_unit(unit)).check, "bfs-distances");

  unit = good_unit();
  unit.output[spec::kRanksOffset + std::size_t{8} * 321] ^= std::byte{0x01};
  EXPECT_EQ(mismatch_of(check_unit(unit)).check, "pagerank");
}

TEST(GraphFaultInjection, AVertexMarkedUnreachableThatIsReachedIsCaught) {
  Unit unit = good_unit();
  for (std::size_t byte = 0; byte < 4; ++byte) {
    unit.output[std::size_t{300} * 4 + byte] = std::byte{0xFF};
  }
  EXPECT_EQ(mismatch_of(check_unit(unit)).check, "bfs-distances");
}

TEST(GraphFaultInjection, ANaNOrGarbageWhereARankBelongsIsAMismatchNotAnUnrunCheck) {
  for (const double bad :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -0.0}) {
    Unit unit = good_unit();
    std::vector<std::byte> encoded;
    evidence::append_double(encoded, bad);
    std::copy(encoded.begin(), encoded.end(),
              unit.output.begin() +
                  static_cast<std::ptrdiff_t>(spec::kRanksOffset + std::size_t{8} * 77));
    const ExactMismatch found = mismatch_of(check_unit(unit));
    EXPECT_EQ(found.check, "pagerank");
  }
}

TEST(GraphFaultInjection, ACorruptedInputIsJudgedByTheOutputItDidNotProduce) {
  // The oracle judges the output against the input it is given (oracle.hpp).
  // One head of the INPUT moves, the output stays: a mismatch of the output;
  // the runner's bracket says the input was the side that changed.
  Unit unit = good_unit();
  unit.input[std::size_t{100} * 4] ^= std::byte{0x01};
  const Verdict verdict = check_unit(unit);
  EXPECT_TRUE(std::holds_alternative<ExactMismatch>(verdict));
}

TEST(GraphFaultInjection, ACorruptedSourceMovesTheWholeDistanceSection) {
  Unit unit = good_unit();
  unit.input[spec::kEdges * 4] ^= std::byte{0x02};
  const ExactMismatch found = mismatch_of(check_unit(unit));
  EXPECT_EQ(found.check, "bfs-distances");
}

TEST(GraphFaultInjection, AnInputBeyondTheVertexRangeIsComputedOnLikeAnyOther) {
  // Totality (spec.hpp): stored numbers beyond V are reduced modulo V by
  // workload and oracle alike, so an output the specification gives for such
  // an input is agreed with.
  std::vector<std::uint32_t> heads(spec::kEdges);
  for (std::size_t e = 0; e < spec::kEdges; ++e) {
    heads[e] = static_cast<std::uint32_t>(0xFFFF0000U + (e * 7919U) % 65536U);
  }
  const std::uint32_t source = 0xFFFFFFFFU;
  Unit unit{3, evidence::input_bytes(heads, source),
            evidence::model_output(heads, source, kAccumulators)};
  EXPECT_TRUE(std::holds_alternative<Agreement>(check_unit(unit)));
  unit.output[spec::kRanksOffset + 9] ^= std::byte{0x01};
  EXPECT_TRUE(std::holds_alternative<ExactMismatch>(check_unit(unit)));
}

// T5b ------------------------------------------------------------------------

void expect_not_checked(const Verdict& verdict) {
  ASSERT_TRUE(std::holds_alternative<NotChecked>(verdict))
      << "an unrun check must never be reported as anything else";
  const auto& unchecked = std::get<NotChecked>(verdict);
  EXPECT_EQ(unchecked.blocks_compared, 0U);
  EXPECT_FALSE(unchecked.what.empty());
  EXPECT_FALSE(unchecked.why.empty());
  EXPECT_TRUE(is_valid(verdict, Contract::kExact));
}

/// How many times a factory below was asked for memory.
int& allocations_asked() {
  static int count = 0;
  return count;
}

std::unique_ptr<Scratch> refusing_factory() noexcept {
  ++allocations_asked();
  return nullptr;
}

std::unique_ptr<Scratch> counting_factory() noexcept {
  ++allocations_asked();
  return allocate_scratch();
}

TEST(GraphNotPerformed, EvidenceOfTheWrongSizeIsNotChecked) {
  std::vector<Unit> units(8, good_unit());
  units[0].input.pop_back();
  units[1].input.push_back(std::byte{0});
  units[2].input.clear();
  units[3].output.pop_back();
  units[4].output.push_back(std::byte{0});
  units[5].output.clear();
  units[6].input.resize(spec::kInputBytes / 2);
  units[7].output.resize(spec::kOutputBytes * 2);
  allocations_asked() = 0;
  const GraphOracle oracle(kAccumulators, &counting_factory);
  for (const Unit& unit : units) {
    expect_not_checked(oracle.check(UnitEvidence{unit.index, unit.input, unit.output}));
  }
  EXPECT_EQ(allocations_asked(), 0) << "no memory is taken for evidence that cannot be checked";
}

TEST(GraphNotPerformed, MemoryThatCannotBeAllocatedMakesTheCheckNotPerformedNeverAnAgreement) {
  allocations_asked() = 0;
  const GraphOracle oracle(kAccumulators, &refusing_factory);
  const Unit& unit = good_unit();
  const Verdict verdict = oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
  expect_not_checked(verdict);
  EXPECT_EQ(allocations_asked(), 1);
  // The same refusal for an output that is wrong: still nothing was compared.
  Unit wrong = good_unit();
  wrong.output[5] ^= std::byte{1};
  expect_not_checked(oracle.check(UnitEvidence{wrong.index, wrong.input, wrong.output}));
}

TEST(GraphNotPerformed, TheFactoryIsAskedOncePerCheck) {
  allocations_asked() = 0;
  const GraphOracle oracle(kAccumulators, &counting_factory);
  const Unit& unit = good_unit();
  for (int round = 1; round <= 3; ++round) {
    EXPECT_TRUE(std::holds_alternative<Agreement>(
        oracle.check(UnitEvidence{unit.index, unit.input, unit.output})));
    EXPECT_EQ(allocations_asked(), round);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::graph
