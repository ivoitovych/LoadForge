// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_CONTRACTS_RESULTS_RESULTS_CONTRACT_HPP
#define LOADFORGE_TESTS_CONTRACTS_RESULTS_RESULTS_CONTRACT_HPP

// Provider-side suite for src/controller/results/result.hpp (ADR 0003, 0004).
//
// VerdictContract<Subject>: Subject::derive has the signature of
// results::derive_verdict.
//
// The suites here and in writer_contract.hpp build their results from results_suite::clean_run(), a
// small run with every kind of content a rendering must not lose. Text placed in it is plain ASCII
// with no quote, backslash, comma or newline, so "verbatim" needs no knowledge of any format's
// escaping.

#include <gtest/gtest.h>

#include <ios>
#include <memory>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include "controller/results/result.hpp"

namespace loadforge::contracts {

namespace results_suite {

inline constexpr const char* kAbsentRemedy = "load the coretemp driver to read this sensor";
inline constexpr const char* kDeniedRemedy = "grant read access to energy_uj with a udev rule";
inline constexpr const char* kDeniedDetail = "energy_uj is mode 0400 root";

inline telemetry::Metric counter(const std::string& name) {
  return {name,
          telemetry::Unit::kCount,
          telemetry::Kind::kCounter,
          telemetry::Scope::kMemoryController,
          0,
          std::nullopt,
          "edac"};
}

inline statistics::MetricSummary counted(const std::string& name, std::uint64_t delta) {
  return {counter(name), statistics::CounterSummary{2, delta, 0.5}, 0};
}

/// A run that checked everything and found nothing.
inline results::RunResult clean_run() {
  results::RunResult run;
  run.mode = config::Mode::kStress;
  run.elapsed_ns = 3'600'000'000'000;
  run.fingerprint.loadforge_version = "0.0.0-m2a";
  run.fingerprint.compiler = "gcc 13.3";
  run.fingerprint.flags = "-O2 -ffp-contract=off";
  run.fingerprint.cpu_model = "Example CPU";
  run.capabilities.sources.push_back(
      {"thermal",
       {{telemetry::Metric{"thermal.package", telemetry::Unit::kCelsius, telemetry::Kind::kGauge,
                           telemetry::Scope::kPackage, 0, std::nullopt, "hwmon"},
         telemetry::Unavailable{topology::Availability::kAbsent, "/sys/class/hwmon",
                                "no package sensor is exposed", kAbsentRemedy}}}});
  run.signature.window = {0, run.elapsed_ns};
  run.signature.metrics.push_back(
      {telemetry::Metric{"power.package", telemetry::Unit::kJoules, telemetry::Kind::kCounter,
                         telemetry::Scope::kPackage, 0, std::nullopt, "powercap"},
       telemetry::Unavailable{topology::Availability::kDenied,
                              "/sys/class/powercap/intel-rapl:0/energy_uj", kDeniedDetail,
                              kDeniedRemedy},
       0});
  run.signature.metrics.push_back(counted(std::string(results::kCorrectedErrorsMetric), 0));
  results::WorkloadResult workload;
  workload.descriptor = {"compression", 1, workloads::DeterminismClass::kBitExact,
                         verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}};
  workload.tally = verification::Tally{100, 10, 10, 0, 0, 10, 0};
  workload.logical_bytes = 819200;
  run.workloads.push_back(workload);
  run.workers.push_back({0, controller::WorkerFate::kCompleted, 3, "exited 0"});
  run.observers.sample_interval_ns = 100'000'000;
  run.verdict = results::Verdict::kClean;
  return run;
}

inline results::Finding finding(verification::ErrorRecord record) {
  return results::Finding{std::move(record), std::nullopt, {}};
}

inline verification::ExactBitCorruption exact_record() {
  verification::ExactBitCorruption record;
  record.check = "input against its regeneration";
  record.side = verification::Side::kInput;
  return record;
}

inline verification::NumericalVerificationFailure numerical_record() {
  verification::NumericalVerificationFailure record;
  record.check = "residual";
  record.residual = 2.0;
  record.tolerance = 1.0;
  return record;
}

inline verification::VerificationNotPerformed unrun_record() {
  return {{}, "oracle over compression units", "no unit was sampled", 0};
}

/// A stream buffer that refuses every byte.
class RefusingBuffer final : public std::streambuf {
 protected:
  int_type overflow(int_type /*ch*/) override { return traits_type::eof(); }
  std::streamsize xsputn(const char* /*s*/, std::streamsize /*n*/) override { return 0; }
};

}  // namespace results_suite

template <typename Subject>
class VerdictContract : public ::testing::Test {};
TYPED_TEST_SUITE_P(VerdictContract);

using results::Verdict;

TYPED_TEST_P(VerdictContract, ARunThatCheckedEverythingAndFoundNothingIsClean) {
  EXPECT_EQ(TypeParam::derive(results_suite::clean_run()), Verdict::kClean);
}

TYPED_TEST_P(VerdictContract, AWrongResultIsAFindingWhateverElseHappened) {
  for (const verification::ErrorRecord& record :
       {verification::ErrorRecord{results_suite::exact_record()},
        verification::ErrorRecord{results_suite::numerical_record()}}) {
    results::RunResult run = results_suite::clean_run();
    run.workloads[0].findings.push_back(results_suite::finding(results_suite::unrun_record()));
    run.workloads[0].findings.push_back(results_suite::finding(record));
    run.workers[0].fate = controller::WorkerFate::kFault;
    EXPECT_EQ(TypeParam::derive(run), Verdict::kFindings) << verification::kind_name(record);
  }
}

TYPED_TEST_P(VerdictContract, AnUncorrectedHardwareErrorIsAFinding) {
  results::RunResult run = results_suite::clean_run();
  run.signature.metrics.push_back(
      results_suite::counted(std::string(results::kUncorrectedErrorsMetric), 1));
  EXPECT_EQ(TypeParam::derive(run), Verdict::kFindings);
  run.signature.metrics.back() =
      results_suite::counted(std::string(results::kUncorrectedErrorsMetric), 0);
  EXPECT_EQ(TypeParam::derive(run), Verdict::kClean) << "a zero delta is no error";
}

TYPED_TEST_P(VerdictContract, AnythingNotCheckedOrNotFinishedIsIncomplete) {
  results::RunResult run = results_suite::clean_run();
  run.workloads.clear();
  EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete) << "no workload ran";

  run = results_suite::clean_run();
  run.workloads[0].findings.push_back(results_suite::finding(results_suite::unrun_record()));
  EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete) << "a check that was not made";

  run = results_suite::clean_run();
  run.workloads[0].tally.oracle_blocks = 0;
  EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete) << "the oracle compared nothing";

  run = results_suite::clean_run();
  run.workloads[0].tally.integrity_checked = 0;
  EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete) << "the bracket ran on nothing";

  run = results_suite::clean_run();
  run.workers[0].fate = controller::WorkerFate::kTimedOut;
  EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete) << "a worker did not finish";

  run.signature.metrics.push_back(
      results_suite::counted(std::string(results::kCorrectedErrorsMetric), 3));
  EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete) << "incomplete outranks corrected";
}

TYPED_TEST_P(VerdictContract, CorrectedErrorsMakeACleanRunNotClean) {
  results::RunResult run = results_suite::clean_run();
  run.signature.metrics.push_back(
      results_suite::counted(std::string(results::kCorrectedErrorsMetric), 3));
  EXPECT_EQ(TypeParam::derive(run), Verdict::kCorrectedErrors);
  run.signature.metrics.back().value = telemetry::Unavailable{
      topology::Availability::kAbsent, "/sys/devices/system/edac", "no EDAC driver", "none"};
  EXPECT_EQ(TypeParam::derive(run), Verdict::kClean)
      << "absent counters are not errors; the capability report says EDAC was not read";
}

TYPED_TEST_P(VerdictContract, AnErrorCounterThatWentBlindForPartOfTheRunIsNotAClaimOfCleanliness) {
  const std::string corrected{results::kCorrectedErrorsMetric};
  const std::string uncorrected{results::kUncorrectedErrorsMetric};
  for (const std::string& name : {corrected, uncorrected}) {
    results::RunResult run = results_suite::clean_run();
    run.signature.metrics.push_back(results_suite::counted(name, 0));
    ASSERT_EQ(TypeParam::derive(run), Verdict::kClean);
    run.signature.metrics.back().unavailable_samples = 2;
    EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete)
        << name << ": unreadable for two rounds, errors may have been counted in the gap";

    run.signature.metrics.back().unavailable_samples = 0;
    std::get<statistics::CounterSummary>(run.signature.metrics.back().value).resets = 1;
    EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete)
        << name << ": reset, so the advance across the reset is unknown";

    run.signature.metrics.back() = {
        results_suite::counter(name),
        telemetry::Unavailable{topology::Availability::kVanished, "/sys/devices/system/edac",
                               "gone", "re-load the driver"},
        5};
    EXPECT_EQ(TypeParam::derive(run), Verdict::kIncomplete)
        << name << ": present at probe and unreadable throughout is blind";
  }
}

TYPED_TEST_P(VerdictContract, BlindnessOfAnythingElseOrAWrongResultDoesNotChangeTheVerdict) {
  results::RunResult run = results_suite::clean_run();
  const statistics::MetricSummary fan{results_suite::counter("fan.pulses"),
                                      statistics::CounterSummary{2, 5, 1.0, 3}, 4};
  run.signature.metrics.push_back(fan);
  EXPECT_EQ(TypeParam::derive(run), Verdict::kClean)
      << "only the hardware error counters stand for 'nothing was found wrong'";

  results::RunResult found = results_suite::clean_run();
  found.signature.metrics.push_back(
      results_suite::counted(std::string(results::kUncorrectedErrorsMetric), 2));
  found.signature.metrics.back().unavailable_samples = 1;
  EXPECT_EQ(TypeParam::derive(found), Verdict::kFindings) << "a finding outranks incompleteness";

  results::RunResult corrected = results_suite::clean_run();
  corrected.signature.metrics.push_back(
      results_suite::counted(std::string(results::kCorrectedErrorsMetric), 2));
  corrected.signature.metrics.back().unavailable_samples = 1;
  EXPECT_EQ(TypeParam::derive(corrected), Verdict::kIncomplete)
      << "errors counted and part of the run not seen: incomplete outranks corrected";
}

REGISTER_TYPED_TEST_SUITE_P(VerdictContract, ARunThatCheckedEverythingAndFoundNothingIsClean,
                            AWrongResultIsAFindingWhateverElseHappened,
                            AnUncorrectedHardwareErrorIsAFinding,
                            AnythingNotCheckedOrNotFinishedIsIncomplete,
                            CorrectedErrorsMakeACleanRunNotClean,
                            AnErrorCounterThatWentBlindForPartOfTheRunIsNotAClaimOfCleanliness,
                            BlindnessOfAnythingElseOrAWrongResultDoesNotChangeTheVerdict);

}  // namespace loadforge::contracts

#endif
