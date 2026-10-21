// SPDX-License-Identifier: GPL-3.0-or-later
//
// T1 for the graph oracle: the expected output it recomputes, against a model
// of the specification written as a gather over explicit in-edge lists, on
// graphs of every awkward shape; and the plans it refuses.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

#include "oracle/verification/oracle/graph/graph_evidence.hpp"
#include "verification/oracle/graph/graph_oracle.hpp"
#include "workloads/graph/spec.hpp"

namespace loadforge::verification::oracle::graph {
namespace {

namespace spec = workloads::graph;

TEST(GraphLoadU32, ReadsLittleEndianWhateverTheHostIs) {
  const std::vector<std::byte> bytes = {std::byte{1},    std::byte{2},    std::byte{3},
                                        std::byte{4},    std::byte{0xFF}, std::byte{0xFF},
                                        std::byte{0xFF}, std::byte{0xFF}, std::byte{0},
                                        std::byte{0},    std::byte{0},    std::byte{0x80}};
  EXPECT_EQ(load_u32(bytes, 0), 0x04030201U);
  EXPECT_EQ(load_u32(bytes, 1), 0xFFFFFFFFU);
  EXPECT_EQ(load_u32(bytes, 2), 0x80000000U);
}

TEST(GraphScratch, TheDefaultFactoryGivesMemoryThatCanBeUsed) {
  const std::unique_ptr<Scratch> scratch = allocate_scratch();
  ASSERT_NE(scratch, nullptr);
  scratch->expected.front() = std::byte{1};
  scratch->expected.back() = std::byte{2};
  scratch->accumulators.back() = 3.0;
  EXPECT_EQ(scratch->expected.back(), std::byte{2});
}

struct Shape {
  const char* name;
  std::vector<std::uint32_t> heads;
  std::uint32_t source;
};

std::vector<Shape> shapes() {
  std::vector<Shape> out;
  const std::vector<std::uint32_t> generated = evidence::generated_numbers(evidence::kSeed, 1);
  out.push_back({"generated",
                 std::vector<std::uint32_t>(generated.begin(), generated.begin() + spec::kEdges),
                 generated[spec::kEdges]});
  out.push_back({"every edge to vertex 0", std::vector<std::uint32_t>(spec::kEdges, 0U), 0U});
  std::vector<std::uint32_t> loops(spec::kEdges);
  std::vector<std::uint32_t> ring(spec::kEdges);
  std::vector<std::uint32_t> beyond(spec::kEdges);
  for (std::size_t e = 0; e < spec::kEdges; ++e) {
    loops[e] = static_cast<std::uint32_t>(e / spec::kOutDegree);
    ring[e] = static_cast<std::uint32_t>((e / spec::kOutDegree + 1 + (e % 8)) % spec::kVertices);
    // Stored numbers at and beyond V, which the specification reduces modulo V.
    beyond[e] =
        static_cast<std::uint32_t>(e % 5 == 0 ? 0xFFFFFFFFU - e : 3 * spec::kVertices + (e % 977));
  }
  out.push_back({"self-loops only: nothing but the source is reached", loops, 17U});
  out.push_back({"a ring with chords: a long BFS", ring, 0U});
  out.push_back({"heads and source beyond V", beyond, 0x80000000U + 12345U});
  return out;
}

TEST(GraphExpectedOutput, IsTheModelsOnEveryShapeForEveryAccumulatorCount) {
  const std::unique_ptr<Scratch> scratch = allocate_scratch();
  ASSERT_NE(scratch, nullptr);
  for (const Shape& shape : shapes()) {
    const std::vector<std::byte> input = evidence::input_bytes(shape.heads, shape.source);
    for (const std::uint32_t accumulators : spec::kAccumulators) {
      compute_expected_output(input, accumulators, *scratch);
      const std::vector<std::byte> model =
          evidence::model_output(shape.heads, shape.source, accumulators);
      ASSERT_EQ(model.size(), spec::kOutputBytes);
      EXPECT_TRUE(std::equal(model.begin(), model.end(), scratch->expected.begin()))
          << shape.name << ", a = " << accumulators;
    }
  }
}

TEST(GraphExpectedOutput, HasTheShapesTheyWereChosenFor) {
  const std::unique_ptr<Scratch> scratch = allocate_scratch();
  ASSERT_NE(scratch, nullptr);
  const std::vector<Shape> all = shapes();

  // Self-loops: only the source is reached, and every vertex's rank is the
  // teleport term plus its own share.
  compute_expected_output(evidence::input_bytes(all[2].heads, all[2].source), 4, *scratch);
  const std::span<const std::byte> out(scratch->expected);
  EXPECT_EQ(load_u32(out, 17), 0U);
  EXPECT_EQ(load_u32(out, 16), spec::kUnreachable);
  EXPECT_EQ(load_u32(out, spec::kVertices - 1), spec::kUnreachable);

  // A source beyond V is reduced: 0x80000000 + 12345 mod 32768 is 12345.
  compute_expected_output(evidence::input_bytes(all[4].heads, all[4].source), 2, *scratch);
  EXPECT_EQ(load_u32(out, 12345), 0U);
}

TEST(GraphExpectedOutput, DependsOnTheAccumulatorsOnlyThroughTheRanks) {
  const std::vector<Shape> all = shapes();
  const std::vector<std::byte> input = evidence::input_bytes(all[0].heads, all[0].source);
  std::vector<std::vector<std::byte>> outputs;
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const std::unique_ptr<Scratch> scratch = allocate_scratch();
    ASSERT_NE(scratch, nullptr);
    compute_expected_output(input, accumulators, *scratch);
    outputs.emplace_back(scratch->expected.begin(), scratch->expected.end());
  }
  for (std::size_t which = 1; which < outputs.size(); ++which) {
    EXPECT_TRUE(std::equal(outputs[0].begin(), outputs[0].begin() + spec::kRanksOffset,
                           outputs[which].begin()))
        << "the distances are a property of the graph";
    EXPECT_FALSE(std::equal(outputs[0].begin() + spec::kRanksOffset, outputs[0].end(),
                            outputs[which].begin() + spec::kRanksOffset))
        << "a different summation order, different bits";
  }
}

TEST(GraphCreate, GivesTheOracleForEveryAccumulatorCount) {
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const workloads::Plan plan{spec::kInputBytes, spec::kOutputBytes, 0, 5, accumulators, 0, 0};
    auto created = create(workloads::WorkloadConfig{}, plan);
    ASSERT_TRUE(created.has_value());
    const auto oracle = std::move(created).value();
    EXPECT_EQ(oracle->workload(), "graph");
    EXPECT_EQ(oracle->contract(), Contract::kExact);
  }
}

TEST(GraphCreate, RefusesAPlanThatIsNotAUnitOfTheSpecification) {
  const auto refused = [](const workloads::Plan& plan) {
    auto created = create(workloads::WorkloadConfig{}, plan);
    EXPECT_FALSE(created.has_value());
    EXPECT_EQ(created.error().fault, OracleFault::kConfiguration);
    EXPECT_FALSE(created.error().detail.empty());
  };
  const workloads::Plan good{spec::kInputBytes, spec::kOutputBytes, 0, 1, 4, 0, 0};
  workloads::Plan plan = good;
  plan.input_bytes = spec::kInputBytes - 4;
  refused(plan);
  plan = good;
  plan.input_bytes = spec::kInputBytes + 4;
  refused(plan);
  plan = good;
  plan.output_bytes = spec::kOutputBytes - 8;
  refused(plan);
  plan = good;
  plan.output_bytes = 0;
  refused(plan);
  for (const std::uint32_t accumulators : {0U, 3U, 5U, 16U, 9U}) {
    plan = good;
    plan.decomposition = accumulators;
    refused(plan);
  }
}

}  // namespace
}  // namespace loadforge::verification::oracle::graph
