// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/capability/capability.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "telemetry/sample.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

/// Metrics of one source that are not present for one reason.
struct Group {
  std::string name;
  topology::Availability kind = topology::Availability::kUnreadable;
  std::string remediation;
  std::size_t count = 0;
  const MetricStatus* first = nullptr;
};

/// The present metrics of a source by name, in order of first appearance.
struct Presence {
  std::string name;
  std::size_t count = 0;
};

}  // namespace

CapabilityReport probe_capabilities(std::span<Source* const> sources) {
  CapabilityReport report;
  for (Source* const source : sources) {
    SourceStatus status;
    status.source = std::string{source->name()};
    status.metrics = source->probe();
    report.sources.push_back(std::move(status));
  }
  return report;
}

std::string render_capabilities(const CapabilityReport& report) {
  const CapabilityCounts totals = count(report);
  std::string text = "telemetry capability: " + std::to_string(totals.present) + " of " +
                     std::to_string(totals.metrics) + " metric(s) present\n";
  for (const SourceStatus& source : report.sources) {
    std::vector<Presence> present;
    std::vector<Group> missing;
    for (const MetricStatus& status : source.metrics) {
      if (!status.unavailable.has_value()) {
        const auto found = std::find_if(present.begin(), present.end(), [&status](const Presence& p) {
          return p.name == status.metric.name;
        });
        if (found == present.end()) {
          present.push_back(Presence{status.metric.name, 1});
        } else {
          ++found->count;
        }
        continue;
      }
      const Unavailable& why = *status.unavailable;
      const auto found = std::find_if(missing.begin(), missing.end(), [&](const Group& group) {
        return group.name == status.metric.name && group.kind == why.kind &&
               group.remediation == why.remediation;
      });
      if (found == missing.end()) {
        missing.push_back(Group{status.metric.name, why.kind, why.remediation, 1, &status});
      } else {
        ++found->count;
      }
    }

    const CapabilityCounts counts = count(CapabilityReport{{source}});
    text += "  " + source.source + ": " + std::to_string(counts.present) + " of " +
            std::to_string(counts.metrics) + " present\n";
    if (!present.empty()) {
      text += "    present:";
      for (const Presence& entry : present) {
        text += " " + entry.name;
        if (entry.count > 1) {
          text += " x" + std::to_string(entry.count);
        }
      }
      text += "\n";
    }
    for (const Group& group : missing) {
      const Metric& metric = group.first->metric;
      const Unavailable& why = *group.first->unavailable;
      text += "    " + group.name;
      if (group.count > 1) {
        text += " x" + std::to_string(group.count);
      } else {
        text += " (" + std::string{describe(metric.scope)} + " " +
                std::to_string(metric.scope_index) + ")";
      }
      text += ": " + std::string{topology::describe(group.kind)} + "\n";
      text += "      where:  " + why.path + "\n";
      text += "      why:    " + why.detail + "\n";
      text += "      remedy: " + group.remediation + "\n";
    }
  }
  return text;
}

}  // namespace loadforge::telemetry
