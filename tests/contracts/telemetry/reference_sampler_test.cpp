// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference sampler, run against the sampler suite.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "contracts/telemetry/sampler_contract.hpp"
#include "telemetry/sampler.hpp"

namespace loadforge::contracts {
namespace {

using telemetry::CapabilityReport;
using telemetry::TimelineError;

class ReferenceSamplerImpl final : public telemetry::Sampler {
 public:
  ReferenceSamplerImpl(std::vector<std::unique_ptr<telemetry::Source>> sources,
                       telemetry::TimelineSink& timeline)
      : sources_(std::move(sources)), timeline_(&timeline) {}

  core::Result<CapabilityReport, TimelineError> probe() override {
    if (report_.has_value()) {
      return *report_;
    }
    CapabilityReport report;
    for (const auto& source : sources_) {
      std::vector<telemetry::SeriesId> ids;
      const std::vector<telemetry::MetricStatus> statuses = source->probe();
      for (const telemetry::MetricStatus& status : statuses) {
        if (!status.unavailable.has_value()) {
          auto id = timeline_->add_series(status.metric);
          if (!id.has_value()) {
            return id.error();
          }
          ids.push_back(id.value());
        }
      }
      series_.push_back(ids);
      report.sources.push_back({std::string(source->name()), statuses});
    }
    report_ = report;
    return report;
  }

  core::Result<std::size_t, TimelineError> round(std::int64_t now_ns) override {
    if (!report_.has_value()) {
      return std::size_t{0};
    }
    std::size_t appended = 0;
    for (std::size_t at = 0; at < sources_.size(); ++at) {
      std::vector<telemetry::Reading> readings;
      sources_[at]->sample(readings);
      const std::vector<telemetry::SeriesId>& ids = series_[at];
      if (readings.size() != ids.size()) {
        const std::string detail = std::string(sources_[at]->name()) + " returned " +
                                   std::to_string(readings.size()) + " readings for " +
                                   std::to_string(ids.size()) + " metrics";
        readings.assign(ids.size(), telemetry::Unavailable{topology::Availability::kMalformed, "",
                                                           detail, "report a bug in the source"});
      }
      for (std::size_t metric = 0; metric < ids.size(); ++metric) {
        auto added = timeline_->append(ids[metric], {now_ns, readings[metric]});
        if (!added.has_value()) {
          return added.error();
        }
        ++appended;
      }
    }
    return appended;
  }

 private:
  std::vector<std::unique_ptr<telemetry::Source>> sources_;
  telemetry::TimelineSink* timeline_;
  std::vector<std::vector<telemetry::SeriesId>> series_;
  std::optional<CapabilityReport> report_;
};

struct ReferenceSampler {
  static std::unique_ptr<telemetry::Sampler> make(
      std::vector<std::unique_ptr<telemetry::Source>> sources, telemetry::TimelineSink& timeline) {
    return std::make_unique<ReferenceSamplerImpl>(std::move(sources), timeline);
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, SamplerContract, ::testing::Types<ReferenceSampler>);

}  // namespace
}  // namespace loadforge::contracts
