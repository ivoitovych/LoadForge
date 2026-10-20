// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definitions the graph specification carries.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "workloads/graph/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::workloads {
namespace {

TEST(GraphSpecDefinition, TheNameIsAValidWorkloadNameAndTheSizesAreTheUnitsLayout) {
  EXPECT_EQ(graph::kName, "graph");
  EXPECT_TRUE(is_valid_name(graph::kName));
  EXPECT_EQ(graph::kVersion, 1U);
  EXPECT_EQ(graph::kVertices, std::size_t{1} << 15U) << "a power of two: the draw's modulo";
  EXPECT_EQ(graph::kOutDegree, 8U);
  EXPECT_EQ(graph::kEdges, 262144U);
  EXPECT_EQ(graph::kIterations, 16U);
  EXPECT_EQ(graph::kInputBytes, 262145U * 4U);
  EXPECT_EQ(graph::kInputBytes, 1048580U);
  EXPECT_EQ(graph::kRanksOffset, 131072U);
  EXPECT_EQ(graph::kRanksOffset % alignof(double), 0U) << "the ranks start on a double";
  EXPECT_EQ(graph::kOutputBytes, 131072U + 262144U);
  EXPECT_EQ(graph::kSections, 2U);
  EXPECT_EQ(graph::kUnreachable, 0xFFFFFFFFU);
}

TEST(GraphSpecDefinition, ADrawMakesTheVertexItIsModuloTheVertexCount) {
  using graph::vertex_of;
  EXPECT_EQ(vertex_of(0), 0U);
  EXPECT_EQ(vertex_of(1), 1U);
  EXPECT_EQ(vertex_of(32767), 32767U);
  EXPECT_EQ(vertex_of(32768), 0U);
  EXPECT_EQ(vertex_of(32769), 1U);
  EXPECT_EQ(vertex_of(~std::uint64_t{0}), 32767U);
  EXPECT_EQ(vertex_of(0xFFFFFFFF00000000ULL | 5U), 5U) << "the high bits take no part";
}

TEST(GraphSpecDefinition, AStoredVertexNumberIsReducedModuloTheVertexCount) {
  using graph::effective_vertex;
  EXPECT_EQ(effective_vertex(0), 0U);
  EXPECT_EQ(effective_vertex(32767), 32767U);
  EXPECT_EQ(effective_vertex(32768), 0U) << "totality: not below V is reduced";
  EXPECT_EQ(effective_vertex(0xFFFFFFFFU), 32767U);
}

TEST(GraphSpecDefinition, AnEdgeStartsAtItsSourceVertex) {
  EXPECT_EQ(graph::tail_of(0), 0U);
  EXPECT_EQ(graph::tail_of(7), 0U);
  EXPECT_EQ(graph::tail_of(8), 1U);
  EXPECT_EQ(graph::tail_of(graph::kEdges - 1), graph::kVertices - 1);
}

TEST(GraphSpecDefinition, TheAccumulatorsAreOneTwoFourAndEightAndTheDefaultIsOne) {
  for (const std::uint32_t accumulators : {1U, 2U, 4U, 8U}) {
    EXPECT_TRUE(graph::is_accumulators(accumulators)) << accumulators;
  }
  for (const std::uint32_t accumulators : {0U, 3U, 5U, 16U}) {
    EXPECT_FALSE(graph::is_accumulators(accumulators)) << accumulators;
  }
  EXPECT_TRUE(graph::is_accumulators(graph::kDefaultAccumulators));
  EXPECT_EQ(graph::kDefaultAccumulators, 4U);
}

TEST(GraphSpecDefinition, ThePageRankConstantsAreExactInDouble) {
  EXPECT_EQ(std::bit_cast<std::uint64_t>(graph::kDamping), 0x3FEB333333333333ULL)
      << "the double nearest 0.85";
  const long double exact_teleport = (1.0L - static_cast<long double>(graph::kDamping)) /
                                     static_cast<long double>(graph::kVertices);
  EXPECT_EQ(static_cast<long double>(graph::kTeleport), exact_teleport)
      << "1 - d and the division by a power of two round nothing";
  EXPECT_EQ(graph::kTeleport * static_cast<double>(graph::kVertices) + graph::kDamping, 1.0);
  EXPECT_EQ(graph::kInitialRank, std::ldexp(1.0, -15));
  EXPECT_EQ(graph::kInitialRank * static_cast<double>(graph::kVertices), 1.0);
  EXPECT_EQ(graph::kEdgeShare, 1.0 / static_cast<double>(graph::kOutDegree));
  EXPECT_EQ(graph::kEdgeShare * 3.0, 0.375) << "scaling by 1/8 is exact";
}

}  // namespace
}  // namespace loadforge::workloads
