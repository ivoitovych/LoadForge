// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/capability/capability.hpp"

#include <algorithm>
#include <cstddef>
#include <locale>
#include <ostream>
#include <sstream>
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
  const Metric* metric = nullptr;    ///< The first metric in the group.
  const Unavailable* why = nullptr;  ///< Why, for that first metric.
};

/// The present metrics of a source by name, in order of first appearance.
struct Presence {
  std::string name;
  std::size_t count = 0;
};

// The report is written to a stream and the groups are built in place, and not
// as `text += a + b + c` and `push_back(Group{...})`: a string or an aggregate
// of several strings built from temporaries carries a cleanup for the object
// that is half built, and the branches inside it are ones no test can take
// (docs/JOURNAL.md 1.13). A stream's destructor is a call, not a branch.

void write_present(std::ostream& out, const std::vector<Presence>& present) {
  out << "    present:";
  for (const Presence& entry : present) {
    out << ' ' << entry.name;
    if (entry.count > 1) {
      out << " x" << entry.count;
    }
  }
  out << '\n';
}

void write_missing(std::ostream& out, const Group& group) {
  const Metric& metric = *group.metric;
  const Unavailable& why = *group.why;
  out << "    " << group.name;
  if (group.count > 1) {
    out << " x" << group.count;
  } else {
    out << " (" << describe(metric.scope) << ' ' << metric.scope_index << ')';
  }
  out << ": " << topology::describe(group.kind) << '\n'
      << "      where:  " << why.path << '\n'
      << "      why:    " << why.detail << '\n'
      << "      remedy: " << group.remediation << '\n';
}

}  // namespace

std::string render_capabilities(const CapabilityReport& report) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  const CapabilityCounts totals = count(report);
  out << "telemetry capability: " << totals.present << " of " << totals.metrics
      << " metric(s) present\n";
  for (const SourceStatus& source : report.sources) {
    std::vector<Presence> present;
    std::vector<Group> missing;
    std::size_t present_count = 0;
    for (const MetricStatus& status : source.metrics) {
      if (!status.unavailable.has_value()) {
        ++present_count;
        const auto found = std::find_if(
            present.begin(), present.end(),
            [&status](const Presence& entry) { return entry.name == status.metric.name; });
        if (found == present.end()) {
          Presence& entry = present.emplace_back();
          entry.name = status.metric.name;
          entry.count = 1;
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
        Group& group = missing.emplace_back();
        group.name = status.metric.name;
        group.kind = why.kind;
        group.remediation = why.remediation;
        group.count = 1;
        group.metric = &status.metric;
        group.why = &why;
      } else {
        ++found->count;
      }
    }

    out << "  " << source.source << ": " << present_count << " of " << source.metrics.size()
        << " present\n";
    if (!present.empty()) {
      write_present(out, present);
    }
    for (const Group& group : missing) {
      write_missing(out, group);
    }
  }
  return out.str();
}

}  // namespace loadforge::telemetry
