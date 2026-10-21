// SPDX-License-Identifier: GPL-3.0-or-later
#include "primitives/cache/cache.hpp"

#include <optional>
#include <utility>

#include "primitives/rng/philox.hpp"

namespace loadforge::primitives::cache {
namespace {

constexpr std::uint32_t kBytesPerWord = 4;

struct RunContext {
  const Config* config;
  std::span<std::uint32_t> scratch;
  std::uint64_t words_per_slot;
  std::span<std::uint64_t> partials;
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the signature is Executor::Task's.
void run_unit(void* context, std::size_t index, std::size_t slot) noexcept {
  auto& run_context = *static_cast<RunContext*>(context);
  const std::span<std::uint32_t> mine =
      run_context.scratch.subspan(slot * run_context.words_per_slot, run_context.words_per_slot);
  run_context.partials[index] = unit_digest(*run_context.config, index, mine);
}

std::optional<ConfigError> refusal_of(const Config& config) noexcept {
  if (config.nodes < kMinNodes) {
    return ConfigError::kTooFewNodes;
  }
  if (config.nodes > kMaxNodes) {
    return ConfigError::kTooManyNodes;
  }
  if (config.line_bytes == 0 || config.line_bytes % kBytesPerWord != 0 ||
      config.line_bytes > kMaxLineBytes) {
    return ConfigError::kLineBytesInvalid;
  }
  return std::nullopt;
}

std::uint64_t words_of(const Config& config) noexcept {
  return config.nodes * (config.line_bytes / kBytesPerWord);
}

}  // namespace

core::Result<Geometry, GeometryError> geometry_for_level(const std::vector<topology::Cache>& caches,
                                                         std::uint32_t level) noexcept {
  for (const topology::Cache& candidate : caches) {
    if (candidate.kind.level != level || candidate.kind.type == topology::CacheType::kInstruction) {
      continue;
    }
    if (!candidate.size_bytes.has_value()) {
      return GeometryError::kSizeUnknown;
    }
    if (!candidate.line_bytes.has_value() || *candidate.line_bytes == 0) {
      return GeometryError::kLineUnknown;
    }
    return Geometry{*candidate.size_bytes, *candidate.line_bytes};
  }
  return GeometryError::kLevelAbsent;
}

std::uint64_t nodes_in(const Geometry& geometry, std::uint32_t percent) noexcept {
  const std::uint64_t lines = geometry.cache_bytes / geometry.line_bytes;
  return (lines / 100) * percent + ((lines % 100) * percent) / 100;
}

core::Result<std::uint64_t, ConfigError> scratch_words(const Config& config) noexcept {
  if (const std::optional<ConfigError> refusal = refusal_of(config)) {
    return *refusal;
  }
  return words_of(config);
}

std::uint64_t unit_digest(const Config& config, std::uint64_t unit,
                          std::span<std::uint32_t> scratch) noexcept {
  const std::uint64_t stride = config.line_bytes / kBytesPerWord;
  for (std::uint64_t node = 0; node < config.nodes; ++node) {
    scratch[node * stride] = static_cast<std::uint32_t>(node);
  }
  for (std::uint64_t node = config.nodes - 1; node >= 1; --node) {
    const std::uint64_t other = rng::u64_of(rng::block_at(config.seed, unit, node)) % node;
    std::swap(scratch[node * stride], scratch[other * stride]);
  }
  std::uint64_t position = 0;
  for (std::uint64_t step = 0; step < config.steps; ++step) {
    position = scratch[position * stride];
  }
  return position;
}

core::Result<std::uint64_t, ConfigError> run(worker::Executor& executor, const Config& config,
                                             std::span<std::uint32_t> scratch,
                                             std::span<std::uint64_t> partials) noexcept {
  if (const std::optional<ConfigError> refusal = refusal_of(config)) {
    return *refusal;
  }
  const std::uint64_t words = words_of(config);
  if (scratch.size() / words < executor.threads()) {
    return ConfigError::kScratchTooSmall;
  }
  RunContext context{&config, scratch, words, partials};
  executor.run(partials.size(), run_unit, &context);
  std::uint64_t total = 0;
  for (const std::uint64_t partial : partials) {
    total += partial;
  }
  return total;
}

}  // namespace loadforge::primitives::cache
