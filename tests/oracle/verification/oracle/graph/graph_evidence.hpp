// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_GRAPH_GRAPH_EVIDENCE_HPP
#define LOADFORGE_TESTS_ORACLE_VERIFICATION_ORACLE_GRAPH_GRAPH_EVIDENCE_HPP

// Evidence for the graph oracle's tests, in every tier: the unit whose
// distances scipy computed and whose ranks the specification's text gives
// (tests/fixtures/verification/oracle/graph/), its input regenerated with the
// contract's Philox, and an independent model of the whole expected output
// written as a per-vertex gather over explicit in-edge lists -- a different
// shape from the oracle's pass over the edges.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "workloads/graph/spec.hpp"

namespace loadforge::verification::oracle::graph::evidence {

namespace spec = workloads::graph;

/// The seed and unit index of the committed fixtures (generate_fixtures.py).
inline constexpr std::uint64_t kSeed = 0x4C6F616446697263ULL;
inline constexpr std::uint64_t kFixtureIndex = 9;

struct Unit {
  std::uint64_t index = 0;
  std::vector<std::byte> input;
  std::vector<std::byte> output;
};

inline void append_u32(std::vector<std::byte>& out, std::uint32_t word) {
  for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
    out.push_back(static_cast<std::byte>((word >> (8U * byte)) & 0xFFU));
  }
}

inline void append_double(std::vector<std::byte>& out, double value) {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  for (std::size_t byte = 0; byte < sizeof(double); ++byte) {
    out.push_back(static_cast<std::byte>((bits >> (8U * byte)) & 0xFFU));
  }
}

/// The input bytes for the stored vertex numbers `heads` (kEdges) and `source`.
inline std::vector<std::byte> input_bytes(std::span<const std::uint32_t> heads,
                                          std::uint32_t source) {
  std::vector<std::byte> out;
  out.reserve(spec::kInputBytes);
  for (const std::uint32_t head : heads) {
    append_u32(out, head);
  }
  append_u32(out, source);
  return out;
}

/// The heads and source the specification generates for (seed, index).
inline std::vector<std::uint32_t> generated_numbers(std::uint64_t seed, std::uint64_t index) {
  std::vector<std::uint32_t> numbers;
  numbers.reserve(spec::kEdges + 1);
  for (std::size_t draw = 0; draw <= spec::kEdges; ++draw) {
    numbers.push_back(spec::vertex_of(rng::u64_of(rng::block_at(seed, index, draw))));
  }
  return numbers;
}

inline std::vector<std::byte> generated_input(std::uint64_t seed, std::uint64_t index) {
  const std::vector<std::uint32_t> numbers = generated_numbers(seed, index);
  return input_bytes(std::span<const std::uint32_t>(numbers).first(spec::kEdges),
                     numbers[spec::kEdges]);
}

inline std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t at = 0; at < raw.size(); ++at) {
    out[at] = static_cast<std::byte>(static_cast<unsigned char>(raw[at]));
  }
  return out;
}

/// The unit whose distances scipy computed and whose ranks are the
/// specification's, for `accumulators` accumulators.
inline Unit real_tool_unit(std::uint32_t accumulators) {
  const std::string stem = std::string(LOADFORGE_SOURCE_DIR) +
                           "/tests/fixtures/verification/oracle/graph/unit-" +
                           std::to_string(kFixtureIndex);
  std::vector<std::byte> output = read_file(stem + ".distances.bin");
  const std::vector<std::byte> ranks =
      read_file(stem + ".ranks-a" + std::to_string(accumulators) + ".bin");
  output.insert(output.end(), ranks.begin(), ranks.end());
  return Unit{kFixtureIndex, generated_input(kSeed, kFixtureIndex), std::move(output)};
}

/// The expected output of the specification for stored numbers `heads` and
/// `source` (reduced modulo V here, as the specification's totality says) and
/// `accumulators` accumulators: BFS by levels, and PageRank as a gather over
/// each vertex's explicit list of in-edges.
inline std::vector<std::byte> model_output(std::span<const std::uint32_t> heads,
                                           std::uint32_t source, std::uint32_t accumulators) {
  std::vector<std::uint32_t> head(spec::kEdges);
  std::vector<std::vector<std::uint32_t>> in_tails(spec::kVertices);
  std::vector<std::vector<std::uint32_t>> out_heads(spec::kVertices);
  for (std::size_t e = 0; e < spec::kEdges; ++e) {
    head[e] = heads[e] % static_cast<std::uint32_t>(spec::kVertices);
    in_tails[head[e]].push_back(static_cast<std::uint32_t>(e / spec::kOutDegree));
    out_heads[e / spec::kOutDegree].push_back(head[e]);
  }

  std::vector<std::uint32_t> distance(spec::kVertices, spec::kUnreachable);
  std::vector<std::uint32_t> frontier = {source % static_cast<std::uint32_t>(spec::kVertices)};
  distance[frontier[0]] = 0;
  for (std::uint32_t level = 1; !frontier.empty(); ++level) {
    std::vector<std::uint32_t> following;
    for (const std::uint32_t tail : frontier) {
      for (const std::uint32_t reached : out_heads[tail]) {
        if (distance[reached] == spec::kUnreachable) {
          distance[reached] = level;
          following.push_back(reached);
        }
      }
    }
    frontier = std::move(following);
  }

  std::vector<double> rank(spec::kVertices, 1.0 / static_cast<double>(spec::kVertices));
  for (std::uint32_t iteration = 0; iteration < spec::kIterations; ++iteration) {
    std::vector<double> following(spec::kVertices);
    for (std::size_t v = 0; v < spec::kVertices; ++v) {
      std::vector<double> parts(accumulators, 0.0);
      for (std::size_t q = 0; q < in_tails[v].size(); ++q) {
        parts[q % accumulators] += rank[in_tails[v][q]] * 0.125;
      }
      double total = parts[0];
      for (std::size_t j = 1; j < parts.size(); ++j) {
        total = total + parts[j];
      }
      const double damped = spec::kDamping * total;
      following[v] = spec::kTeleport + damped;
    }
    rank = std::move(following);
  }

  std::vector<std::byte> out;
  out.reserve(spec::kOutputBytes);
  for (const std::uint32_t d : distance) {
    append_u32(out, d);
  }
  for (const double r : rank) {
    append_double(out, r);
  }
  return out;
}

}  // namespace loadforge::verification::oracle::graph::evidence

#endif
