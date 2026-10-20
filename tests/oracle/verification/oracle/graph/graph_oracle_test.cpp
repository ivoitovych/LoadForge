// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3 for the graph oracle: scipy's distances and the specification's ranks on a
// generated unit are agreed with for the accumulator count they were made for,
// a corrupted output is not, and an output made for another count is not,
// because the contract is exact and the summation order is part of the answer.

#include "verification/oracle/graph/graph_oracle.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "contracts/verification/oracle_contract.hpp"
#include "oracle/verification/oracle/graph/graph_evidence.hpp"
#include "workloads/graph/spec.hpp"

namespace loadforge::verification::oracle::graph {
namespace {

namespace spec = workloads::graph;
using evidence::Unit;

std::unique_ptr<Oracle> make_oracle(std::uint32_t accumulators) {
  auto created =
      create(workloads::WorkloadConfig{},
             workloads::Plan{spec::kInputBytes, spec::kOutputBytes, 0, 1, accumulators, 0, 0});
  return std::move(created).value();
}

contracts::OracleCase as_case(const Unit& unit) {
  return contracts::OracleCase{unit.index, unit.input, unit.output};
}

Verdict check_unit(const Oracle& oracle, const Unit& unit) {
  return oracle.check(UnitEvidence{unit.index, unit.input, unit.output});
}

std::vector<contracts::OracleCase> real_tool_cases(std::uint32_t accumulators) {
  return {as_case(evidence::real_tool_unit(accumulators))};
}

// A flip in the distances, in the ranks, and in both.
std::vector<contracts::OracleCase> corrupted_cases(std::uint32_t accumulators) {
  const contracts::OracleCase base = as_case(evidence::real_tool_unit(accumulators));
  std::vector<contracts::OracleCase> cases;
  contracts::OracleCase distance = base;
  distance.output[1234] ^= std::byte{0x04};
  cases.push_back(distance);
  contracts::OracleCase rank = base;
  rank.output[spec::kRanksOffset + 5000] ^= std::byte{0x01};
  cases.push_back(rank);
  contracts::OracleCase both = base;
  both.output[3] ^= std::byte{0x80};
  both.output[spec::kOutputBytes - 1] ^= std::byte{0x01};
  cases.push_back(both);
  return cases;
}

TEST(GraphOracleRealTool, TheFixtureIsAgreedWithForItsAccumulatorsAndOnlyForThem) {
  for (const std::uint32_t made_for : spec::kAccumulators) {
    const Unit unit = evidence::real_tool_unit(made_for);
    for (const std::uint32_t judged_by : spec::kAccumulators) {
      const Verdict verdict = check_unit(*make_oracle(judged_by), unit);
      if (made_for == judged_by) {
        ASSERT_TRUE(std::holds_alternative<Agreement>(verdict)) << "a = " << made_for;
        EXPECT_EQ(std::get<Agreement>(verdict).blocks_compared, spec::kSections);
      } else {
        // The distances are the same; the ranks differ in their bits, because
        // the order of the sums is part of what the specification pins.
        ASSERT_TRUE(std::holds_alternative<ExactMismatch>(verdict))
            << "made for " << made_for << ", judged by " << judged_by;
        const auto& mismatch = std::get<ExactMismatch>(verdict);
        EXPECT_EQ(mismatch.check, "pagerank");
        ASSERT_TRUE(mismatch.difference.has_value());
        EXPECT_GE(mismatch.difference->first_offset, spec::kRanksOffset);
      }
    }
  }
}

TEST(GraphOracleRealTool, ThreeReadingsOfTheSpecificationProduceTheSameBytes) {
  // The Python model of the ranks, scipy's distances, and a gather over explicit
  // in-edge lists written in C++: the fixture and the test-local model agree to
  // the bit, and the oracle agrees with the fixture above.
  const std::vector<std::uint32_t> numbers =
      evidence::generated_numbers(evidence::kSeed, evidence::kFixtureIndex);
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const std::vector<std::byte> model =
        evidence::model_output(std::span<const std::uint32_t>(numbers).first(spec::kEdges),
                               numbers[spec::kEdges], accumulators);
    EXPECT_EQ(model, evidence::real_tool_unit(accumulators).output) << "a = " << accumulators;
  }
}

TEST(GraphOracleRealTool, TheFixtureHasReachableAndUnreachableVertices) {
  // A random graph of out-degree 8 reaches almost every vertex; the test is
  // that distances beyond zero exist, so the BFS did something.
  const Unit unit = evidence::real_tool_unit(4);
  std::uint32_t deepest = 0;
  std::size_t reached = 0;
  for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
    const std::uint32_t distance = load_u32(unit.output, vertex);
    if (distance != spec::kUnreachable) {
      ++reached;
      deepest = std::max(deepest, distance);
    }
  }
  EXPECT_GT(reached, spec::kVertices * 9 / 10);
  EXPECT_GE(deepest, 3U);
}

TEST(GraphOracleExact, AMismatchCarriesTheDifferenceTheRecordModelDescribes) {
  const Unit good = evidence::real_tool_unit(4);
  const auto oracle = make_oracle(4);

  Unit one_bit = good;
  one_bit.output[1234] ^= std::byte{0x04};
  const Verdict verdict = check_unit(*oracle, one_bit);
  ASSERT_TRUE(std::holds_alternative<ExactMismatch>(verdict));
  const auto& mismatch = std::get<ExactMismatch>(verdict);
  EXPECT_EQ(mismatch.check, "bfs-distances");
  EXPECT_EQ(mismatch.blocks_compared, spec::kSections);
  ASSERT_TRUE(mismatch.difference.has_value());
  const Difference& difference = *mismatch.difference;
  EXPECT_EQ(difference.domain, Domain::kStored);
  EXPECT_EQ(difference.first_offset, 1234U);
  EXPECT_EQ(difference.last_offset, 1234U);
  EXPECT_EQ(difference.popcount, 1U);
  EXPECT_EQ(difference.window_offset, 1232U);
  ASSERT_EQ(difference.expected.size(), kMaxWindowBytes);
  ASSERT_EQ(difference.actual.size(), kMaxWindowBytes);
  for (std::size_t at = 0; at < kMaxWindowBytes; ++at) {
    EXPECT_EQ(difference.expected[at], good.output[1232 + at]);
    EXPECT_EQ(difference.actual[at], one_bit.output[1232 + at]);
  }
  EXPECT_EQ(classify_bits(difference.popcount, difference.first_offset, difference.last_offset, 64),
            BitClass::kSingleBit);
  EXPECT_TRUE(is_valid(verdict, Contract::kExact));
}

TEST(GraphOracleRealTool, TheInputIsGeneratedByTheContractsPhilox) {
  // The oracle agrees with the fixture, which Python made from its own Philox;
  // this says the C++ input is the one the Python graph was built from: the
  // fixture's distances, from scipy, are the distances of THIS input's graph.
  const Unit unit = evidence::real_tool_unit(4);
  const std::uint32_t source = load_u32(unit.input, spec::kEdges);
  EXPECT_EQ(load_u32(unit.output, source), 0U) << "the source is at distance zero";
  for (std::size_t edge = 0; edge < 2000; ++edge) {
    const std::uint32_t tail = spec::tail_of(edge);
    const std::uint32_t head = load_u32(unit.input, edge);
    const std::uint32_t d_tail = load_u32(unit.output, tail);
    const std::uint32_t d_head = load_u32(unit.output, head);
    if (d_tail != spec::kUnreachable) {
      EXPECT_LE(d_head, d_tail + 1) << "the triangle inequality, edge " << edge;
    }
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::graph

// The suites are instantiated where they are declared: the macros name them
// unqualified.
namespace loadforge::contracts {
namespace {

namespace under_test = verification::oracle::graph;

// This binary instantiates the oracle suite only.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OracleRegistryContract);

// One decomposition runs the generic suite: every check of the graph oracle is
// a recomputation of four megabytes, and the suite makes about thirty of them,
// four at once, on the same counters. The other decompositions are run against
// the same fixtures, one check apiece, by the tests above.
struct GraphSubject {
  static constexpr const char* kWorkload = "graph";
  static constexpr verification::Contract kContract = verification::Contract::kExact;
  static std::unique_ptr<verification::Oracle> make() { return under_test::make_oracle(4); }
  static std::vector<OracleCase> good() { return under_test::real_tool_cases(4); }
  static std::vector<OracleCase> wrong() { return under_test::corrupted_cases(4); }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Graph, OracleContract, ::testing::Types<GraphSubject>);

}  // namespace
}  // namespace loadforge::contracts
