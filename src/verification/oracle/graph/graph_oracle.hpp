// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_VERIFICATION_ORACLE_GRAPH_GRAPH_ORACLE_HPP
#define LOADFORGE_VERIFICATION_ORACLE_GRAPH_GRAPH_ORACLE_HPP

// The independent oracle of the graph workload (verification.md §3, §5),
// written from src/workloads/graph/spec.hpp and from nothing else. The
// specification pins the arithmetic, so a correct output is a single byte
// string and the oracle's contract is Contract::kExact: it recomputes the
// whole expected output -- a scalar BFS and kIterations scalar PageRank sweeps
// -- from the input it is given and the decomposition of its plan, and
// compares every byte.
//
// The recomputation needs about four megabytes of working memory, which the
// oracle takes from a ScratchFactory once per check(). A factory that cannot
// give it makes the check NotChecked (oracle.hpp), never an agreement; the
// factory is the seam a test uses to make that happen.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "core/result.hpp"
#include "verification/oracle.hpp"
#include "workloads/graph/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::verification::oracle::graph {

/// The working memory of one recomputation.
struct Scratch {
  /// The head of every edge as a vertex, decoded once from the input.
  std::array<std::uint32_t, workloads::graph::kEdges> head;
  std::array<std::uint32_t, workloads::graph::kVertices> distance;
  std::array<std::uint32_t, workloads::graph::kVertices> queue;
  std::array<std::uint32_t, workloads::graph::kVertices> in_degree;
  /// For each edge, which accumulator its term goes to: its rank among the
  /// in-edges of its head, modulo the accumulator count.
  std::array<std::uint8_t, workloads::graph::kEdges> slot;
  std::array<double, workloads::graph::kVertices> rank_even;
  std::array<double, workloads::graph::kVertices> rank_odd;
  std::array<double, workloads::graph::kVertices * 8> accumulators;
  /// The expected output, as the bytes the workload is to have written.
  std::array<std::byte, workloads::graph::kOutputBytes> expected;
};

/// Memory for one recomputation, or nothing when there is none to be had.
using ScratchFactory = std::unique_ptr<Scratch> (*)() noexcept;

/// The factory the oracle uses unless it is given another.
[[nodiscard]] std::unique_ptr<Scratch> allocate_scratch() noexcept;

/// Fills scratch.expected with the output the specification gives for `input`
/// (kInputBytes) and `accumulators` (one of kAccumulators).
void compute_expected_output(std::span<const std::byte> input, std::uint32_t accumulators,
                             Scratch& scratch) noexcept;

/// The little-endian u32 at `element` of a span of u32 stored as bytes.
/// Precondition: (element + 1) * 4 <= bytes.size().
[[nodiscard]] std::uint32_t load_u32(std::span<const std::byte> bytes,
                                     std::size_t element) noexcept;

class GraphOracle final : public Oracle {
 public:
  /// Precondition: workloads::graph::is_accumulators(accumulators); create()
  /// checks it.
  explicit GraphOracle(std::uint32_t accumulators,
                       ScratchFactory scratch = &allocate_scratch) noexcept
      : accumulators_(accumulators), scratch_(scratch) {}

  [[nodiscard]] std::string_view workload() const noexcept override;
  [[nodiscard]] Contract contract() const noexcept override;
  [[nodiscard]] Verdict check(const UnitEvidence& evidence) const override;

 private:
  std::uint32_t accumulators_;
  ScratchFactory scratch_;
};

/// The oracle for a plan the graph workload resolved: kConfiguration when the
/// plan's sizes are not a unit of the specification or its decomposition is not
/// one of its accumulator counts.
[[nodiscard]] core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& config, const workloads::Plan& plan);

}  // namespace loadforge::verification::oracle::graph

#endif
