// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/oracle/graph/graph_oracle.hpp"

#include <algorithm>
#include <bit>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace loadforge::verification::oracle::graph {
namespace {

namespace spec = workloads::graph;

constexpr std::size_t kMaxAccumulators = 8;
static_assert(spec::kAccumulators.back() == kMaxAccumulators);

NotChecked not_checked(std::string what, std::string why) {
  return NotChecked{std::move(what), std::move(why), 0};
}

void store_u32(std::span<std::byte, sizeof(std::uint32_t)> destination,
               std::uint32_t word) noexcept {
  for (std::size_t byte = 0; byte < destination.size(); ++byte) {
    destination[byte] = static_cast<std::byte>((word >> (8U * byte)) & 0xFFU);
  }
}

void store_double(std::span<std::byte, sizeof(double)> destination, double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (std::size_t byte = 0; byte < destination.size(); ++byte) {
    destination[byte] = static_cast<std::byte>((bits >> (8U * byte)) & 0xFFU);
  }
}

/// The distances from the source: a breadth-first search over the out-edges,
/// which are the eight consecutive edges of a vertex.
void breadth_first(std::span<const std::uint32_t> heads, std::uint32_t source,
                   Scratch& scratch) noexcept {
  const std::span<std::uint32_t> distance(scratch.distance);
  const std::span<std::uint32_t> queue(scratch.queue);
  std::ranges::fill(distance, spec::kUnreachable);
  distance[source] = 0;
  queue[0] = source;
  std::size_t read = 0;
  std::size_t written = 1;
  while (read < written) {
    const std::uint32_t tail = queue[read++];
    for (std::size_t out = 0; out < spec::kOutDegree; ++out) {
      const std::uint32_t head = heads[std::size_t{tail} * spec::kOutDegree + out];
      if (distance[head] == spec::kUnreachable) {
        distance[head] = distance[tail] + 1;
        queue[written++] = head;
      }
    }
  }
}

/// PageRank after kIterations iterations, the arithmetic of the specification
/// in the specification's order. The in-edges of a vertex in increasing edge
/// number are met by walking the edges in increasing order, so each
/// accumulator receives its terms in increasing q.
std::span<const double> page_rank(std::span<const std::uint32_t> heads, std::size_t accumulators,
                                  Scratch& scratch) noexcept {
  const std::span<std::uint32_t> in_degree(scratch.in_degree);
  const std::span<std::uint8_t> slot(scratch.slot);
  std::ranges::fill(in_degree, 0U);
  for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
    const std::uint32_t head = heads[edge];
    slot[edge] = static_cast<std::uint8_t>(in_degree[head] % accumulators);
    ++in_degree[head];
  }

  std::span<double> previous(scratch.rank_even);
  std::span<double> next(scratch.rank_odd);
  const std::span<double> sums(scratch.accumulators.data(), spec::kVertices * accumulators);
  std::ranges::fill(previous, spec::kInitialRank);
  for (std::uint32_t iteration = 0; iteration < spec::kIterations; ++iteration) {
    std::ranges::fill(sums, 0.0);
    for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
      const double term = previous[spec::tail_of(edge)] * spec::kEdgeShare;
      sums[std::size_t{heads[edge]} * accumulators + slot[edge]] += term;
    }
    for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
      double total = sums[vertex * accumulators];
      for (std::size_t part = 1; part < accumulators; ++part) {
        total += sums[vertex * accumulators + part];
      }
      const double damped = spec::kDamping * total;
      next[vertex] = spec::kTeleport + damped;
    }
    std::swap(previous, next);
  }
  return previous;
}

/// The comparison of the output with the expected bytes as the verdict's
/// Difference describes it: first and last differing byte, the differing bits
/// over the whole comparison, and the 64-byte window from the word holding the
/// first. Nothing when the two are equal.
std::optional<Difference> compare(std::span<const std::byte> expected,
                                  std::span<const std::byte> actual) {
  std::optional<std::size_t> first;
  std::size_t last = 0;
  std::uint64_t popcount = 0;
  for (std::size_t at = 0; at < expected.size(); ++at) {
    const auto delta = std::to_integer<unsigned>(expected[at] ^ actual[at]);
    if (delta != 0) {
      if (!first.has_value()) {
        first = at;
      }
      last = at;
      popcount += static_cast<std::uint64_t>(std::popcount(delta));
    }
  }
  if (!first.has_value()) {
    return std::nullopt;
  }
  Difference difference;
  difference.domain = Domain::kStored;
  difference.first_offset = *first;
  difference.last_offset = last;
  difference.popcount = popcount;
  difference.window_offset = *first / kWordBytes * kWordBytes;
  const std::size_t end =
      std::min<std::size_t>(expected.size(), difference.window_offset + kMaxWindowBytes);
  difference.expected.assign(
      expected.begin() + static_cast<std::ptrdiff_t>(difference.window_offset),
      expected.begin() + static_cast<std::ptrdiff_t>(end));
  difference.actual.assign(actual.begin() + static_cast<std::ptrdiff_t>(difference.window_offset),
                           actual.begin() + static_cast<std::ptrdiff_t>(end));
  return difference;
}

}  // namespace

std::uint32_t load_u32(std::span<const std::byte> bytes, std::size_t element) noexcept {
  std::uint32_t word = 0;
  for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
    word |= std::to_integer<std::uint32_t>(bytes[element * sizeof(std::uint32_t) + byte])
            << (8U * byte);
  }
  return word;
}

std::unique_ptr<Scratch> allocate_scratch() noexcept {
  return std::unique_ptr<Scratch>(new (std::nothrow) Scratch);
}

void compute_expected_output(std::span<const std::byte> input, std::uint32_t accumulators,
                             Scratch& scratch) noexcept {
  const std::span<std::uint32_t> heads(scratch.head);
  for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
    heads[edge] = spec::effective_vertex(load_u32(input, edge));
  }
  const std::uint32_t source = spec::effective_vertex(load_u32(input, spec::kEdges));
  breadth_first(heads, source, scratch);
  const std::span<const double> ranks = page_rank(heads, accumulators, scratch);
  const std::span<std::byte> expected(scratch.expected);
  const std::span<const std::uint32_t> distances(scratch.distance);
  for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
    store_u32(expected.subspan(vertex * sizeof(std::uint32_t)).first<sizeof(std::uint32_t)>(),
              distances[vertex]);
    store_double(
        expected.subspan(spec::kRanksOffset + vertex * sizeof(double)).first<sizeof(double)>(),
        ranks[vertex]);
  }
}

std::string_view GraphOracle::workload() const noexcept { return spec::kName; }

Contract GraphOracle::contract() const noexcept { return Contract::kExact; }

Verdict GraphOracle::check(const UnitEvidence& evidence) const {
  if (evidence.input.size() != spec::kInputBytes || evidence.output.size() != spec::kOutputBytes) {
    return not_checked("graph unit", "evidence is not 1048580 bytes of input and 393216 of output");
  }
  const std::unique_ptr<Scratch> scratch = scratch_();
  if (scratch == nullptr) {
    return not_checked("graph expected output",
                       "memory for recomputing the expected output could not be allocated");
  }
  compute_expected_output(evidence.input, accumulators_, *scratch);
  std::optional<Difference> difference = compare(scratch->expected, evidence.output);
  if (!difference.has_value()) {
    return Agreement{spec::kSections};
  }
  const bool in_distances = difference->first_offset < spec::kRanksOffset;
  return ExactMismatch{in_distances ? "bfs-distances" : "pagerank", std::move(difference),
                       spec::kSections};
}

core::Result<std::unique_ptr<Oracle>, OracleError> create(
    const workloads::WorkloadConfig& /*config*/, const workloads::Plan& plan) {
  if (plan.input_bytes != spec::kInputBytes || plan.output_bytes != spec::kOutputBytes) {
    std::string detail =
        "the plan's unit is not a 32768-vertex graph in and its distances and ranks out";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  if (!spec::is_accumulators(plan.decomposition)) {
    std::string detail = "the plan's decomposition is not 1, 2, 4 or 8 accumulators";
    return OracleError{OracleFault::kConfiguration, std::move(detail)};
  }
  std::unique_ptr<Oracle> oracle = std::make_unique<GraphOracle>(plan.decomposition);
  return oracle;
}

}  // namespace loadforge::verification::oracle::graph
