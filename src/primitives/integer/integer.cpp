// SPDX-License-Identifier: GPL-3.0-or-later
#include "primitives/integer/integer.hpp"

#include <array>
#include <bit>

#include "primitives/rng/philox.hpp"

namespace loadforge::primitives::integer {
namespace {

struct RunContext {
  const Config* config;
  std::span<std::uint64_t> partials;
};

void run_unit(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  auto& run_context = *static_cast<RunContext*>(context);
  run_context.partials[index] = unit_digest(*run_context.config, index);
}

}  // namespace

std::uint64_t unit_digest(const Config& config, std::uint64_t unit) noexcept {
  std::array<std::uint64_t, kLanes> lanes{};
  for (std::size_t lane = 0; lane < kLanes; ++lane) {
    lanes.at(lane) = rng::u64_of(rng::block_at(config.seed, unit, lane));
  }
  for (std::uint64_t step = 0; step < config.steps; ++step) {
    for (std::uint64_t& lane : lanes) {
      lane = lane * kMultiplier + kIncrement;
    }
  }
  return lanes.at(0) ^ std::rotl(lanes.at(1), 16) ^ std::rotl(lanes.at(2), 32) ^
         std::rotl(lanes.at(3), 48);
}

std::uint64_t run(worker::Executor& executor, const Config& config,
                  std::span<std::uint64_t> partials) noexcept {
  RunContext context{&config, partials};
  executor.run(partials.size(), run_unit, &context);
  std::uint64_t total = 0;
  for (const std::uint64_t partial : partials) {
    total += partial;
  }
  return total;
}

}  // namespace loadforge::primitives::integer
