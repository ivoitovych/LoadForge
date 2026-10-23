// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console reporter: exactly what a person sees, line for line.

#include "reporters/console.hpp"

#include <gtest/gtest.h>

#include <csignal>

#include <sstream>
#include <string>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/supervision.hpp"
#include "core/duration.hpp"
#include "core/version.hpp"
#include "verification/isolation.hpp"
#include "verification/record.hpp"

namespace loadforge::reporters {
namespace {

TEST(ConsoleReporterTest, TheBannerNamesTheRunAndWhetherEachLimitIsEnforced) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunPlan plan;
  plan.mode = config::Mode::kStress;
  plan.duration = core::Duration{30'000};
  plan.workers = 2;
  plan.executable = "/usr/bin/loadforge";
  plan.limits = {controller::LimitStatus{"duration", true, "30s"},
                 controller::LimitStatus{"temperature", false, "not until M3"}};
  plan.topology = "4 logical CPU(s) on 4 core(s) in 1 package(s); 13 cache(s), deepest L3";
  reporter.run_starting(plan);
  EXPECT_EQ(out.str(), std::string{"loadforge "} + std::string{core::kVersion} +
                           ": stress for 30s with 2 worker(s), null workload\n"
                           "  executable: /usr/bin/loadforge\n"
                           "  topology: 4 logical CPU(s) on 4 core(s) in 1 package(s); 13 "
                           "cache(s), deepest L3\n"
                           "  limit duration: enforced (30s)\n"
                           "  limit temperature: NOT ENFORCED (not until M3)\n");
}

TEST(ConsoleReporterTest, AnEventCarriesTheTimeIntoTheRun) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  reporter.event(1'234'567'890, "worker 0 (pid 42): exited with code 0");
  reporter.event(0, "spawned 2 worker(s): pids 42 43");
  EXPECT_EQ(out.str(),
            "[1.235 s] worker 0 (pid 42): exited with code 0\n"
            "[0.000 s] spawned 2 worker(s): pids 42 43\n");
}

TEST(ConsoleReporterTest, TheSummaryNamesEachWorkersFateAndWhatItSent) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunReport report;
  report.elapsed_ns = 30'500'000'000;
  controller::WorkerReport first;
  first.slot = 0;
  first.pid = 42;
  first.fate = controller::WorkerFate::kCompleted;
  first.termination = "exited with code 0";
  first.stop = "stop requested";
  first.progress = 300;
  first.records = {"started: slot 0 pid 42", "finished: the controller asked for a stop"};
  controller::WorkerReport second;
  second.slot = 1;
  second.pid = 43;
  second.fate = controller::WorkerFate::kFault;
  second.termination = "killed by signal 11";
  second.stop = "running";
  second.progress = 7;
  report.workers = {first, second};
  reporter.run_finished(report);
  EXPECT_EQ(out.str(),
            "run ended after 30.500 s: 1 of 2 worker(s) completed\n"
            "  worker 0 (pid 42): completed; exited with code 0; progress 300; stop requested\n"
            "    started: slot 0 pid 42\n"
            "    finished: the controller asked for a stop\n"
            "  worker 1 (pid 43): fault; killed by signal 11; progress 7; running\n");
}

// --- verified workloads ----------------------------------------------------------------

controller::WorkerReport a_verifying_worker() {
  controller::WorkerReport worker;
  worker.slot = 0;
  worker.pid = 42;
  worker.fate = controller::WorkerFate::kCompleted;
  worker.termination = "exited with code 0";
  worker.stop = "stop requested";
  worker.progress = 12;
  worker.work = controller::WorkerWork{};
  worker.tally_seen = true;
  worker.tally.units_computed = 12;
  worker.tally.oracle_checked = 10;
  worker.tally.oracle_blocks = 640;
  worker.tally.oracle_failed = 1;
  worker.tally.oracle_not_performed = 2;
  worker.tally.integrity_checked = 6;
  worker.tally.integrity_failed = 3;
  return worker;
}

TEST(ConsoleReporterTest, TheBannerStatesEachWorkloadTheRegistersAndTheCapabilities) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunPlan plan;
  plan.mode = config::Mode::kStress;
  plan.duration = core::Duration{1000};
  plan.workers = 2;
  plan.executable = "/usr/bin/loadforge";
  plan.topology = "t";
  plan.workloads = {"compression: 1 thread(s) on cpu 0", "compression: 1 thread(s) on cpu 1"};
  plan.environment = "floating point (asserted at startup): rounding to nearest";
  plan.capabilities = "telemetry capability: 1 of 2 metric(s) present\n  thermal: 0 of 1 present\n";
  reporter.run_starting(plan);
  EXPECT_EQ(out.str(), std::string{"loadforge "} + std::string{core::kVersion} +
                           ": stress for 1s with 2 worker(s)\n"
                           "  executable: /usr/bin/loadforge\n"
                           "  topology: t\n"
                           "  workload compression: 1 thread(s) on cpu 0\n"
                           "  workload compression: 1 thread(s) on cpu 1\n"
                           "  floating point (asserted at startup): rounding to nearest\n"
                           "  capabilities:\n"
                           "telemetry capability: 1 of 2 metric(s) present\n"
                           "  thermal: 0 of 1 present\n");
  plan.capabilities = "no trailing newline";
  std::ostringstream again;
  ConsoleReporter second(again);
  second.run_starting(plan);
  EXPECT_NE(again.str().find("  capabilities:\nno trailing newline\n"), std::string::npos);
}

TEST(ConsoleReporterTest, WhatAWorkerCheckedIsToldAsSampledAndNeverAsCorrect) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunReport report;
  report.workers = {a_verifying_worker()};
  reporter.run_finished(report);
  const std::string text = out.str();
  EXPECT_NE(text.find("    verification: 12 unit(s) computed; the oracle checked 10 of them over "
                      "640 sampled block(s) and found 1 wrong, 2 not checked; the input bracket "
                      "covered 6 unit(s) and found 3 wrong\n"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("    findings: 0 wrong result(s), 0 check(s) not performed\n"),
            std::string::npos)
      << text;
  EXPECT_EQ(text.find("correct"), std::string::npos);
}

TEST(ConsoleReporterTest, AWorkerThatSentNoTallySaysNothingItCheckedIsKnown) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunReport report;
  controller::WorkerReport worker = a_verifying_worker();
  worker.tally_seen = false;
  report.workers = {worker};
  reporter.run_finished(report);
  EXPECT_NE(out.str().find("    verification: this worker sent no tally; nothing it checked is "
                           "known\n"),
            std::string::npos)
      << out.str();
}

TEST(ConsoleReporterTest, EveryFindingIsPrintedUnderItsWorkerAndTheOnesNotKeptAreCounted) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunReport report;
  controller::WorkerReport worker = a_verifying_worker();
  verification::VerificationNotPerformed unrun;
  unrun.what = "oracle over compression units";
  unrun.why = "no unit was sampled";
  controller::FindingReport finding;
  finding.record = unrun;
  worker.findings = {finding};
  worker.not_performed = 1;
  worker.findings_dropped = 4;
  report.workers = {worker};
  reporter.run_finished(report);
  const std::string text = out.str();
  EXPECT_NE(
      text.find("    findings: 0 wrong result(s), 1 check(s) not performed (4 more not kept)\n"),
      std::string::npos)
      << text;
  EXPECT_NE(text.find("      VerificationNotPerformed"), std::string::npos) << text;
  EXPECT_NE(text.find("      "), std::string::npos);
}

TEST(ConsoleReporterTest, IsolationIsToldInTheLaddersWordsAndNeverAsAVerdict) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunReport report;
  controller::WorkerReport worker = a_verifying_worker();
  verification::ExactBitCorruption wrong;
  wrong.check = "decoded output against input";
  controller::FindingReport isolated;
  isolated.record = wrong;
  verification::IsolationReport ladder;
  ladder.answers = {verification::Answer::kYes, verification::Answer::kNo,
                    verification::Answer::kYes};
  ladder.finding = verification::derive_finding(ladder.answers);
  ladder.confidence = verification::confidence_of(ladder.finding);
  isolated.isolation = ladder;
  controller::FindingReport skipped;
  skipped.record = wrong;
  skipped.not_isolated = "past the limit of 1 isolations";
  verification::VerificationNotPerformed unrun;
  unrun.what = "w";
  unrun.why = "y";
  controller::FindingReport unchecked;
  unchecked.record = unrun;
  worker.findings = {isolated, skipped, unchecked};
  report.workers = {worker};
  reporter.isolation_finished(report);
  EXPECT_EQ(out.str(),
            "  worker 0 isolation of ExactBitCorruption:\n"
            "      isolation: same core yes, another core no, single thread yes\n"
            "      " +
                std::string{verification::describe(ladder.finding)} + " (confidence " +
                std::string{verification::describe(ladder.confidence)} +
                ")\n"
                "  worker 0 ExactBitCorruption not isolated: past the limit of 1 isolations\n")
      << "a check that was not made is not isolated and says nothing";
}

TEST(ConsoleReporterTest, ARunTheCeilingEndedSaysWhichSensorWhatItReadAndWhen) {
  std::ostringstream out;
  ConsoleReporter reporter(out);
  controller::RunReport report;
  report.elapsed_ns = 4'000'000'000;
  EXPECT_EQ(out.str(), "");
  reporter.run_finished(report);
  EXPECT_EQ(out.str().find("temperature"), std::string::npos) << "no stop, no line";
  controller::TemperatureStop stop;
  stop.sensor = "thermal.core 3 (hwmon:coretemp)";
  stop.celsius = 91.0;
  stop.ceiling_c = 90;
  stop.at_ns = 3'500'000'000;
  report.temperature_stop = stop;
  std::ostringstream stopped;
  ConsoleReporter again(stopped);
  again.run_finished(report);
  EXPECT_EQ(stopped.str(),
            "run ended after 4.000 s: 0 of 0 worker(s) completed\n"
            "  ended by the temperature ceiling: thermal.core 3 (hwmon:coretemp) read 91.0 C, "
            "at or above the ceiling of 90 C, 3.500 s into the run\n");
}

TEST(ConsoleReporterTest, ARunASignalStoppedSaysWhichAndWhetherTheWorkersWereKilled) {
  controller::RunReport report;
  report.elapsed_ns = 4'000'000'000;
  report.stop_signal = SIGTERM;
  std::ostringstream stopped;
  ConsoleReporter reporter(stopped);
  reporter.run_finished(report);
  EXPECT_EQ(stopped.str(),
            "run ended after 4.000 s: 0 of 0 worker(s) completed\n"
            "  stopped by SIGTERM\n");
  report.stop_forced = true;
  std::ostringstream forced;
  ConsoleReporter again(forced);
  again.run_finished(report);
  EXPECT_EQ(forced.str(),
            "run ended after 4.000 s: 0 of 0 worker(s) completed\n"
            "  stopped by SIGTERM; a later signal killed the workers at once\n");
}

}  // namespace
}  // namespace loadforge::reporters
