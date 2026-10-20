// SPDX-License-Identifier: GPL-3.0-or-later
#include "primitives/floating/floating.hpp"

#include <array>
#include <cmath>

#include "primitives/rng/philox.hpp"

namespace loadforge::primitives::floating {
namespace {

struct RunContext {
  const Config* config;
  std::span<double> partials;
};

void run_unit(void* context, std::size_t index, std::size_t /*slot*/) noexcept {
  auto& run_context = *static_cast<RunContext*>(context);
  run_context.partials[index] = unit_value(*run_context.config, index);
}

std::uint64_t pair_bits(std::uint32_t low, std::uint32_t high) noexcept {
  return std::uint64_t{low} | (std::uint64_t{high} << 32U);
}

}  // namespace

double unit_value(const Config& config, std::uint64_t unit) noexcept {
  std::array<double, kAccumulators> acc{};
  for (std::uint64_t element = 0; element < config.elements; ++element) {
    const rng::Block block = rng::block_at(config.seed, unit, element);
    const double x = rng::unit_interval(pair_bits(block[0], block[1]));
    const double y = rng::unit_interval(pair_bits(block[2], block[3]));
    double& lane = acc[element % kAccumulators];
    lane = std::fma(x, y, lane);
  }
  return ((acc[0] + acc[1]) + (acc[2] + acc[3])) + ((acc[4] + acc[5]) + (acc[6] + acc[7]));
}

double run(worker::Executor& executor, const Config& config,
           std::span<double> partials) noexcept {
  RunContext context{&config, partials};
  executor.run(partials.size(), run_unit, &context);
  double total = 0.0;
  for (const double partial : partials) {
    total += partial;
  }
  return total;
}

}  // namespace loadforge::primitives::floating
