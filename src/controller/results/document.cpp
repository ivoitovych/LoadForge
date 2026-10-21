// SPDX-License-Identifier: GPL-3.0-or-later
#include "controller/results/document.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

#include "config/config.hpp"
#include "controller/supervision.hpp"
#include "primitives/simd/dispatch.hpp"
#include "telemetry/timeline.hpp"
#include "topology/source.hpp"
#include "verification/isolation.hpp"
#include "verification/record.hpp"
#include "workloads/workload.hpp"

namespace loadforge::results {

Node Node::boolean(bool value) {
  Node node;
  node.value_ = value;
  return node;
}

Node Node::integer(std::int64_t value) {
  Node node;
  node.value_ = value;
  return node;
}

Node Node::unsigned_integer(std::uint64_t value) {
  Node node;
  node.value_ = value;
  return node;
}

Node Node::real(double value) {
  Node node;
  if (std::isnan(value)) {
    node.value_ = std::string{"nan"};
  } else if (std::isinf(value)) {
    node.value_ = std::string{value > 0 ? "inf" : "-inf"};
  } else {
    node.value_ = value;
  }
  return node;
}

Node Node::text(std::string value) {
  Node node;
  node.value_ = std::move(value);
  return node;
}

Node Node::array() {
  Node node;
  node.value_ = Array{};
  return node;
}

Node Node::object() {
  Node node;
  node.value_ = Object{};
  return node;
}

Node Node::absence(Absence value) {
  Node node;
  node.value_ = std::move(value);
  return node;
}

Node& Node::add(std::string key, Node value) {
  auto& object = std::get<Object>(value_);
  object.keys.push_back(std::move(key));
  object.values.push_back(std::move(value));
  return *this;
}

Node& Node::push(Node value) {
  std::get<Array>(value_).items.push_back(std::move(value));
  return *this;
}

NodeKind Node::kind() const noexcept { return static_cast<NodeKind>(value_.index()); }
bool Node::as_bool() const { return std::get<bool>(value_); }
std::int64_t Node::as_integer() const { return std::get<std::int64_t>(value_); }
std::uint64_t Node::as_unsigned() const { return std::get<std::uint64_t>(value_); }
double Node::as_real() const { return std::get<double>(value_); }
const std::string& Node::as_text() const { return std::get<std::string>(value_); }
const Array& Node::as_array() const { return std::get<Array>(value_); }
const Object& Node::as_object() const { return std::get<Object>(value_); }
const Absence& Node::as_absence() const { return std::get<Absence>(value_); }

const Node* Node::find(std::string_view key) const {
  const auto& object = std::get<Object>(value_);
  for (std::size_t index = 0; index < object.keys.size(); ++index) {
    if (object.keys[index] == key) {
      return &object.values[index];
    }
  }
  return nullptr;
}

std::string format_real(double value) {
  std::array<char, 40> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return {buffer.data(), written.ptr};
}

std::string_view availability_code(topology::Availability availability) noexcept {
  switch (availability) {
    case topology::Availability::kPresent:
      return "present";
    case topology::Availability::kAbsent:
      return "absent";
    case topology::Availability::kVanished:
      return "vanished";
    case topology::Availability::kDenied:
      return "denied";
    case topology::Availability::kMalformed:
      return "malformed";
    case topology::Availability::kUnreadable:
      return "unreadable";
  }
  return "unrecognised";
}

std::string_view verdict_code(Verdict verdict) noexcept {
  switch (verdict) {
    case Verdict::kClean:
      return "clean";
    case Verdict::kCorrectedErrors:
      return "corrected_errors";
    case Verdict::kIncomplete:
      return "incomplete";
    case Verdict::kFindings:
      return "findings";
  }
  return "unrecognised_verdict";
}

namespace {

using telemetry::Unavailable;

Node text(std::string_view value) { return Node::text(std::string(value)); }

Node opt_text(const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return Node::null();
  }
  return Node::text(*value);
}
Node opt_unsigned(const std::optional<std::uint64_t>& value) {
  if (!value.has_value()) {
    return Node::null();
  }
  return Node::unsigned_integer(*value);
}
Node opt_integer(const std::optional<std::int64_t>& value) {
  if (!value.has_value()) {
    return Node::null();
  }
  return Node::integer(*value);
}
Node opt_real(const std::optional<double>& value) {
  if (!value.has_value()) {
    return Node::null();
  }
  return Node::real(*value);
}
Node opt_bool(const std::optional<bool>& value) {
  if (!value.has_value()) {
    return Node::null();
  }
  return Node::boolean(*value);
}

std::string hex(const std::vector<std::byte>& bytes) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const std::byte byte : bytes) {
    const auto value = std::to_integer<unsigned>(byte);
    out.push_back(kDigits[value >> 4U]);
    out.push_back(kDigits[value & 0xFU]);
  }
  return out;
}

Node absence_of(const Unavailable& unavailable) {
  Absence absence;
  absence.kind = availability_code(unavailable.kind);
  absence.words = topology::describe(unavailable.kind);
  absence.path = unavailable.path;
  absence.detail = unavailable.detail;
  absence.remediation = unavailable.remediation;
  return Node::absence(std::move(absence));
}

Node metric_node(const telemetry::Metric& metric) {
  Node node = Node::object();
  node.add("name", Node::text(metric.name));
  node.add("unit", text(telemetry::describe(metric.unit)));
  node.add("kind", text(metric.kind == telemetry::Kind::kGauge ? "gauge" : "counter"));
  node.add("scope", text(telemetry::describe(metric.scope)));
  node.add("scope_index", Node::unsigned_integer(metric.scope_index));
  node.add("modulus", opt_unsigned(metric.modulus));
  node.add("provider", Node::text(metric.provider));
  return node;
}

Node fingerprint_node(const Fingerprint& fingerprint) {
  Node node = Node::object();
  std::vector<std::string_view> absent;
  const auto note = [&absent](std::string_view name, bool present) {
    if (!present) {
      absent.push_back(name);
    }
  };
  node.add("loadforge_version", Node::text(fingerprint.loadforge_version));
  node.add("git_commit", opt_text(fingerprint.git_commit));
  note("git_commit", fingerprint.git_commit.has_value());
  node.add("compiler", Node::text(fingerprint.compiler));
  node.add("flags", Node::text(fingerprint.flags));
  node.add("cpu_model", opt_text(fingerprint.cpu_model));
  note("cpu_model", fingerprint.cpu_model.has_value());
  node.add("microcode", opt_text(fingerprint.microcode));
  note("microcode", fingerprint.microcode.has_value());
  node.add("kernel", opt_text(fingerprint.kernel));
  note("kernel", fingerprint.kernel.has_value());
  node.add("smt_enabled", opt_bool(fingerprint.smt_enabled));
  note("smt_enabled", fingerprint.smt_enabled.has_value());
  node.add("power_source", opt_text(fingerprint.power_source));
  note("power_source", fingerprint.power_source.has_value());
  node.add("battery_state", opt_text(fingerprint.battery_state));
  note("battery_state", fingerprint.battery_state.has_value());
  node.add("cpufreq_governor", opt_text(fingerprint.cpufreq_governor));
  note("cpufreq_governor", fingerprint.cpufreq_governor.has_value());
  node.add("energy_performance_preference", opt_text(fingerprint.energy_performance_preference));
  note("energy_performance_preference", fingerprint.energy_performance_preference.has_value());
  node.add("platform_profile", opt_text(fingerprint.platform_profile));
  note("platform_profile", fingerprint.platform_profile.has_value());
  node.add("turbo_enabled", opt_bool(fingerprint.turbo_enabled));
  note("turbo_enabled", fingerprint.turbo_enabled.has_value());
  node.add("cooling_profile", opt_text(fingerprint.cooling_profile));
  note("cooling_profile", fingerprint.cooling_profile.has_value());
  node.add("cgroup_memory_limit_bytes", opt_unsigned(fingerprint.cgroup_memory_limit_bytes));
  note("cgroup_memory_limit_bytes", fingerprint.cgroup_memory_limit_bytes.has_value());
  node.add("huge_page_policy", opt_text(fingerprint.huge_page_policy));
  note("huge_page_policy", fingerprint.huge_page_policy.has_value());
  node.add("temperature_at_start_c", opt_real(fingerprint.temperature_at_start_c));
  note("temperature_at_start_c", fingerprint.temperature_at_start_c.has_value());

  if (fingerprint.fp_environment.has_value()) {
    const fpenv::FpState& state = *fingerprint.fp_environment;
    Node fp = Node::object();
    fp.add("rounding", text(fpenv::describe(state.rounding)));
    fp.add("flush_to_zero", Node::boolean(state.flush_to_zero));
    fp.add("denormals_are_zero", opt_bool(state.denormals_are_zero));
    fp.add("default_nan", opt_bool(state.default_nan));
    node.add("fp_environment", std::move(fp));
  } else {
    node.add("fp_environment", Node::null());
    note("fp_environment", false);
  }
  node.add("isa_path", fingerprint.isa_path.has_value()
                           ? text(simd::describe(*fingerprint.isa_path))
                           : Node::null());
  note("isa_path", fingerprint.isa_path.has_value());

  Node names = Node::array();
  for (const std::string_view name : absent) {
    names.push(text(name));
  }
  node.add("absent_fields", std::move(names));
  return node;
}

Node capabilities_node(const telemetry::CapabilityReport& report) {
  Node sources = Node::array();
  for (const telemetry::SourceStatus& source : report.sources) {
    Node metrics = Node::array();
    for (const telemetry::MetricStatus& status : source.metrics) {
      Node one = Node::object();
      one.add("metric", metric_node(status.metric));
      one.add("availability",
              status.unavailable.has_value() ? absence_of(*status.unavailable) : text("present"));
      metrics.push(std::move(one));
    }
    Node node = Node::object();
    node.add("source", Node::text(source.source));
    node.add("metrics", std::move(metrics));
    sources.push(std::move(node));
  }
  return sources;
}

Node summary_node(const statistics::Summary& summary) {
  if (const auto* gauge = std::get_if<statistics::GaugeSummary>(&summary)) {
    Node node = Node::object();
    node.add("type", text("gauge"));
    node.add("samples", Node::unsigned_integer(gauge->samples));
    node.add("min", Node::real(gauge->min));
    node.add("max", Node::real(gauge->max));
    node.add("mean", Node::real(gauge->mean));
    node.add("stddev", opt_real(gauge->stddev));
    node.add("first", Node::real(gauge->first));
    node.add("last", Node::real(gauge->last));
    node.add("slope_per_second", opt_real(gauge->slope_per_second));
    return node;
  }
  if (const auto* counter = std::get_if<statistics::CounterSummary>(&summary)) {
    Node node = Node::object();
    node.add("type", text("counter"));
    node.add("samples", Node::unsigned_integer(counter->samples));
    node.add("delta", Node::unsigned_integer(counter->delta));
    node.add("rate_per_second", opt_real(counter->rate_per_second));
    node.add("resets", Node::unsigned_integer(counter->resets));
    return node;
  }
  return absence_of(*std::get_if<Unavailable>(&summary));
}

Node signature_node(const statistics::LoadSignature& signature) {
  Node window = Node::object();
  window.add("start_ns", Node::integer(signature.window.start_ns));
  window.add("end_ns", Node::integer(signature.window.end_ns));
  Node metrics = Node::array();
  for (const statistics::MetricSummary& summary : signature.metrics) {
    Node one = Node::object();
    one.add("metric", metric_node(summary.metric));
    one.add("unavailable_samples", Node::unsigned_integer(summary.unavailable_samples));
    one.add("summary", summary_node(summary.value));
    metrics.push(std::move(one));
  }
  Node node = Node::object();
  node.add("window", std::move(window));
  node.add("metrics", std::move(metrics));
  return node;
}

Node provenance_node(const verification::Provenance& provenance) {
  const auto number = [](const std::optional<std::uint32_t>& value) {
    return value.has_value() ? Node::unsigned_integer(*value) : Node::null();
  };
  Node node = Node::object();
  node.add("logical_cpu", number(provenance.logical_cpu));
  node.add("physical_core", number(provenance.physical_core));
  node.add("numa_node", number(provenance.numa_node));
  node.add("buffer_address", opt_unsigned(provenance.buffer_address));
  node.add("buffer_offset", opt_unsigned(provenance.buffer_offset));
  node.add("iteration", opt_unsigned(provenance.iteration));
  node.add("workload", opt_text(provenance.workload));
  node.add("workload_version", number(provenance.workload_version));
  node.add("seed", opt_unsigned(provenance.seed));
  node.add("isa",
           provenance.isa.has_value() ? text(simd::describe(*provenance.isa)) : Node::null());
  node.add("input_integrity", provenance.input_integrity.has_value()
                                  ? text(verification::describe(*provenance.input_integrity))
                                  : Node::null());
  if (provenance.duty.has_value()) {
    Node duty = Node::object();
    duty.add("oracle_period", Node::unsigned_integer(provenance.duty->oracle_period));
    duty.add("integrity_period", Node::unsigned_integer(provenance.duty->integrity_period));
    node.add("duty", std::move(duty));
  } else {
    node.add("duty", Node::null());
  }
  return node;
}

Node difference_node(const verification::Difference& difference) {
  Node node = Node::object();
  node.add("domain", text(verification::describe(difference.domain)));
  node.add("first_offset", Node::unsigned_integer(difference.first_offset));
  node.add("last_offset", Node::unsigned_integer(difference.last_offset));
  node.add("popcount", Node::unsigned_integer(difference.popcount));
  node.add("window_offset", Node::unsigned_integer(difference.window_offset));
  node.add("expected_hex", Node::text(hex(difference.expected)));
  node.add("actual_hex", Node::text(hex(difference.actual)));
  return node;
}

Node isolation_node(const verification::IsolationReport& report) {
  Node steps = Node::array();
  for (const verification::Step& step : report.steps) {
    Node one = Node::object();
    one.add("rung", text(verification::describe(step.request.rung)));
    one.add("cpu", Node::unsigned_integer(step.request.cpu));
    one.add("threads", Node::unsigned_integer(step.request.threads));
    one.add("outcome", text(verification::describe(step.outcome)));
    steps.push(std::move(one));
  }
  Node answers = Node::object();
  answers.add("same_core", text(verification::describe(report.answers.same_core)));
  answers.add("other_core", text(verification::describe(report.answers.other_core)));
  answers.add("single_thread", text(verification::describe(report.answers.single_thread)));
  Node node = Node::object();
  node.add("steps", std::move(steps));
  node.add("answers", std::move(answers));
  node.add("finding", text(verification::describe(report.finding)));
  node.add("confidence", text(verification::describe(report.confidence)));
  return node;
}

Node reading_node(const telemetry::Reading& reading) {
  if (const auto* gauge = std::get_if<telemetry::Gauge>(&reading)) {
    Node node = Node::object();
    node.add("type", text("gauge"));
    node.add("value", Node::real(gauge->value));
    return node;
  }
  if (const auto* counter = std::get_if<telemetry::Counter>(&reading)) {
    Node node = Node::object();
    node.add("type", text("counter"));
    node.add("raw", Node::unsigned_integer(counter->raw));
    return node;
  }
  return absence_of(*std::get_if<Unavailable>(&reading));
}

Node record_node(const verification::ErrorRecord& record) {
  Node node = Node::object();
  node.add("kind", text(verification::kind_name(record)));
  node.add("provenance", provenance_node(verification::provenance_of(record)));
  if (const auto* exact = std::get_if<verification::ExactBitCorruption>(&record)) {
    node.add("side", text(verification::describe(exact->side)));
    node.add("check", Node::text(exact->check));
    node.add("line_bytes", Node::unsigned_integer(exact->line_bytes));
    node.add("difference",
             exact->difference.has_value() ? difference_node(*exact->difference) : Node::null());
    const std::optional<verification::BitClass> bit_class = verification::classify(*exact);
    node.add("bit_class",
             bit_class.has_value() ? text(verification::describe(*bit_class)) : Node::null());
  } else if (const auto* numerical =
                 std::get_if<verification::NumericalVerificationFailure>(&record)) {
    node.add("check", Node::text(numerical->check));
    node.add("residual", Node::real(numerical->residual));
    node.add("tolerance", Node::real(numerical->tolerance));
    node.add("ratio", Node::real(verification::ratio(*numerical)));
    node.add("condition_number", opt_real(numerical->condition_number));
    node.add("element", opt_unsigned(numerical->element));
  } else {
    const auto* unrun = std::get_if<verification::VerificationNotPerformed>(&record);
    node.add("what", Node::text(unrun->what));
    node.add("why", Node::text(unrun->why));
    node.add("blocks_compared", Node::unsigned_integer(unrun->blocks_compared));
  }
  return node;
}

Node finding_node(const Finding& finding) {
  Node node = Node::object();
  node.add("record", record_node(finding.record));
  node.add("isolation",
           finding.isolation.has_value() ? isolation_node(*finding.isolation) : Node::null());
  Node telemetry = Node::array();
  for (const TelemetryAt& at : finding.telemetry) {
    Node one = Node::object();
    one.add("metric", metric_node(at.metric));
    one.add("timestamp_ns", Node::integer(at.point.timestamp_ns));
    one.add("reading", reading_node(at.point.reading));
    telemetry.push(std::move(one));
  }
  node.add("telemetry", std::move(telemetry));
  return node;
}

Node workload_node(const WorkloadResult& workload) {
  Node paths = Node::array();
  for (const simd::IsaPath path : {simd::IsaPath::kScalar, simd::IsaPath::kAvx2,
                                   simd::IsaPath::kAvx512, simd::IsaPath::kNeon}) {
    if (workload.descriptor.paths.contains(path)) {
      paths.push(text(simd::describe(path)));
    }
  }
  Node descriptor = Node::object();
  descriptor.add("name", Node::text(workload.descriptor.name));
  descriptor.add("version", Node::unsigned_integer(workload.descriptor.version));
  descriptor.add("determinism", text(workloads::describe(workload.descriptor.determinism)));
  descriptor.add("contract", text(verification::describe(workload.descriptor.contract)));
  descriptor.add("paths", std::move(paths));

  Node duty = Node::object();
  duty.add("oracle_period", Node::unsigned_integer(workload.duty.oracle_period));
  duty.add("integrity_period", Node::unsigned_integer(workload.duty.integrity_period));

  const verification::Tally& tally = workload.tally;
  Node counts = Node::object();
  counts.add("units_computed", Node::unsigned_integer(tally.units_computed));
  counts.add("oracle_checked", Node::unsigned_integer(tally.oracle_checked));
  counts.add("oracle_blocks", Node::unsigned_integer(tally.oracle_blocks));
  counts.add("oracle_failed", Node::unsigned_integer(tally.oracle_failed));
  counts.add("oracle_not_performed", Node::unsigned_integer(tally.oracle_not_performed));
  counts.add("integrity_checked", Node::unsigned_integer(tally.integrity_checked));
  counts.add("integrity_failed", Node::unsigned_integer(tally.integrity_failed));

  Node findings = Node::array();
  for (const Finding& finding : workload.findings) {
    findings.push(finding_node(finding));
  }

  Node node = Node::object();
  node.add("descriptor", std::move(descriptor));
  node.add("isa", text(simd::describe(workload.isa)));
  node.add("decomposition", Node::unsigned_integer(workload.decomposition));
  node.add("duty", std::move(duty));
  node.add("tally", std::move(counts));
  node.add("logical_bytes", Node::unsigned_integer(workload.logical_bytes));
  node.add("findings", std::move(findings));
  return node;
}

Node worker_node(const WorkerResult& worker) {
  Node node = Node::object();
  node.add("slot", Node::unsigned_integer(worker.slot));
  node.add("fate", text(controller::describe(worker.fate)));
  node.add("cpu", worker.cpu.has_value() ? Node::unsigned_integer(*worker.cpu) : Node::null());
  node.add("termination", Node::text(worker.termination));
  return node;
}

}  // namespace

Node to_document(const RunResult& result) {
  Node root = Node::object();
  root.add("schema_version", Node::unsigned_integer(result.schema_version));
  root.add("mode", text(config::describe(result.mode)));
  root.add("elapsed_ns", Node::integer(result.elapsed_ns));
  Node verdict = Node::object();
  verdict.add("code", text(verdict_code(result.verdict)));
  verdict.add("words", text(describe(result.verdict)));
  root.add("verdict", std::move(verdict));
  root.add("fingerprint", fingerprint_node(result.fingerprint));
  root.add("capabilities", capabilities_node(result.capabilities));
  root.add("signature", signature_node(result.signature));
  Node workloads = Node::array();
  for (const WorkloadResult& workload : result.workloads) {
    workloads.push(workload_node(workload));
  }
  root.add("workloads", std::move(workloads));
  Node workers = Node::array();
  for (const WorkerResult& worker : result.workers) {
    workers.push(worker_node(worker));
  }
  root.add("workers", std::move(workers));
  Node observers = Node::object();
  observers.add("sample_interval_ns", Node::integer(result.observers.sample_interval_ns));
  observers.add("journal_flush_interval_ns",
                opt_integer(result.observers.journal_flush_interval_ns));
  observers.add("controller_overhead", opt_real(result.observers.controller_overhead));
  root.add("observers", std::move(observers));
  return root;
}

}  // namespace loadforge::results
