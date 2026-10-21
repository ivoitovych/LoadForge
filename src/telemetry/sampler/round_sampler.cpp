// SPDX-License-Identifier: GPL-3.0-or-later
// The provider of telemetry/sampler.hpp. No thread, no clock: the caller
// drives every round and says what time it is.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "telemetry/sampler.hpp"

namespace loadforge::telemetry {
namespace {

using ProbeOutcome = core::Result<CapabilityReport, TimelineError>;

class RoundSampler final : public Sampler {
 public:
  RoundSampler(std::vector<std::unique_ptr<Source>> sources, TimelineSink& timeline) noexcept
      : sources_(std::move(sources)), timeline_(&timeline) {}

  core::Result<CapabilityReport, TimelineError> probe() override {
    if (!outcome_.has_value()) {
      outcome_ = probe_sources();
    }
    return *outcome_;
  }

  core::Result<std::size_t, TimelineError> round(std::int64_t now_ns) override {
    if (!outcome_.has_value() || !outcome_->has_value()) {
      return std::size_t{0};
    }
    std::size_t appended = 0;
    for (std::size_t at = 0; at < sources_.size(); ++at) {
      readings_.clear();
      sources_[at]->sample(readings_);
      const std::vector<SeriesId>& ids = series_[at];
      if (readings_.size() != ids.size()) {
        replace_with_malformed(*sources_[at], ids.size());
      }
      for (std::size_t metric = 0; metric < ids.size(); ++metric) {
        auto added = timeline_->append(ids[metric], Point{now_ns, readings_[metric]});
        if (!added.has_value()) {
          return added.error();
        }
        ++appended;
      }
    }
    return appended;
  }

 private:
  // Probes in order and registers the present metrics, stopping at the first
  // series the timeline refuses: nothing more is registered, and the same
  // refusal is the answer to every later probe() (the sources were probed
  // once and are not probed again).
  ProbeOutcome probe_sources() {
    CapabilityReport report;
    for (const std::unique_ptr<Source>& source : sources_) {
      std::vector<MetricStatus> statuses = source->probe();
      std::vector<SeriesId> ids;
      for (const MetricStatus& status : statuses) {
        if (status.unavailable.has_value()) {
          continue;
        }
        auto id = timeline_->add_series(status.metric);
        if (!id.has_value()) {
          return id.error();
        }
        ids.push_back(id.value());
      }
      series_.push_back(std::move(ids));
      SourceStatus& entry = report.sources.emplace_back();
      entry.source = source->name();
      entry.metrics = std::move(statuses);
    }
    return report;
  }

  // A source that returned the wrong number of readings has none of them
  // trusted: each of its series gets the same Unavailable naming it.
  void replace_with_malformed(const Source& source, std::size_t expected) {
    std::string detail = std::string(source.name()) + " returned " +
                         std::to_string(readings_.size()) + " readings for " +
                         std::to_string(expected) + " present metrics";
    Unavailable malformed;
    malformed.kind = topology::Availability::kMalformed;
    malformed.detail = std::move(detail);
    malformed.remediation = "this is a defect in the ";
    malformed.remediation += source.name();
    malformed.remediation += " source: report it with the capability report";
    readings_.assign(expected, Reading{std::move(malformed)});
  }

  std::vector<std::unique_ptr<Source>> sources_;
  TimelineSink* timeline_;
  std::optional<ProbeOutcome> outcome_;
  std::vector<std::vector<SeriesId>> series_;
  std::vector<Reading> readings_;
};

}  // namespace

std::unique_ptr<Sampler> make_sampler(std::vector<std::unique_ptr<Source>> sources,
                                      TimelineSink& timeline) {
  return std::make_unique<RoundSampler>(std::move(sources), timeline);
}

}  // namespace loadforge::telemetry
