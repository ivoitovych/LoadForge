// SPDX-License-Identifier: GPL-3.0-or-later
//
// T4: properties of the output that hold for every graph, checked on many
// graphs -- generated units, random bytes (so out-of-range vertex numbers and
// graphs of every shape) and relabelled copies. They need no reference: each
// is a statement the correct answer satisfies, which a wrong one rarely does.

#include <gtest/gtest.h>

#include <cmath>
#include <numeric>

#include "graph_support.hpp"

namespace loadforge::graph_test {
namespace {

constexpr std::uint32_t kUnreached = 0xFFFFFFFFU;

/// A mix of inputs: generated units of several seeds, random bytes, and
/// random bytes with every word reduced (random graphs with no excess).
std::vector<Bytes> inputs() {
  std::vector<Bytes> all;
  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    all.push_back(reference_input(seed, seed));
  }
  for (std::uint64_t seed = 100; seed < 102; ++seed) {
    Bytes noise(spec::kInputBytes);
    rng::fill_stream(seed, 0, noise);
    all.push_back(noise);
  }
  // A sparse graph: most edges lead to a handful of vertices, so most
  // vertices have no in-edges and many are unreachable.
  Bytes narrow = reference_input(9, 9);
  for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
    set_word(narrow, edge, word(narrow, edge) % 64U);
  }
  all.push_back(narrow);
  return all;
}

std::unique_ptr<workloads::Workload> scalar(std::uint32_t accumulators = 0) {
  return make(config_for(accumulators, simd::IsaPath::kScalar));
}

TEST(GraphProperty, TheSourceIsTheOnlyVertexAtDistanceZeroAndEveryOtherDistanceHasAWitness) {
  const auto workload = scalar();
  ASSERT_NE(workload, nullptr);
  for (const Bytes& input : inputs()) {
    const std::vector<std::uint32_t> distance = distances_of(run_serial(*workload, input));
    const std::uint32_t source = word(input, spec::kEdges) % 32768U;
    EXPECT_EQ(distance[source], 0U);
    EXPECT_EQ(std::ranges::count(distance, 0U), 1);
    // Every reached vertex but the source has an in-neighbour exactly one
    // closer (a shortest path's last edge), and none has an in-neighbour more
    // than one closer (the triangle inequality, on every edge).
    std::vector<bool> witnessed(spec::kVertices, false);
    for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
      const std::uint32_t tail = distance[edge / 8];
      const std::uint32_t head_vertex = word(input, edge) % 32768U;
      if (tail == kUnreached) {
        continue;
      }
      ASSERT_LE(distance[head_vertex], tail + 1);
      if (distance[head_vertex] == tail + 1) {
        witnessed[head_vertex] = true;
      }
    }
    for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
      if (distance[vertex] != kUnreached && vertex != source) {
        EXPECT_TRUE(witnessed[vertex]) << vertex;
      }
    }
  }
}

TEST(GraphProperty, AnUnreachedVertexHasNoReachedInNeighbour) {
  const auto workload = scalar();
  ASSERT_NE(workload, nullptr);
  for (const Bytes& input : inputs()) {
    const std::vector<std::uint32_t> distance = distances_of(run_serial(*workload, input));
    for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
      if (distance[edge / 8] != kUnreached) {
        EXPECT_NE(distance[word(input, edge) % 32768U], kUnreached) << "edge " << edge;
      }
    }
  }
}

TEST(GraphProperty, RanksAreFiniteAtLeastTheTeleportTermAndSumToOne) {
  for (const std::uint32_t accumulators : spec::kAccumulators) {
    const auto workload = scalar(accumulators);
    ASSERT_NE(workload, nullptr);
    for (const Bytes& input : inputs()) {
      const std::vector<double> rank = ranks_of(run_serial(*workload, input));
      double sum = 0.0;
      for (const double value : rank) {
        ASSERT_TRUE(std::isfinite(value));
        ASSERT_GE(value, spec::kTeleport);
        sum += value;
      }
      // No vertex is dangling, so each sweep keeps the total mass at one.
      EXPECT_NEAR(sum, 1.0, 1e-9) << accumulators;
    }
  }
}

TEST(GraphProperty, AVertexWithNoInEdgeHasExactlyTheTeleportRank) {
  const auto workload = scalar();
  ASSERT_NE(workload, nullptr);
  for (const Bytes& input : inputs()) {
    std::vector<bool> has_in_edge(spec::kVertices, false);
    for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
      has_in_edge[word(input, edge) % 32768U] = true;
    }
    const std::vector<double> rank = ranks_of(run_serial(*workload, input));
    for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
      if (!has_in_edge[vertex]) {
        ASSERT_EQ(rank[vertex], spec::kTeleport) << vertex;
      }
    }
  }
}

TEST(GraphProperty, TheDecompositionNeverChangesTheDistancesAndMovesTheRanksOnlyAtRounding) {
  for (const Bytes& input : inputs()) {
    const Bytes base = run_serial(*scalar(1), input);
    for (const std::uint32_t accumulators : {2U, 4U, 8U}) {
      const Bytes other = run_serial(*scalar(accumulators), input);
      EXPECT_EQ(distances_of(other), distances_of(base)) << accumulators;
      const std::vector<double> a = ranks_of(base);
      const std::vector<double> b = ranks_of(other);
      for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
        // Different summation orders of the same non-negative terms: rounding
        // error compounded over sixteen sweeps through the busiest hubs is
        // many orders of magnitude below this bound, a wrong sum is not.
        ASSERT_NEAR(a[vertex], b[vertex], 1e-10 * a[vertex]) << vertex;
      }
    }
  }
}

TEST(GraphProperty, RelabellingTheVerticesRelabelsTheDistances) {
  // Metamorphic: the same graph with vertex v renamed pi(v) has the same
  // distances under the renaming. (Ranks are not covered: a renaming changes
  // the order of a vertex's in-edges, hence the summation order.)
  const auto workload = scalar();
  ASSERT_NE(workload, nullptr);
  std::vector<std::uint32_t> pi(spec::kVertices);
  std::iota(pi.begin(), pi.end(), 0U);
  // A fixed pseudo-random permutation: sort by a Philox key.
  std::vector<std::uint64_t> key(spec::kVertices);
  for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
    key[vertex] = rng::u64_of(rng::block_at(77, 0, vertex));
  }
  std::ranges::sort(pi, [&key](std::uint32_t lhs, std::uint32_t rhs) {
    return key[lhs] != key[rhs] ? key[lhs] < key[rhs] : lhs < rhs;
  });  // pi[rank] = the vertex of that rank; so rename vertex pi[i] to i:
  std::vector<std::uint32_t> rename(spec::kVertices);
  for (std::size_t i = 0; i < spec::kVertices; ++i) {
    rename[pi[i]] = static_cast<std::uint32_t>(i);
  }
  for (const Bytes& input : inputs()) {
    Bytes moved(spec::kInputBytes);
    for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
      for (std::size_t slot = 0; slot < 8; ++slot) {
        set_word(moved, rename[vertex] * 8 + slot, rename[word(input, vertex * 8 + slot) % 32768U]);
      }
    }
    set_word(moved, spec::kEdges, rename[word(input, spec::kEdges) % 32768U]);
    const std::vector<std::uint32_t> before = distances_of(run_serial(*workload, input));
    const std::vector<std::uint32_t> after = distances_of(run_serial(*workload, moved));
    for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
      ASSERT_EQ(after[rename[vertex]], before[vertex]) << vertex;
    }
  }
}

TEST(GraphProperty, RedirectingAnEdgeOfAReachedVertexBoundsTheNewHeadsDistance) {
  // Metamorphic: after edge 0 (an edge of vertex 0) is made to lead to
  // `target`, the target is at most one step beyond vertex 0's old distance,
  // whatever the edge led to before.
  const auto workload = scalar();
  ASSERT_NE(workload, nullptr);
  const Bytes input = reference_input(21, 1);
  const std::vector<std::uint32_t> before = distances_of(run_serial(*workload, input));
  ASSERT_NE(before[0], kUnreached) << "the generated graph reaches vertex 0 from this source";
  Bytes changed = input;
  const std::uint32_t target = 12345;
  set_word(changed, 0, target);
  const std::vector<std::uint32_t> after = distances_of(run_serial(*workload, changed));
  EXPECT_LE(after[target], before[0] + 1);
  EXPECT_EQ(after[word(input, spec::kEdges) % 32768U], 0U);
}

}  // namespace
}  // namespace loadforge::graph_test
