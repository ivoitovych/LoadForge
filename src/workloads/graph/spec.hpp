// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKLOADS_GRAPH_SPEC_HPP
#define LOADFORGE_WORKLOADS_GRAPH_SPEC_HPP

// CONTRACT (ADR 0004). The specification the graph workload and its
// independent oracle are both written from (verification.md §3, §5;
// determinism.md §2; IMPLEMENTATION M4). Frozen: changed only by a contract
// change, never inside a stream.
//
//   The graph stream writes the stress implementation: a parallel
//     direction-optimising BFS and a parallel pull-based PageRank, with
//     scalar, AVX2 and NEON kernels behind the dispatch seam
//     (src/workloads/graph/).
//   The wave-3 oracle stream writes the oracle (a scalar BFS and a scalar
//     PageRank that follow the arithmetic below, src/verification/oracle/
//     graph/) from this file and nothing else -- never from the kernels.
//
// VERSIONS. kVersion versions this SPECIFICATION: the unit's format and the
// check below. A change to either increments it. The workload's own bits are
// versioned separately, by Descriptor::version (workload.hpp), which moves
// whenever a configuration's output changes -- including when this
// specification does not.
//
// A UNIT: ONE DIRECTED GRAPH, BFS DISTANCES FROM ONE SOURCE, AND PAGERANK
// -----------------------------------------------------------------------
// V = kVertices vertices numbered [0, V); every vertex has exactly kOutDegree
// = 8 out-edges, so E = kEdges = 8 V and nothing is dangling. Edge e, for e in
// [0, E), runs from vertex e / 8 (integer division) and is the (e mod 8)-th
// out-edge of its source. The graph is a multigraph: self-loops and repeated
// edges are allowed and count as edges, every one.
//
//   input   (E + 1) little-endian u32, kInputBytes in all:
//             targets[e], e in [0, E): the head of edge e, then
//             source: the BFS source.
//           Draw d of the unit is u64_of(block_at(seed, index, d))
//           (primitives/rng/philox.hpp); targets[e] is draw e modulo V, and
//           source is draw E modulo V (the u64 modulo; V is a power of two,
//           so it is the low 15 bits). Only the first two words of each
//           Philox block are used.
//   output  kOutputBytes, in two sections:
//             [0, kRanksOffset)  distance[v], v in [0, V): u32, the number
//                                of edges on a shortest directed path from
//                                the source to v (0 for the source itself),
//                                or kUnreachable = 0xFFFFFFFF;
//             [kRanksOffset, kOutputBytes)  rank[v], v in [0, V): the
//                                PageRank after kIterations iterations, a
//                                little-endian double.
//
// TOTALITY. The workload computes on every input byte pattern (workload.hpp).
// The specification's answer for a vertex number that is not below V, which
// the generator never writes: it is REDUCED MODULO V before use, as a u32
// modulo, both for the heads of edges and for the source. Workload and oracle
// do the same, so they agree on a corrupt input as on any other, and the
// input-integrity bracket (verification.md §4a) is what reports the
// corruption. Nothing else needs a rule: every operation below is defined for
// every input.
//
// BFS. Distances follow edges forward, from tail to head: distance[source] =
// 0, and for any other vertex the least k such that some vertex at distance
// k - 1 has an edge to it. This is a property of the graph and the source,
// not of how the frontier was traversed; the BFS tree (which parent reached a
// vertex) is not part of the output, because it depends on frontier order
// (determinism.md §2).
//
// PAGERANK. With d = kDamping (the double nearest 0.85) and c = kTeleport =
// (1 - d) / V, which is computed exactly in double (1 - d is exact because d
// is within a factor 2 of 1, and V is a power of two), r_0[v] = 1/V = 2^-15 for
// every vertex, and for it = 1 .. kIterations (a Jacobi iteration: iteration
// it reads only r_{it-1}):
//
//   for each vertex v:
//     the in-edges of v are the edges e with head(e) = v, taken in
//     INCREASING e, as in_0, in_1, ..., in_{m-1} (m = 0 is possible);
//     each contributes the term  t_q = r_{it-1}[tail(in_q)] * 0.125
//     (exact: a power-of-two scaling; rank/8 is the rank over the out-degree);
//     the terms are accumulated into `a` accumulators, a the decomposition:
//     term q goes to accumulator q mod a, each accumulator starts at +0.0 and
//     adds its terms in increasing q;
//     S = ((acc_0 + acc_1) + acc_2) + ... + acc_{a-1}, left to right
//         (for a = 1, S = acc_0);
//     r_it[v] = c + d * S, with d * S rounded to double BEFORE the addition.
//
// Every operation is one IEEE 754 binary64 add or multiply, in an order the
// specification pins, so the result is a function of the decomposition a and
// the input and nothing else. A fused multiply-add is NOT permitted for
// d * S + c (it would round once instead of twice and change the bits), and
// no other operation of the unit is a multiply-add. There is no libm call.
// There is no normalisation, no convergence test and no early exit: all
// kIterations iterations run.
//
// DECOMPOSITION: the number of accumulators a per vertex, one of
// kAccumulators, default kDefaultAccumulators -- the width of the partial-sum
// vector a gather kernel keeps per vertex.
//
// DETERMINISM: kBitExactFixedDecomposition (determinism.md §2, §3). The
// distances are BitExact, a property of the graph. The ranks are the same
// bits for one a at any thread count, any run, ANY ISA PATH and either
// architecture -- stronger than the class demands, because the arithmetic is
// pinned -- and different bits for different a (the summation order differs).
// Descriptor::determinism is kBitExactFixedDecomposition, the weaker of the
// two sections.
//
// THE CHECK IS EXACT -- there is no tolerance in this specification.
// ------------------------------------------------------------------
// The oracle's contract is Contract::kExact, and for one reason: the
// arithmetic is pinned, so a correct output is a single byte string. The
// oracle recomputes the whole expected output by plain scalar code, from the
// INPUT it is given and the decomposition of its plan, and compares it with
// the output it is given, all kOutputBytes. (It does not sample a subgraph: a
// full scalar BFS and kIterations scalar PageRank sweeps over 262144 edges
// cost a few milliseconds, so the whole output is covered. The triangle
// inequality, distance[head] <= distance[tail] + 1 for every edge with a
// reachable tail, is a property of the correct answer and is implied by
// equality with it; the oracle's own tests may use it as a cross-check.)
//   * Agreement{kSections}: every byte equal. blocks_compared counts the two
//     sections, both compared;
//   * ExactMismatch: some byte differs. blocks_compared is kSections (the
//     whole output is compared in one go); `check` is "bfs-distances" when
//     the first differing byte is below kRanksOffset and "pagerank"
//     otherwise, so when both sections are wrong it is the distances that are
//     named; `difference` is a Difference in Domain::kStored over the whole
//     output (first and last differing byte from the start of the output,
//     popcount over the whole comparison, expected and actual from the
//     window, as record.hpp defines them);
//   * NotChecked (blocks_compared 0) when the evidence is not of the sizes
//     kInputBytes and kOutputBytes. Nothing else: no input makes the
//     expected output undefined.
// The same value of a is needed to recompute the ranks: it is the oracle's
// plan's decomposition. An oracle created for a decomposition not in
// kAccumulators is refused as OracleFault::kConfiguration.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loadforge::workloads::graph {

/// The name configuration and reports use: Descriptor::name.
inline constexpr std::string_view kName = "graph";
inline constexpr std::uint32_t kVersion = 1;

inline constexpr std::size_t kVertices = 32768;
inline constexpr std::size_t kOutDegree = 8;
inline constexpr std::size_t kEdges = kVertices * kOutDegree;
inline constexpr std::uint32_t kIterations = 16;
inline constexpr std::uint32_t kUnreachable = 0xFFFFFFFFU;

inline constexpr std::size_t kInputBytes = (kEdges + 1) * sizeof(std::uint32_t);
inline constexpr std::size_t kRanksOffset = kVertices * sizeof(std::uint32_t);
inline constexpr std::size_t kOutputBytes = kRanksOffset + kVertices * sizeof(double);
/// Sections of the output: distances, ranks.
inline constexpr std::uint64_t kSections = 2;

inline constexpr std::array<std::uint32_t, 4> kAccumulators = {1, 2, 4, 8};
inline constexpr std::uint32_t kDefaultAccumulators = 4;

/// d, the double nearest 0.85.
inline constexpr double kDamping = 0.85;
/// c = (1 - d) / V; both operations are exact in double.
inline constexpr double kTeleport = (1.0 - kDamping) / static_cast<double>(kVertices);
/// r_0[v] = 1 / V, exact.
inline constexpr double kInitialRank = 1.0 / static_cast<double>(kVertices);
/// The factor each rank is scaled by to make an edge's term: 1 / kOutDegree.
inline constexpr double kEdgeShare = 0.125;

/// The vertex a 64-bit draw makes: the draw modulo V.
[[nodiscard]] constexpr std::uint32_t vertex_of(std::uint64_t bits) noexcept {
  return static_cast<std::uint32_t>(bits % kVertices);
}

/// A vertex number read from the input, reduced modulo V (TOTALITY above).
[[nodiscard]] constexpr std::uint32_t effective_vertex(std::uint32_t stored) noexcept {
  return stored % static_cast<std::uint32_t>(kVertices);
}

/// The tail (source vertex) of edge `edge`. Precondition: edge < kEdges.
[[nodiscard]] constexpr std::uint32_t tail_of(std::size_t edge) noexcept {
  return static_cast<std::uint32_t>(edge / kOutDegree);
}

/// Whether a configured decomposition is one the specification has.
[[nodiscard]] constexpr bool is_accumulators(std::uint32_t accumulators) noexcept {
  return std::ranges::any_of(kAccumulators,
                             [accumulators](std::uint32_t a) { return a == accumulators; });
}

}  // namespace loadforge::workloads::graph

#endif
