// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_WORKLOADS_GRAPH_SUPPORT_HPP
#define LOADFORGE_TESTS_WORKLOADS_GRAPH_SUPPORT_HPP

// What the graph workload's tests share, written from workloads/graph/spec.hpp
// and nothing in src/workloads/graph/ other than the public create():
//
//   * executors of three shapes (serial, a fixed partition over threads, and
//     threads that pull indices as they find them -- a real, varying schedule);
//   * an independent reference: the unit's input from the generator's own
//     definition, a plain queue BFS, and a PageRank that gathers each vertex's
//     in-edges into a vector and accumulates them exactly as the text of the
//     specification says, term by term.
//
// Every function here is plain and slow on purpose. None of them is the
// implementation's algorithm.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <queue>
#include <span>
#include <thread>
#include <vector>

#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"
#include "workloads/graph/graph.hpp"
#include "workloads/graph/spec.hpp"
#include "workloads/workload.hpp"

namespace loadforge::graph_test {

namespace spec = workloads::graph;
using Bytes = std::vector<std::byte>;

class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    ++runs;
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
  std::size_t runs = 0;
};

/// Slot s takes the indices congruent to s, last to first.
class PartitionExecutor final : public worker::Executor {
 public:
  explicit PartitionExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([this, slot, count, task, context] {
        for (std::size_t index = count; index-- > 0;) {
          if (index % threads_ == slot) {
            task(context, index, slot);
          }
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  std::size_t threads_;
};

/// Every thread claims the next unclaimed index until none is left: which
/// thread runs which index differs from run to run.
class PullExecutor final : public worker::Executor {
 public:
  explicit PullExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([&next, slot, count, task, context] {
        for (std::size_t index = next.fetch_add(1); index < count; index = next.fetch_add(1)) {
          task(context, index, slot);
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  std::size_t threads_;
};

[[nodiscard]] inline workloads::WorkloadConfig config_for(std::uint32_t decomposition,
                                                          simd::IsaPath isa,
                                                          std::uint64_t seed = 7) {
  return workloads::WorkloadConfig{seed, 3 * (spec::kInputBytes + spec::kOutputBytes),
                                   decomposition, isa};
}

[[nodiscard]] inline std::unique_ptr<workloads::Workload> make(
    const workloads::WorkloadConfig& config) {
  auto created = spec::create(config);
  return created.has_value() ? std::move(created).value() : nullptr;
}

/// Whether this machine can run the path's kernel (and this build has one).
[[nodiscard]] inline bool can_run(simd::IsaPath path) {
  if (!simd::compiled_paths().contains(path)) {
    return false;
  }
#if defined(__x86_64__)
  if (path == simd::IsaPath::kAvx2) {
    // NOLINTNEXTLINE(readability-implicit-bool-conversion): the builtin answers an int.
    return __builtin_cpu_supports("avx2") != 0;
  }
  return path == simd::IsaPath::kScalar;
#else
  return path == simd::IsaPath::kScalar || path == simd::IsaPath::kNeon;
#endif
}

/// The paths the graph workload has a kernel for and this machine can run.
[[nodiscard]] inline std::vector<simd::IsaPath> runnable_paths() {
  std::vector<simd::IsaPath> paths;
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, simd::IsaPath::kAvx2,
                                   simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    if (can_run(path) && make(config_for(0, path)) != nullptr) {
      paths.push_back(path);
    }
  }
  return paths;
}

// ---- words of the input --------------------------------------------------------

[[nodiscard]] inline std::uint32_t word(const Bytes& bytes, std::size_t index) {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data() + index * 4, 4);
  return value;
}

inline void set_word(Bytes& bytes, std::size_t index, std::uint32_t value) {
  std::memcpy(bytes.data() + index * 4, &value, 4);
}

/// The unit's input as spec.hpp defines it: draw d is u64_of(block_at(seed,
/// index, d)); the word is the draw modulo V.
[[nodiscard]] inline Bytes reference_input(std::uint64_t seed, std::uint64_t index) {
  Bytes input(spec::kInputBytes);
  for (std::size_t draw = 0; draw <= spec::kEdges; ++draw) {
    const rng::Block block = rng::block_at(seed, index, draw);
    const std::uint64_t bits = std::uint64_t{block[0]} | (std::uint64_t{block[1]} << 32U);
    set_word(input, draw, static_cast<std::uint32_t>(bits % 32768U));
  }
  return input;
}

/// An input built from a head function: edge e (tail e / 8) runs to head(e).
template <typename Head>
[[nodiscard]] Bytes input_of(Head head, std::uint32_t source) {
  Bytes input(spec::kInputBytes);
  for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
    set_word(input, edge, head(edge));
  }
  set_word(input, spec::kEdges, source);
  return input;
}

// ---- the reference ---------------------------------------------------------------

struct Reference {
  std::vector<std::uint32_t> distance;
  std::vector<double> rank;
};

[[nodiscard]] inline std::vector<std::uint32_t> reference_heads(const Bytes& input) {
  std::vector<std::uint32_t> heads(spec::kEdges);
  for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
    heads[edge] = word(input, edge) % 32768U;
  }
  return heads;
}

[[nodiscard]] inline std::uint32_t reference_source(const Bytes& input) {
  return word(input, spec::kEdges) % 32768U;
}

[[nodiscard]] inline std::vector<std::uint32_t> reference_bfs(const Bytes& input) {
  const std::vector<std::uint32_t> heads = reference_heads(input);
  std::vector<std::uint32_t> distance(spec::kVertices, 0xFFFFFFFFU);
  std::queue<std::uint32_t> queue;
  distance[reference_source(input)] = 0;
  queue.push(reference_source(input));
  while (!queue.empty()) {
    const std::uint32_t tail = queue.front();
    queue.pop();
    for (std::size_t slot = 0; slot < 8; ++slot) {
      const std::uint32_t head = heads[std::size_t{tail} * 8 + slot];
      if (distance[head] == 0xFFFFFFFFU) {
        distance[head] = distance[tail] + 1;
        queue.push(head);
      }
    }
  }
  return distance;
}

[[nodiscard]] inline std::vector<double> reference_pagerank(const Bytes& input,
                                                            std::uint32_t accumulators) {
  const std::vector<std::uint32_t> heads = reference_heads(input);
  std::vector<std::vector<std::uint32_t>> in_edges(spec::kVertices);
  for (std::size_t edge = 0; edge < spec::kEdges; ++edge) {
    in_edges[heads[edge]].push_back(static_cast<std::uint32_t>(edge));
  }
  const double teleport = (1.0 - 0.85) / 32768.0;
  std::vector<double> rank(spec::kVertices, 1.0 / 32768.0);
  for (int iteration = 0; iteration < 16; ++iteration) {
    std::vector<double> next(spec::kVertices);
    for (std::size_t vertex = 0; vertex < spec::kVertices; ++vertex) {
      std::vector<double> acc(accumulators, 0.0);
      for (std::size_t q = 0; q < in_edges[vertex].size(); ++q) {
        const double term = rank[in_edges[vertex][q] / 8] * 0.125;
        acc[q % accumulators] = acc[q % accumulators] + term;
      }
      double sum = acc[0];
      for (std::uint32_t lane = 1; lane < accumulators; ++lane) {
        sum = sum + acc[lane];
      }
      const volatile double product = 0.85 * sum;  // rounded to double before the addition
      next[vertex] = teleport + product;
    }
    rank = std::move(next);
  }
  return rank;
}

[[nodiscard]] inline Bytes pack(const std::vector<std::uint32_t>& distance,
                                const std::vector<double>& rank) {
  Bytes output(spec::kOutputBytes);
  std::memcpy(output.data(), distance.data(), spec::kRanksOffset);
  std::memcpy(output.data() + spec::kRanksOffset, rank.data(), spec::kVertices * sizeof(double));
  return output;
}

[[nodiscard]] inline Bytes reference_output(const Bytes& input, std::uint32_t accumulators) {
  return pack(reference_bfs(input), reference_pagerank(input, accumulators));
}

// ---- running the workload --------------------------------------------------------

[[nodiscard]] inline Bytes run_compute(const workloads::Workload& workload, const Bytes& input,
                                       worker::Executor& executor, std::byte fill = std::byte{0}) {
  Bytes output(workload.plan().output_bytes, fill);
  Bytes scratch(workload.plan().scratch_bytes * executor.threads(), fill);
  workload.compute(0, input, output, scratch, executor);
  return output;
}

[[nodiscard]] inline Bytes run_serial(const workloads::Workload& workload, const Bytes& input) {
  SerialExecutor serial;
  return run_compute(workload, input, serial);
}

[[nodiscard]] inline std::vector<std::uint32_t> distances_of(const Bytes& output) {
  std::vector<std::uint32_t> distance(spec::kVertices);
  std::memcpy(distance.data(), output.data(), spec::kRanksOffset);
  return distance;
}

[[nodiscard]] inline std::vector<double> ranks_of(const Bytes& output) {
  std::vector<double> rank(spec::kVertices);
  std::memcpy(rank.data(), output.data() + spec::kRanksOffset, spec::kVertices * sizeof(double));
  return rank;
}

}  // namespace loadforge::graph_test

#endif
