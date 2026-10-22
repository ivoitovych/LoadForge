// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_CONTROLLER_RESULTS_RICH_RUN_HPP
#define LOADFORGE_TESTS_UNIT_CONTROLLER_RESULTS_RICH_RUN_HPP

// A result with every part of the schema populated: every optional present in
// one run, every one absent in another, every record kind, an isolation report,
// telemetry of each reading kind, a residual that is not a number, and text a
// format must escape.

#include <cmath>
#include <limits>
#include <string>

#include "contracts/results/results_contract.hpp"
#include "controller/results/aggregate.hpp"

namespace loadforge::results::testing {

inline verification::Provenance full_provenance() {
  verification::Provenance p;
  p.logical_cpu = 3;
  p.physical_package = 1;
  p.physical_core = 1;
  p.numa_node = 0;
  p.buffer_address = 0x7f0000001000ULL;
  p.buffer_offset = 4096;
  p.iteration = 123456789012ULL;
  p.workload = "compression";
  p.workload_version = 2;
  p.seed = 18446744073709551615ULL;
  p.isa = simd::IsaPath::kAvx2;
  p.input_integrity = verification::InputIntegrity::kCorruptDuringConsumption;
  p.duty = verification::DutyCycle{4, 8};
  p.decomposition = 8;
  p.footprint_bytes = 33554432;
  return p;
}

inline Fingerprint full_fingerprint() {
  Fingerprint f;
  f.loadforge_version = "0.0.0-m3";
  f.git_commit = "5782b0f3097691f095afb53d39bbf1e95e573e79";
  f.compiler = "gcc 13.3.0";
  f.flags = "-O2 -ffp-contract=off";
  f.cpu_model = "Example CPU, \"quoted\"";
  f.microcode = "0xf4";
  f.kernel = "6.8.0";
  f.smt_enabled = true;
  f.power_source = "ac";
  f.battery_state = "Charging";
  f.cpufreq_governor = "performance";
  f.energy_performance_preference = "performance";
  f.platform_profile = "balanced";
  f.turbo_enabled = false;
  f.cooling_profile = "step_wise";
  f.cgroup_memory_limit_bytes = 4294967296ULL;
  f.huge_page_policy = "madvise";
  f.temperature_at_start_c = 47.5;
  fpenv::FpState fp;
  fp.rounding = fpenv::Rounding::kToNearestEven;
  fp.flush_to_zero = false;
  fp.denormals_are_zero = false;
  fp.default_nan = std::nullopt;
  f.fp_environment = fp;
  f.isa_path = simd::IsaPath::kAvx2;
  return f;
}

/// A run with everything in it.
inline RunResult rich_run() {
  RunResult run = contracts::results_suite::clean_run();
  run.fingerprint = full_fingerprint();
  run.capabilities.sources[0].metrics.push_back(
      {telemetry::Metric{"thermal.core", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                         telemetry::Scope::kCore, 2, std::nullopt, "hwmon"},
       std::nullopt});
  run.capabilities.sources.push_back(
      {"edac",
       {{telemetry::Metric{"edac.corrected", telemetry::Unit::kCount, telemetry::Kind::kCounter,
                           telemetry::Scope::kMemoryController, 0, 4294967296ULL, "edac"},
         std::nullopt}}});
  run.signature.metrics.push_back(
      {telemetry::Metric{"thermal.core", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                         telemetry::Scope::kCore, 2, std::nullopt, "hwmon"},
       statistics::GaugeSummary{3, 40.0, 80.5, 60.25, 20.125, 40.0, 80.5, 0.0625}, 1});
  run.signature.metrics.push_back(
      {telemetry::Metric{"thermal.pkg", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                         telemetry::Scope::kPackage, 0, std::nullopt, "hwmon"},
       statistics::GaugeSummary{1, 50.0, 50.0, 50.0, std::nullopt, 50.0, 50.0, std::nullopt}, 0});
  run.signature.metrics.push_back({contracts::results_suite::counter("edac.other"),
                                   statistics::CounterSummary{4, 17, std::nullopt}, 0});

  WorkloadResult& workload = run.workloads[0];
  workload.isa = simd::IsaPath::kAvx2;
  workload.decomposition = 8;
  workload.duty = verification::DutyCycle{4, 8};
  workload.descriptor.paths = simd::IsaSet{simd::IsaPath::kScalar, simd::IsaPath::kAvx2};
  workload.tally = verification::Tally{1000, 250, 250, 2, 1, 125, 1};
  workload.lock_policy = verification::LockPolicy::kTry;
  workload.memory_lock =
      verification::MemoryLock{verification::LockPolicy::kTry, verification::LockOutcome::kFellBack,
                               "mlock: Cannot allocate memory"};

  verification::ExactBitCorruption exact = contracts::results_suite::exact_record();
  exact.provenance = full_provenance();
  verification::Difference difference;
  difference.first_offset = 9;
  difference.last_offset = 9;
  difference.popcount = 1;
  difference.window_offset = 8;
  difference.expected = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}};
  difference.actual = {std::byte{0x01}, std::byte{0xFF}, std::byte{0x10}};
  exact.difference = difference;
  Finding with_difference{exact, std::nullopt, {}};
  verification::IsolationReport isolation;
  isolation.steps = {
      {{verification::Rung::kSameCore, 3, 1}, verification::RerunOutcome::kReproduced},
      {{verification::Rung::kOtherCore, 5, 1}, verification::RerunOutcome::kClean}};
  isolation.answers = {verification::Answer::kYes, verification::Answer::kNo,
                       verification::Answer::kUnknown};
  isolation.finding = verification::Finding::kCoreLocal;
  isolation.confidence = verification::Confidence::kMedium;
  with_difference.isolation = isolation;
  with_difference.telemetry = {
      {telemetry::Metric{"thermal.core", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                         telemetry::Scope::kCore, 2, std::nullopt, "hwmon"},
       {1'500'000'000, telemetry::Gauge{81.5}}},
      {contracts::results_suite::counter("edac.corrected"), {1'600'000'000, telemetry::Counter{7}}},
      {contracts::results_suite::counter("edac.uncorrected"),
       {1'700'000'000,
        telemetry::Unavailable{topology::Availability::kVanished, "/sys/edac/mc0",
                               "the memory controller went away", "reload the edac driver"}}}};

  verification::NumericalVerificationFailure numerical =
      contracts::results_suite::numerical_record();
  numerical.provenance = full_provenance();
  numerical.residual = std::numeric_limits<double>::quiet_NaN();
  numerical.tolerance = 1e-9;
  numerical.condition_number = 1e6;
  numerical.element = 42;
  verification::NumericalVerificationFailure infinite =
      contracts::results_suite::numerical_record();
  infinite.residual = -std::numeric_limits<double>::infinity();
  verification::NumericalVerificationFailure overflowed =
      contracts::results_suite::numerical_record();
  overflowed.residual = std::numeric_limits<double>::infinity();
  verification::VerificationNotPerformed unrun = contracts::results_suite::unrun_record();
  unrun.provenance = full_provenance();
  unrun.what = "oracle, with a comma, and \"quotes\"\nand a newline";
  unrun.why = std::string("a byte that is not UTF-8: ") + '\xff' + " and a control: " + '\x01';
  unrun.blocks_compared = 12;

  workload.findings = {with_difference,
                       contracts::results_suite::finding(contracts::results_suite::exact_record()),
                       contracts::results_suite::finding(numerical),
                       contracts::results_suite::finding(infinite),
                       contracts::results_suite::finding(overflowed),
                       contracts::results_suite::finding(unrun)};

  run.workers.push_back({1, controller::WorkerFate::kKilled, std::nullopt, "killed by signal 9"});
  run.observers.journal_flush_interval_ns = 1'000'000'000;
  run.observers.controller_overhead = 0.015625;
  seal(run);
  return run;
}

/// A run with every optional absent.
inline RunResult bare_run() {
  RunResult run = contracts::results_suite::clean_run();
  run.fingerprint = Fingerprint{};
  run.capabilities = {};
  run.signature = {};
  run.workloads.clear();
  run.workers.clear();
  run.observers = {};
  run.workloads.push_back(WorkloadResult{});
  seal(run);
  return run;
}

}  // namespace loadforge::results::testing

#endif
