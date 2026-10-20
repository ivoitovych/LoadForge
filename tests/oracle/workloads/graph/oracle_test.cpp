// SPDX-License-Identifier: GPL-3.0-or-later
//
// T3: the graph workload against an independent reference written from the
// specification (graph_support.hpp): a plain queue BFS and a PageRank that
// gathers each vertex's in-edges and accumulates term by term. The check is
// exact -- the specification has no tolerance -- over the whole output, for
// every decomposition and every ISA path this machine runs, on generated
// units, on hand-built graphs whose answer is known without either program,
// and on corrupt input.

#include <gtest/gtest.h>

#include <limits>

#include "graph_support.hpp"

namespace loadforge::graph_test {
namespace {

constexpr std::uint32_t kUnreached = 0xFFFFFFFFU;

Bytes generated(std::uint64_t seed, std::uint64_t index) {
  Bytes input(spec::kInputBytes);
  make(config_for(0, simd::IsaPath::kScalar, seed))->generate(index, input);
  return input;
}

void expect_equals_reference(const Bytes& input, const char* what) {
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const Bytes expected = reference_output(input, accumulators);
    for (const simd::IsaPath path : runnable_paths()) {
      const auto workload = make(config_for(accumulators, path));
      ASSERT_NE(workload, nullptr);
      EXPECT_EQ(run_serial(*workload, input), expected)
          << what << ", " << accumulators << " accumulators, " << simd::describe(path);
    }
  }
}

TEST(GraphOracle, GeneratedUnitsMatchTheReferenceBitForBit) {
  for (const auto& [seed, index] : {std::pair<std::uint64_t, std::uint64_t>{1, 0}, {1, 1}, {99, 4}}) {
    expect_equals_reference(generated(seed, index), "generated");
  }
}

TEST(GraphOracle, TheReferenceInputIsWhatTheWorkloadGenerates) {
  EXPECT_EQ(generated(5, 3), reference_input(5, 3));
}

TEST(GraphOracle, AParallelRunMatchesTheReferenceToo) {
  const Bytes input = generated(3, 2);
  const Bytes expected = reference_output(input, 4);
  const auto workload = make(config_for(4, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  PullExecutor pull(4);
  EXPECT_EQ(run_compute(*workload, input, pull), expected);
  PartitionExecutor partition(3);
  EXPECT_EQ(run_compute(*workload, input, partition), expected);
}

// ---- graphs whose answer is known without either implementation ------------------

TEST(GraphOracle, ARingHasDistanceEqualToTheStepsAroundIt) {
  // Every edge of vertex v runs to v + 1, and the source is 100.
  const Bytes input = input_of(
      [](std::size_t edge) { return static_cast<std::uint32_t>((edge / 8 + 1) % spec::kVertices); },
      100);
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Bytes output = run_serial(*workload, input);
  const std::vector<std::uint32_t> distance = distances_of(output);
  for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
    EXPECT_EQ(distance[vertex], (vertex + spec::kVertices - 100) % spec::kVertices) << vertex;
  }
  const std::vector<double> rank = ranks_of(output);
  EXPECT_EQ(*std::ranges::min_element(rank), *std::ranges::max_element(rank))
      << "the ring is symmetric: every vertex has the same rank, to the bit";
  expect_equals_reference(input, "ring");
}

TEST(GraphOracle, AStarReachesEverythingInOneStepAndTheHubOutranksTheRest) {
  // Vertex 0 has an edge to 0 .. 7; every other vertex's edges all run to 0;
  // the source is a leaf (1234). Vertices 8.. have no in-edges at all.
  const Bytes input = input_of(
      [](std::size_t edge) {
        return edge / 8 == 0 ? static_cast<std::uint32_t>(edge % 8) : std::uint32_t{0};
      },
      1234);
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Bytes output = run_serial(*workload, input);
  const std::vector<std::uint32_t> distance = distances_of(output);
  EXPECT_EQ(distance[1234], 0U);
  EXPECT_EQ(distance[0], 1U);
  for (std::size_t vertex = 1; vertex < 8; ++vertex) {
    EXPECT_EQ(distance[vertex], 2U) << vertex;
  }
  EXPECT_EQ(distance[9], kUnreached) << "no edge leads to 9";
  EXPECT_EQ(distance[spec::kVertices - 1], kUnreached);
  const std::vector<double> rank = ranks_of(output);
  EXPECT_GT(rank[0], rank[1]);
  EXPECT_EQ(rank[9], spec::kTeleport) << "no in-edges: only the teleport term, exactly";
  expect_equals_reference(input, "star");
}

TEST(GraphOracle, EveryEdgeASelfLoopLeavesEverythingButTheSourceUnreachable) {
  const Bytes input = input_of(
      [](std::size_t edge) { return static_cast<std::uint32_t>(edge / 8); }, 77);
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  const Bytes output = run_serial(*workload, input);
  const std::vector<std::uint32_t> distance = distances_of(output);
  for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
    EXPECT_EQ(distance[vertex], vertex == 77 ? 0U : kUnreached) << vertex;
  }
  const std::vector<double> rank = ranks_of(output);
  EXPECT_EQ(rank[3], rank[4]);
  expect_equals_reference(input, "self-loops");
}

TEST(GraphOracle, APathOfThirtyTwoThousandLevelsIsComputedAndIsBounded) {
  // Edge e runs to tail + 1: the deepest BFS the format allows.
  const Bytes input = input_of(
      [](std::size_t edge) { return static_cast<std::uint32_t>((edge / 8 + 1) % spec::kVertices); },
      0);
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  SerialExecutor executor;
  const Bytes output = run_compute(*workload, input, executor);
  const std::vector<std::uint32_t> distance = distances_of(output);
  EXPECT_EQ(distance[spec::kVertices - 1], spec::kVertices - 1);
  EXPECT_LT(executor.runs, 16U + 64U) << "a chain's levels must not each cost an executor round trip";
}

TEST(GraphOracle, AVertexNumberNotBelowVIsReducedModuloV) {
  Bytes input = generated(8, 8);
  Bytes reduced = input;
  for (std::size_t at = 0; at <= spec::kEdges; at += 3) {
    set_word(input, at, word(input, at) + 32768U * static_cast<std::uint32_t>(1 + at % 7));
    // The reduction is a u32 modulo; the largest multiple stays in range.
    ASSERT_GE(word(input, at), 32768U);
  }
  set_word(input, 1, 0xFFFFFFFFU);
  set_word(reduced, 1, 0xFFFFFFFFU % 32768U);
  set_word(input, spec::kEdges, 0xFFFFFFFFU);
  set_word(reduced, spec::kEdges, 0xFFFFFFFFU % 32768U);
  const auto workload = make(config_for(0, simd::IsaPath::kScalar));
  ASSERT_NE(workload, nullptr);
  EXPECT_EQ(run_serial(*workload, input), run_serial(*workload, reduced))
      << "the reduced input and the raw one are the same graph";
  expect_equals_reference(input, "unreduced words");
}

TEST(GraphOracle, EveryByteOfEveryPatternIsAnInputWithAnExactAnswer) {
  for (const unsigned fill : {0x00U, 0xFFU, 0x7FU, 0x80U}) {
    const Bytes input(spec::kInputBytes, static_cast<std::byte>(fill));
    expect_equals_reference(input, "constant bytes");
  }
}

TEST(GraphOracle, RandomBytesAreAnInputToo) {
  Bytes input(spec::kInputBytes);
  rng::fill_stream(0xBAD, 1, input);
  expect_equals_reference(input, "random bytes");
}

// ---- the triangle inequality ------------------------------------------------------

TEST(GraphOracle, TheTriangleInequalityHoldsOnEveryEdge) {
  for (std::uint64_t index = 0; index < 3; ++index) {
    const Bytes input = generated(11, index);
    const auto workload = make(config_for(0, simd::IsaPath::kScalar));
    ASSERT_NE(workload, nullptr);
    const std::vector<std::uint32_t> distance = distances_of(run_serial(*workload, input));
    EXPECT_EQ(distance[word(input, spec::kEdges)], 0U);
    for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
      const std::uint32_t tail = distance[edge / 8];
      const std::uint32_t head = distance[word(input, edge)];
      if (tail != kUnreached) {
        ASSERT_LE(head, tail + 1) << "edge " << edge;
      }
    }
  }
}

}  // namespace
}  // namespace loadforge::graph_test
