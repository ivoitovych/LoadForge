// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console reporter: exactly what a person sees, line for line.

#include "reporters/console.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/supervision.hpp"
#include "core/duration.hpp"
#include "core/version.hpp"

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
  reporter.run_starting(plan);
  EXPECT_EQ(out.str(), std::string{"loadforge "} + std::string{core::kVersion} +
                           ": stress for 30s with 2 worker(s), null workload\n"
                           "  executable: /usr/bin/loadforge\n"
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

}  // namespace
}  // namespace loadforge::reporters
