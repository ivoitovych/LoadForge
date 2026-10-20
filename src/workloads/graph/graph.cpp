// SPDX-License-Identifier: GPL-3.0-or-later
#include "workloads/graph/graph.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "primitives/rng/philox.hpp"
#include "primitives/simd/dispatch.hpp"
#include "worker/executor.hpp"
#include "workloads/graph/kernels.hpp"
#include "workloads/graph/spec.hpp"

// THE UNIT'S COMPUTATION
// ----------------------
// compute() works in the scratch it is given and writes the output last:
//
//   1. the in-edge lists (a counting sort of the edges by head, in increasing
//      edge number, which is the order the specification pins the PageRank
//      terms in), serially -- a few hundred thousand integer operations;
//   2. BFS, level by level, switching between a top-down step over the
//      frontier's out-edges and a bottom-up step over the unvisited vertices'
//      in-edges (direction optimisation). Distances are a property of the
//      graph, so the order edges are examined in changes nothing in the output;
//   3. kIterations Jacobi sweeps of PageRank, each split over vertex ranges.
//      Every vertex's sum is computed by one task in the order the
//      specification pins, so the schedule changes nothing either.
//
// ATOMICS, AND WHY relaxed IS ENOUGH (PLAN §4.5)
// ---------------------------------------------
// Inside one BFS step the threads share three things, and nothing else:
//   * dist[], read and written through std::atomic_ref with memory_order_relaxed;
//   * the discovery counter, fetch_add with memory_order_relaxed;
//   * the slots of the next frontier, plain stores.
// A step is bracketed by Executor::run, which orders everything a task did
// before run returns and everything the caller did before run is called
// (executor.hpp), so values the step reads that earlier steps wrote -- the
// frontier, the distances of levels below the current one -- need no ordering
// of their own. Within a step:
//   * top-down: a vertex is claimed by compare_exchange of dist[h] from
//     kUnreachable to level + 1. Exactly one task wins it (the modification
//     order of one location is total whatever the memory order), and only the
//     winner appends h. A loser, or a task that sees a stale kUnreachable
//     and loses the exchange, appends nothing.
//   * bottom-up: dist[v] is written by the task that owns v alone. A task
//     reads dist[u] of other vertices and asks whether it equals the current
//     level; those were written before the step began, and the only writes
//     during the step store level + 1, which is never equal to level -- so
//     the answer is the same whichever value of dist[u] the load observes.
//   * the counter hands out each slot of the next frontier to one task, so
//     two tasks never store to the same slot; the caller reads the slots and
//     the count after run returns.
// No value read in a step depends on the order of two writes of that step.
// The ordering test is the repeated adversarial-schedule comparison in
// tests/determinism/workloads/graph, together with the TSan job.

namespace loadforge::workloads::graph {
namespace {

static_assert(std::endian::native == std::endian::little,
              "the unit's words are little-endian and are copied, not converted");
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(kVertices % 512 == 0);

/// Descriptor::version: the version of this workload's bits, which moves
/// whenever a configuration's output does (workload.hpp). It starts at the
/// specification's own.
constexpr std::uint32_t kWorkloadVersion = kVersion;

constexpr std::size_t kLine = 64;
constexpr std::size_t kWordBytes = sizeof(std::uint32_t);

/// Vertices per PageRank task and per bottom-up task.
constexpr std::size_t kVertexChunk = 512;
/// Frontier vertices per top-down task.
constexpr std::size_t kFrontierChunk = 64;
/// A top-down step with fewer frontier vertices than this runs on the calling
/// thread: handing it to the executor would cost more than the step.
constexpr std::size_t kParallelFrontier = 256;
/// Bottom-up steps need a frontier of at least this many vertices and more than
/// 1/kAlpha of the unvisited ones. The first bound caps the number of such
/// steps at kVertices / kBottomUpMinimum (frontiers are disjoint), so no input
/// -- a path, say -- can make the work grow with the number of levels.
constexpr std::size_t kBottomUpMinimum = 512;
constexpr std::size_t kAlpha = 14;

[[nodiscard]] constexpr std::size_t padded(std::size_t bytes) noexcept {
  return (bytes + kLine - 1) / kLine * kLine;
}

constexpr std::size_t kOffsetsBytes = padded((kVertices + 1) * kWordBytes);
constexpr std::size_t kTailsBytes = padded(kEdges * kWordBytes);
constexpr std::size_t kRegionBytes = padded(kVertices * sizeof(double));
constexpr std::size_t kDistBytes = padded(kVertices * kWordBytes);
/// The scratch of one executor slot. The unit uses one arena of it -- the
/// executor's slots together are scratch_bytes * threads(), and compute() uses
/// the first scratch_bytes of that -- plus the room to align its start.
constexpr std::size_t kScratchBytes =
    kLine + kOffsetsBytes + kTailsBytes + 2 * kRegionBytes + kDistBytes;

[[nodiscard]] std::uint32_t word_at(std::span<const std::byte> bytes, std::size_t index) noexcept {
  std::uint32_t word = 0;
  std::memcpy(&word, bytes.subspan(index * kWordBytes, kWordBytes).data(), kWordBytes);
  return word;
}

void put_word(std::span<std::byte> bytes, std::size_t index, std::uint32_t word) noexcept {
  std::memcpy(bytes.subspan(index * kWordBytes, kWordBytes).data(), &word, kWordBytes);
}

/// The head of edge `edge`, reduced modulo V (TOTALITY, spec.hpp).
[[nodiscard]] std::uint32_t head_at(std::span<const std::byte> input, std::size_t edge) noexcept {
  return effective_vertex(word_at(input, edge));
}

/// A typed view of `bytes`; the arena is aligned to a cache line and every
/// region starts on one, so the pointer is aligned for any type used here.
template <typename T>
[[nodiscard]] std::span<T> view_as(std::span<std::byte> bytes, std::size_t count) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): typed views of scratch.
  return {reinterpret_cast<T*>(bytes.data()), count};
}

/// The scratch, carved. The two regions are reused by phase: region A holds
/// the BFS frontiers and then the ranks of even iterations, region B the
/// counting sort's cursors and then the ranks of odd iterations.
struct Arena {
  std::span<std::uint32_t> offsets;
  std::span<std::uint32_t> tails;
  std::span<std::byte> region_a;
  std::span<std::byte> region_b;
  std::span<std::uint32_t> dist;
};

[[nodiscard]] Arena carve(std::span<std::byte> scratch) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the address's low bits.
  const auto address = reinterpret_cast<std::uintptr_t>(scratch.data());
  std::span<std::byte> rest = scratch.subspan((kLine - address % kLine) % kLine);
  const auto take = [&rest](std::size_t bytes) noexcept {
    const std::span<std::byte> piece = rest.first(bytes);
    rest = rest.subspan(bytes);
    return piece;
  };
  Arena arena;
  arena.offsets = view_as<std::uint32_t>(take(kOffsetsBytes), kVertices + 1);
  arena.tails = view_as<std::uint32_t>(take(kTailsBytes), kEdges);
  arena.region_a = take(kRegionBytes);
  arena.region_b = take(kRegionBytes);
  arena.dist = view_as<std::uint32_t>(take(kDistBytes), kVertices);
  return arena;
}

/// Groups the edges by head. Edges are visited in increasing number, so each
/// head's tails come out in the order the PageRank terms are taken in.
void build_in_edges(std::span<const std::byte> input, const Arena& arena) noexcept {
  const std::span<std::uint32_t> cursor = view_as<std::uint32_t>(arena.region_b, kVertices);
  std::ranges::fill(arena.offsets, 0U);
  for (std::size_t edge = 0; edge < kEdges; ++edge) {
    ++arena.offsets[head_at(input, edge) + 1];
  }
  for (std::size_t vertex = 0; vertex < kVertices; ++vertex) {
    arena.offsets[vertex + 1] += arena.offsets[vertex];
    cursor[vertex] = arena.offsets[vertex];
  }
  for (std::size_t edge = 0; edge < kEdges; ++edge) {
    arena.tails[cursor[head_at(input, edge)]++] = tail_of(edge);
  }
}

// ---- BFS --------------------------------------------------------------------

[[nodiscard]] std::uint32_t load_dist(std::span<std::uint32_t> dist, std::size_t vertex) noexcept {
  return std::atomic_ref<std::uint32_t>(dist[vertex]).load(std::memory_order_relaxed);
}

/// One BFS step, for both directions: what the tasks of the step share.
struct Step {
  std::span<const std::byte> input;
  const Arena* arena = nullptr;
  std::span<const std::uint32_t> frontier;  ///< the vertices at distance `level`
  std::span<std::uint32_t> next;            ///< their discoveries, at distance level + 1
  std::uint32_t level = 0;
  std::atomic<std::uint32_t> found{0};
};

void append(Step& step, std::uint32_t vertex) noexcept {
  step.next[step.found.fetch_add(1, std::memory_order_relaxed)] = vertex;
}

/// Top-down task `index`: the out-edges of its slice of the frontier.
void top_down_task(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  auto& step = *static_cast<Step*>(context);
  const std::size_t first = index * kFrontierChunk;
  const std::size_t last = std::min(first + kFrontierChunk, step.frontier.size());
  for (std::size_t at = first; at < last; ++at) {
    for (std::size_t edge = step.frontier[at] * kOutDegree, stop = edge + kOutDegree; edge < stop;
         ++edge) {
      const std::uint32_t head = head_at(step.input, edge);
      std::uint32_t expected = kUnreachable;
      if (std::atomic_ref<std::uint32_t>(step.arena->dist[head])
              .compare_exchange_strong(expected, step.level + 1, std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
        append(step, head);
      }
    }
  }
}

/// Bottom-up task `index`: each unvisited vertex of its range looks for an
/// in-neighbour at the current level.
void bottom_up_task(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  auto& step = *static_cast<Step*>(context);
  const Arena& arena = *step.arena;
  for (std::size_t vertex = index * kVertexChunk, stop = vertex + kVertexChunk; vertex < stop;
       ++vertex) {
    if (load_dist(arena.dist, vertex) != kUnreachable) {
      continue;
    }
    for (std::size_t at = arena.offsets[vertex]; at < arena.offsets[vertex + 1]; ++at) {
      if (load_dist(arena.dist, arena.tails[at]) == step.level) {
        std::atomic_ref<std::uint32_t>(arena.dist[vertex])
            .store(step.level + 1, std::memory_order_relaxed);
        append(step, static_cast<std::uint32_t>(vertex));
        break;
      }
    }
  }
}

void run_bfs(std::span<const std::byte> input, const Arena& arena, worker::Executor& executor) {
  const std::span<std::uint32_t> lists = view_as<std::uint32_t>(arena.region_a, 2 * kVertices);
  std::span<std::uint32_t> frontier = lists.first(kVertices);
  std::span<std::uint32_t> spare = lists.last(kVertices);
  std::ranges::fill(arena.dist, kUnreachable);
  const std::uint32_t source = effective_vertex(word_at(input, kEdges));
  arena.dist[source] = 0;
  frontier[0] = source;
  std::size_t size = 1;
  std::size_t visited = 1;
  Step step;
  step.input = input;
  step.arena = &arena;
  for (std::uint32_t level = 0; size > 0; ++level) {
    step.frontier = frontier.first(size);
    step.next = spare;
    step.level = level;
    step.found.store(0, std::memory_order_relaxed);
    if (size >= kBottomUpMinimum && size * kAlpha > kVertices - visited) {
      executor.run(kVertices / kVertexChunk, &bottom_up_task, &step);
    } else if (size >= kParallelFrontier) {
      executor.run((size + kFrontierChunk - 1) / kFrontierChunk, &top_down_task, &step);
    } else {
      for (std::size_t index = 0; index * kFrontierChunk < size; ++index) {
        top_down_task(&step, index, 0);
      }
    }
    size = step.found.load(std::memory_order_relaxed);
    visited += size;
    std::swap(frontier, spare);
  }
}

// ---- PageRank ---------------------------------------------------------------

struct Sweep {
  InEdges edges;
  SweepFn kernel = nullptr;
  std::uint32_t accumulators = 0;
  std::span<const double> cur;
  std::span<double> next;
};

void sweep_task(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  const auto& sweep = *static_cast<const Sweep*>(context);
  const std::size_t begin = index * kVertexChunk;
  sweep.kernel(sweep.edges, sweep.cur, sweep.next, sweep.accumulators, begin, begin + kVertexChunk);
}

/// Runs kIterations sweeps and returns the ranks, in whichever region holds
/// them (kIterations is even, so region A -- but the caller need not know).
[[nodiscard]] std::span<const double> run_pagerank(const Arena& arena, SweepFn kernel,
                                                   std::uint32_t accumulators,
                                                   worker::Executor& executor) {
  std::span<double> cur = view_as<double>(arena.region_a, kVertices);
  std::span<double> next = view_as<double>(arena.region_b, kVertices);
  std::ranges::fill(cur, kInitialRank);
  Sweep sweep;
  sweep.edges = InEdges{arena.offsets, arena.tails};
  sweep.kernel = kernel;
  sweep.accumulators = accumulators;
  for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
    sweep.cur = cur;
    sweep.next = next;
    executor.run(kVertices / kVertexChunk, &sweep_task, &sweep);
    std::swap(cur, next);
  }
  return cur;
}

// ---- the workload -----------------------------------------------------------

/// The memory traffic of one unit by its algorithm, in bytes (F12): the input
/// read twice (the sort, the BFS), the in-edge lists written and read, the
/// distances, and per sweep every tail and gathered rank read and every rank
/// written -- plus the output.
constexpr std::uint64_t kLogicalBytes =
    2 * kInputBytes + 2 * (kOffsetsBytes + kEdges * kWordBytes) + 2 * kVertices * kWordBytes +
    kIterations *
        (kEdges * (kWordBytes + sizeof(double)) + kVertices * (kWordBytes + sizeof(double))) +
    kOutputBytes;

class GraphWorkload final : public Workload {
 public:
  GraphWorkload(Descriptor descriptor, const Plan& plan, std::uint64_t seed, SweepFn kernel)
      : descriptor_(std::move(descriptor)), plan_(plan), seed_(seed), kernel_(kernel) {}

  [[nodiscard]] const Descriptor& descriptor() const noexcept override { return descriptor_; }
  [[nodiscard]] const Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    for (std::size_t draw = 0; draw <= kEdges; ++draw) {
      put_word(input, draw, vertex_of(rng::u64_of(rng::block_at(seed_, index, draw))));
    }
  }

  // The signature is the contract's (workload.hpp): two byte spans, output then scratch.
  // NOLINTBEGIN(bugprone-easily-swappable-parameters)
  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> scratch,
               worker::Executor& executor) const noexcept override {
    const Arena arena = carve(scratch);
    build_in_edges(input, arena);
    run_bfs(input, arena, executor);
    const std::span<const double> ranks =
        run_pagerank(arena, kernel_, plan_.decomposition, executor);
    std::memcpy(output.first(kRanksOffset).data(), arena.dist.data(), kRanksOffset);
    std::memcpy(output.subspan(kRanksOffset).data(), ranks.data(), kVertices * sizeof(double));
  }
  // NOLINTEND(bugprone-easily-swappable-parameters)

 private:
  Descriptor descriptor_;
  Plan plan_;
  std::uint64_t seed_;
  SweepFn kernel_;
};

}  // namespace

core::Result<std::unique_ptr<Workload>, WorkloadError> create(const WorkloadConfig& config) {
  const std::uint32_t accumulators =
      config.decomposition == 0 ? kDefaultAccumulators : config.decomposition;
  if (!is_accumulators(accumulators)) {
    return WorkloadError{WorkloadFault::kConfiguration,
                         "graph: decomposition " + std::to_string(config.decomposition) +
                             " is not a number of accumulators (1, 2, 4 or 8)"};
  }
  const simd::KernelTable<SweepFn>& kernels = sweep_kernels();
  const SweepFn kernel = simd::kernel_at(kernels, config.isa);
  if (kernel == nullptr) {
    return WorkloadError{
        WorkloadFault::kConfiguration,
        "graph: no " + std::string(simd::describe(config.isa)) + " kernel in this build"};
  }
  Plan plan;
  plan.input_bytes = kInputBytes;
  plan.output_bytes = kOutputBytes;
  plan.scratch_bytes = kScratchBytes;
  plan.units = config.footprint_bytes / (kInputBytes + kOutputBytes);
  plan.decomposition = accumulators;
  plan.logical_bytes = kLogicalBytes;
  plan.operations = kIterations * (2 * kEdges + (accumulators + 1) * kVertices) + kEdges;
  if (plan.units == 0) {
    return WorkloadError{WorkloadFault::kFootprintTooSmall,
                         "graph: a unit needs " + std::to_string(kInputBytes + kOutputBytes) +
                             " bytes of footprint, " + std::to_string(config.footprint_bytes) +
                             " given"};
  }
  Descriptor descriptor;
  descriptor.name = std::string(kName);
  descriptor.version = kWorkloadVersion;
  descriptor.determinism = DeterminismClass::kBitExactFixedDecomposition;
  descriptor.contract = verification::Contract::kExact;
  descriptor.paths = simd::kernel_paths(kernels);
  return std::unique_ptr<Workload>(
      std::make_unique<GraphWorkload>(std::move(descriptor), plan, config.seed, kernel));
}

}  // namespace loadforge::workloads::graph
