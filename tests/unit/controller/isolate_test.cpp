// SPDX-License-Identifier: GPL-3.0-or-later
//
// Isolation over a finished run: which findings go up the ladder, which are
// left with a stated reason, and a child process that must leave at once.

#include "controller/isolate.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "unit/controller/consumer_models.hpp"
#include "unit/controller/machine_tree.hpp"
#include "verification/isolation.hpp"

namespace loadforge::controller {
namespace {

using controller_testing::MachineShape;
using controller_testing::MachineTree;
using verification::RerunOutcome;

/// A re-runner that answers every request the same and remembers what it was
/// asked and which run it was told about.
class RecordingRerunner final : public ChildAwareRerunner {
 public:
  void use(const WorkerWork& work) override { used.push_back(work.descriptor.name); }
  [[nodiscard]] RerunOutcome rerun(const verification::WrongResult& /*finding*/,
                                   const verification::RerunRequest& request) override {
    requests.push_back(request);
    return answer;
  }
  [[nodiscard]] std::optional<int> child_exit_code() const noexcept override {
    return requests.size() >= child_after ? std::optional<int>{120} : std::nullopt;
  }
  RerunOutcome answer = RerunOutcome::kClean;
  std::size_t child_after = ~std::size_t{0};
  std::vector<std::string> used;
  std::vector<verification::RerunRequest> requests;
};

verification::ExactBitCorruption wrong_on(std::optional<std::uint32_t> cpu) {
  verification::ExactBitCorruption record;
  record.provenance.logical_cpu = cpu;
  record.check = "decoded output against input";
  return record;
}

FindingReport finding_of(verification::ErrorRecord record) {
  FindingReport finding;
  finding.record = std::move(record);
  return finding;
}

WorkerReport worker_with(std::vector<FindingReport> findings, bool real = true,
                         std::uint32_t threads = 1) {
  WorkerReport worker;
  if (real) {
    WorkerWork work;
    work.descriptor.name = "compression";
    work.threads = threads;
    worker.work = work;
  }
  worker.findings = std::move(findings);
  return worker;
}

TEST(IsolateFindings, EachWrongResultGoesUpTheLadderOnItsRecordedCpu) {
  const MachineTree tree(MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  RecordingRerunner rerunner;
  rerunner.answer = RerunOutcome::kClean;
  RunReport report;
  report.workers.push_back(worker_with({finding_of(wrong_on(2)), finding_of(wrong_on(3))}));
  const IsolationSummary summary = isolate_findings(report, &machine, isolator, rerunner, 10);
  EXPECT_EQ(summary.isolated, 2U);
  EXPECT_FALSE(summary.child_exit_code.has_value());
  EXPECT_EQ(rerunner.used, (std::vector<std::string>{"compression"}));
  ASSERT_EQ(rerunner.requests.size(), 2U) << "a clean first rung stops each ladder";
  EXPECT_EQ(rerunner.requests[0].cpu, 2U);
  EXPECT_EQ(rerunner.requests[1].cpu, 3U);
  for (const FindingReport& finding : report.workers[0].findings) {
    ASSERT_TRUE(finding.isolation.has_value());
    EXPECT_EQ(finding.isolation->finding, verification::Finding::kNonReproducibleTransient);
    EXPECT_TRUE(finding.not_isolated.empty());
  }
}

TEST(IsolateFindings, TheThreadsOfThePlacementAreTheWorkersAndAnotherCoreIsOffered) {
  const MachineTree tree(MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  RecordingRerunner rerunner;
  rerunner.answer = RerunOutcome::kReproduced;
  RunReport report;
  report.workers.push_back(worker_with({finding_of(wrong_on(1))}, true, 2));
  ASSERT_EQ(isolate_findings(report, &machine, isolator, rerunner, 10).isolated, 1U);
  ASSERT_EQ(rerunner.requests.size(), 3U);
  EXPECT_EQ(rerunner.requests[0].threads, 2U);
  EXPECT_EQ(rerunner.requests[1].rung, verification::Rung::kOtherCore);
  EXPECT_NE(rerunner.requests[1].cpu, 1U);
  EXPECT_EQ(rerunner.requests[2].threads, 1U);
  EXPECT_EQ(report.workers[0].findings[0].isolation->finding, verification::Finding::kNotCoreLocal);
}

TEST(IsolateFindings, WhatIsNotIsolatedSaysWhyAndIsNeverMadeUpAsAnIsolation) {
  const MachineTree tree(MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  RecordingRerunner rerunner;
  RunReport report;
  verification::VerificationNotPerformed unrun;
  unrun.what = "oracle";
  unrun.why = "none sampled";
  report.workers.push_back(worker_with({finding_of(unrun), finding_of(wrong_on(std::nullopt))}));
  report.workers.push_back(worker_with({finding_of(wrong_on(1))}, false));  // The null workload.
  const IsolationSummary summary = isolate_findings(report, &machine, isolator, rerunner, 10);
  EXPECT_EQ(summary.isolated, 0U);
  EXPECT_TRUE(rerunner.requests.empty());
  EXPECT_EQ(report.workers[0].findings[0].not_isolated,
            "a check that was not made: there is no wrong result to repeat");
  EXPECT_EQ(report.workers[0].findings[1].not_isolated,
            "the worker ran on several CPUs, so the record names none to repeat it on");
  EXPECT_TRUE(report.workers[1].findings[0].not_isolated.empty())
      << "a worker of no workload is not this function's";
  for (const WorkerReport& worker : report.workers) {
    for (const FindingReport& finding : worker.findings) {
      EXPECT_FALSE(finding.isolation.has_value());
    }
  }
}

TEST(IsolateFindings, WithoutATopologyTheOtherCoresCannotBeNamed) {
  controller_testing::ModelIsolator isolator;
  RecordingRerunner rerunner;
  RunReport report;
  report.workers.push_back(worker_with({finding_of(wrong_on(1))}));
  EXPECT_EQ(isolate_findings(report, nullptr, isolator, rerunner, 10).isolated, 0U);
  EXPECT_EQ(report.workers[0].findings[0].not_isolated,
            "no topology snapshot to name another core from");
}

TEST(IsolateFindings, TheLimitIsPerWorkerSoOneFaultyCoreDoesNotStarveTheOthers) {
  const MachineTree tree(MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  RecordingRerunner rerunner;
  RunReport report;
  report.workers.push_back(
      worker_with({finding_of(wrong_on(0)), finding_of(wrong_on(0)), finding_of(wrong_on(0))}));
  report.workers.push_back(worker_with({finding_of(wrong_on(1))}));
  const IsolationSummary summary = isolate_findings(report, &machine, isolator, rerunner, 2);
  EXPECT_EQ(summary.isolated, 3U);
  EXPECT_TRUE(report.workers[0].findings[1].isolation.has_value());
  EXPECT_FALSE(report.workers[0].findings[2].isolation.has_value());
  EXPECT_EQ(report.workers[0].findings[2].not_isolated, "past the limit of 2 isolations");
  EXPECT_TRUE(report.workers[1].findings[0].isolation.has_value());
}

TEST(IsolateFindings, AProcessThatTurnedOutToBeAFailedChildStopsEverythingAtOnce) {
  const MachineTree tree(MachineShape{4, 64, false});
  const topology::Snapshot machine = tree.accepted();
  controller_testing::ModelIsolator isolator;
  RecordingRerunner rerunner;
  rerunner.child_after = 1;
  RunReport report;
  report.workers.push_back(worker_with({finding_of(wrong_on(0)), finding_of(wrong_on(1))}));
  const IsolationSummary summary = isolate_findings(report, &machine, isolator, rerunner, 10);
  ASSERT_TRUE(summary.child_exit_code.has_value());
  EXPECT_EQ(*summary.child_exit_code, 120);
  EXPECT_EQ(summary.isolated, 1U);
  EXPECT_EQ(rerunner.requests.size(), 1U);
  EXPECT_FALSE(report.workers[0].findings[1].isolation.has_value());
}

}  // namespace
}  // namespace loadforge::controller
