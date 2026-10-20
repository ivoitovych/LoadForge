// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PRIMITIVES_CACHE_CACHE_HPP
#define LOADFORGE_PRIMITIVES_CACHE_CACHE_HPP

// The cache calibration load (PLAN §6, layer 1): a dependent pointer chase over
// a working set sized as a fraction of one cache level. Every load needs the
// previous load's result, so the chase measures latency, not bandwidth.
//
// GEOMETRY IS READ, NEVER ASSUMED (PLAN §4.5). The line size and the cache
// size come from the topology (topology::Cache, as CacheHierarchy reports
// them) through geometry_for_level(); nothing here says 64. A machine whose
// kernel does not publish a size or a line size is refused with a reason, which
// is a legitimate reading of real hardware (topology/cache.hpp), not a crash.
//
// THE DEFINITION. A unit has `nodes` nodes, node i occupying `line_bytes` of
// scratch with its successor index in the first four bytes. The nodes form one
// cycle of length `nodes`, built by Sattolo's algorithm: next[i] = i, then for i
// from nodes-1 down to 1, swap next[i] with next[j], where j =
// u64_of(block_at(seed, unit, i)) mod i. The unit starts at node 0, follows
// `steps` successors, and its digest is the node it ends on.
//
// DETERMINISM: BitExact -- integers only, and the digest is a pure function of
// (seed, unit, nodes, steps). The line size changes where nodes sit in memory,
// never which node the chase ends on.
// THREAD-SAFETY: each running unit owns one slot's scratch exclusively; the
// Executor guarantees no two concurrent calls share a slot.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/result.hpp"
#include "topology/cache.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::primitives::cache {

inline constexpr workloads::DeterminismClass kDeterminism = workloads::DeterminismClass::kBitExact;

/// Fewest nodes in a cycle worth chasing.
inline constexpr std::uint64_t kMinNodes = 2;
/// Most nodes: the successor index is four bytes.
inline constexpr std::uint64_t kMaxNodes = 0xFFFFFFFFULL;
/// Largest line size accepted; real lines are 32 to 256 bytes.
inline constexpr std::uint32_t kMaxLineBytes = 4096;

/// What the chase needs to know about one cache level.
struct Geometry {
  std::uint64_t cache_bytes = 0;
  std::uint32_t line_bytes = 0;
};

enum class GeometryError : std::uint8_t {
  kLevelAbsent,  ///< no data or unified cache at that level
  kSizeUnknown,  ///< the kernel did not publish its size
  kLineUnknown,  ///< the kernel did not publish its line size (or said zero)
};

/// The geometry of the first data or unified cache at `level` in `caches`
/// (CacheHierarchy::caches() order: by level, then lowest sharing CPU). An
/// instruction cache is skipped: a data chase through it is not what its level
/// means.
[[nodiscard]] core::Result<Geometry, GeometryError> geometry_for_level(
    const std::vector<topology::Cache>& caches, std::uint32_t level) noexcept;

/// Nodes that fill `percent` percent of the cache, rounded down: floor(lines *
/// percent / 100) with lines = cache_bytes / line_bytes, computed without
/// overflow. line_bytes must be non-zero, as geometry_for_level guarantees.
[[nodiscard]] std::uint64_t nodes_in(const Geometry& geometry, std::uint32_t percent) noexcept;

struct Config {
  std::uint64_t seed = 0;
  std::uint64_t nodes = 0;
  std::uint32_t line_bytes = 0;
  std::uint64_t steps = 0;
};

enum class ConfigError : std::uint8_t {
  kTooFewNodes,       ///< fewer than kMinNodes
  kTooManyNodes,      ///< more than kMaxNodes
  kLineBytesInvalid,  ///< not a multiple of four, zero, or above kMaxLineBytes
  kScratchTooSmall,   ///< the caller's scratch cannot hold every slot's nodes
};

/// 32-bit words of scratch one running unit needs, or why the configuration is
/// refused. `line_bytes / 4` words per node, `nodes` nodes.
[[nodiscard]] core::Result<std::uint64_t, ConfigError> scratch_words(const Config& config) noexcept;

/// The digest of one unit, using `scratch` (at least scratch_words(config)
/// words) for its nodes. Precondition: config is valid and scratch is large
/// enough; run() checks both, and tests call this one directly with valid input.
[[nodiscard]] std::uint64_t unit_digest(const Config& config, std::uint64_t unit,
                                        std::span<std::uint32_t> scratch) noexcept;

/// Runs partials.size() units, writing unit i's digest to partials[i], and
/// returns their wrapping sum in index order. `scratch` is split into
/// executor.threads() slots of scratch_words(config) words, a unit using its
/// slot's. A refused configuration runs nothing.
[[nodiscard]] core::Result<std::uint64_t, ConfigError> run(
    worker::Executor& executor, const Config& config, std::span<std::uint32_t> scratch,
    std::span<std::uint64_t> partials) noexcept;

}  // namespace loadforge::primitives::cache

#endif
