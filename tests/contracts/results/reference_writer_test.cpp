// SPDX-License-Identifier: GPL-3.0-or-later
//
// A trivial reference result writer, run against the writer suite. Its
// format, one "key=value" line per fact, is not a LoadForge format: it exists
// to prove the suite can be passed.

#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <variant>

#include "contracts/results/writer_contract.hpp"
#include "controller/results/result.hpp"
#include "reporters/writer.hpp"

namespace loadforge::contracts {
namespace {

using results::RunResult;

void put_unavailable(std::ostream& out, const std::string& key, const telemetry::Unavailable& u) {
  out << key << ".unavailable=" << topology::describe(u.kind) << '\n'
      << key << ".path=" << u.path << '\n'
      << key << ".detail=" << u.detail << '\n'
      << key << ".remediation=" << u.remediation << '\n';
}

class KeyValueWriter final : public reporters::ResultWriter {
 public:
  [[nodiscard]] std::string_view format() const noexcept override { return "kv"; }
  [[nodiscard]] core::Result<core::Ok, reporters::WriteError> write(
      const RunResult& run, std::ostream& out) const override {
    if (!out.good()) {
      return reporters::WriteError{"the stream was not writable"};
    }
    out << "schema_version=" << run.schema_version << '\n'
        << "mode=" << config::describe(run.mode) << '\n'
        << "elapsed_ns=" << run.elapsed_ns << '\n'
        << "loadforge_version=" << run.fingerprint.loadforge_version << '\n'
        << "cpu_model=" << run.fingerprint.cpu_model.value_or("(absent)") << '\n';
    for (const telemetry::SourceStatus& source : run.capabilities.sources) {
      for (const telemetry::MetricStatus& status : source.metrics) {
        const std::string key = "capability." + status.metric.name;
        if (status.unavailable.has_value()) {
          put_unavailable(out, key, *status.unavailable);
        } else {
          out << key << "=present\n";
        }
      }
    }
    for (const statistics::MetricSummary& summary : run.signature.metrics) {
      const std::string key = "signature." + summary.metric.name;
      if (const auto* unavailable = std::get_if<telemetry::Unavailable>(&summary.value)) {
        put_unavailable(out, key, *unavailable);
      } else if (const auto* counter = std::get_if<statistics::CounterSummary>(&summary.value)) {
        out << key << ".delta=" << counter->delta << '\n';
      } else {
        out << key << ".mean=" << std::get<statistics::GaugeSummary>(summary.value).mean << '\n';
      }
    }
    for (const results::WorkloadResult& workload : run.workloads) {
      out << "workload=" << workload.descriptor.name << '\n'
          << "units=" << workload.tally.units_computed << '\n';
      for (const results::Finding& finding : workload.findings) {
        out << "finding=" << verification::kind_name(finding.record) << '\n';
      }
    }
    for (const results::WorkerResult& worker : run.workers) {
      out << "worker." << worker.slot << '=' << controller::describe(worker.fate) << '\n';
    }
    out << "verdict=" << results::describe(run.verdict) << '\n';
    out.flush();
    if (!out.good()) {
      return reporters::WriteError{"the stream refused the output"};
    }
    return core::Ok{};
  }
};

struct ReferenceWriter {
  static constexpr const char* kFormat = "kv";
  static std::unique_ptr<reporters::ResultWriter> make() {
    return std::make_unique<KeyValueWriter>();
  }
};

INSTANTIATE_TYPED_TEST_SUITE_P(Reference, ResultWriterContract, ::testing::Types<ReferenceWriter>);

}  // namespace
}  // namespace loadforge::contracts
