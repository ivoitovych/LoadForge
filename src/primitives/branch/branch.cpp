// SPDX-License-Identifier: GPL-3.0-or-later
#include "primitives/branch/branch.hpp"

#include <algorithm>

#include "primitives/rng/philox.hpp"

namespace loadforge::primitives::branch {
namespace {

struct RunContext {
  const Config* config;
  std::span<std::uint64_t> scratch;
  std::span<Tally> partials;
};

void run_unit(void* context, std::size_t index, std::size_t slot) noexcept {
  auto& run_context = *static_cast<RunContext*>(context);
  const std::size_t share = run_context.config->elements;
  run_context.partials[index] =
      unit_tally(*run_context.config, index, run_context.scratch.subspan(slot * share, share));
}

}  // namespace

Tally unit_tally(const Config& config, std::uint64_t unit,
                 std::span<std::uint64_t> scratch) noexcept {
  const std::span<std::uint64_t> values = scratch.first(config.elements);
  for (std::uint64_t j = 0; j < config.elements; ++j) {
    values[j] = rng::u64_of(rng::block_at(config.seed, unit, j));
  }
  if (config.sorted) {
    std::sort(values.begin(), values.end());
  }
  Tally tally;
  for (const std::uint64_t value : values) {
    if (value < config.threshold) {
      ++tally.taken;
      tally.below += value;
    } else {
      tally.above ^= value;
    }
  }
  return tally;
}

Tally combine(const Tally& left, const Tally& right) noexcept {
  return Tally{left.taken + right.taken, left.below + right.below, left.above ^ right.above};
}

core::Result<Tally, ConfigError> run(worker::Executor& executor, const Config& config,
                                     std::span<std::uint64_t> scratch,
                                     std::span<Tally> partials) noexcept {
  if (config.elements != 0 && scratch.size() / config.elements < executor.threads()) {
    return ConfigError::kScratchTooSmall;
  }
  RunContext context{&config, scratch, partials};
  executor.run(partials.size(), run_unit, &context);
  Tally total;
  for (const Tally& partial : partials) {
    total = combine(total, partial);
  }
  return total;
}

// No short-circuit and no boolean arithmetic: the fields differ in some bit or none,
// so there is no branch for a test to miss.
[[nodiscard]] bool operator==(const Tally& left, const Tally& right) noexcept {
  return ((left.taken ^ right.taken) | (left.below ^ right.below) | (left.above ^ right.above)) ==
         0;
}

}  // namespace loadforge::primitives::branch
