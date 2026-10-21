// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR 0003, rule 3: the provider's suite, run against the mock this module
// wrote of the contract. The controller consumes the sampler and the timeline
// sink (orchestration.hpp, RunOptions); a mock that fails a provider's test is
// a reading of the contract that differs from the provider's.

#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include "contracts/telemetry/sampler_contract.hpp"
#include "contracts/telemetry/timeline_contract.hpp"
#include "unit/controller/telemetry_mocks.hpp"

namespace loadforge::contracts {
namespace {

struct ControllerMockSampler {
  static std::unique_ptr<telemetry::Sampler> make(
      std::vector<std::unique_ptr<telemetry::Source>> sources, telemetry::TimelineSink& timeline) {
    return std::make_unique<mocks::MockSampler>(std::move(sources), timeline);
  }
};

struct ControllerMockTimeline {
  static std::unique_ptr<telemetry::Timeline> make(std::size_t capacity) {
    return std::make_unique<mocks::MockTimeline>(capacity);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(ControllerMock, SamplerContract,
                               ::testing::Types<ControllerMockSampler>);
INSTANTIATE_TYPED_TEST_SUITE_P(ControllerMock, TimelineContract,
                               ::testing::Types<ControllerMockTimeline>);

}  // namespace
}  // namespace loadforge::contracts
