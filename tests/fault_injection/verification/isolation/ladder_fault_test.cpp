// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fault injection for the re-run ladder (T5) and its unrun checks (T5b).
//
// A fault is injected into a modelled machine -- on one core, on a cache
// cluster, on a node, everywhere, only under concurrency, or nowhere a re-run
// can find it -- and the placement the isolation module chooses from a real
// discovery of a layout decides where the ladder looks. Each test asserts the
// finding that fault must produce. The unrun checks make a re-run impossible
// and assert that the ladder says inconclusive or unlocated, never clean.
//
// Last, every path: both kinds of wrong result, a one- and a many-thread
// original, with and without another core, and each outcome on each rung,
// checked against the ladder's rules as the contract states them.

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "topology/snapshot.hpp"
#include "unit/verification/isolation/layouts.hpp"
#include "verification/isolation.hpp"
#include "verification/isolation/ladder.hpp"
#include "verification/isolation/placement.hpp"
#include "verification/record.hpp"

namespace loadforge::verification::isolation {
namespace {

/// A machine with a fault in it: a unit run on a faulty CPU, with at least
/// `threads_to_fail` threads, comes out wrong; a worker cannot be started on
/// an unreachable CPU; everything else comes out clean.
class FaultyMachine final : public Rerunner {
 public:
  FaultyMachine(std::set<std::uint32_t> faulty, std::uint32_t threads_to_fail,
                std::set<std::uint32_t> unreachable = {})
      : faulty_(std::move(faulty)),
        threads_to_fail_(threads_to_fail),
        unreachable_(std::move(unreachable)) {}

  RerunOutcome rerun(const WrongResult& /*finding*/, const RerunRequest& request) override {
    requests.push_back(request);
    if (unreachable_.contains(request.cpu)) {
      return RerunOutcome::kNotPerformed;
    }
    const bool fails = faulty_.contains(request.cpu) && request.threads >= threads_to_fail_;
    return fails ? RerunOutcome::kReproduced : RerunOutcome::kClean;
  }
  std::vector<RerunRequest> requests;

 private:
  std::set<std::uint32_t> faulty_;
  std::uint32_t threads_to_fail_;
  std::set<std::uint32_t> unreachable_;
};

WrongResult exact() {
  ExactBitCorruption record;
  record.provenance.iteration = 41;
  record.check = "round trip";
  return record;
}

WrongResult numerical() {
  NumericalVerificationFailure record;
  record.provenance.iteration = 41;
  record.check = "residual";
  record.residual = 3.0;
  record.tolerance = 1.0;
  return record;
}

Placement place(void (*build)(layouts::Tree&), const char* name, std::uint32_t cpu,
                std::uint32_t threads) {
  layouts::Tree tree(name);
  build(tree);
  const auto snapshot = tree.take();
  if (!snapshot.has_value()) {
    ADD_FAILURE() << "discovery refused the " << name << " layout";
    return Placement{cpu, threads, {}};
  }
  return placement_for(snapshot.value(), cpu, threads);
}

// --- T5: a fault injected, its finding asserted -------------------------------

TEST(InjectedFault, OneCoreOfAnSmtMachineIsFoundCoreLocal) {
  // The fault is in core 0, so both of its threads (cpu 0 and 4) fail. Were
  // the sibling chosen as "another core", it would reproduce there and the
  // finding would be not core-local: the wrong answer.
  FaultyMachine machine({0, 4}, 1);
  const Placement placement = place(layouts::smt, "fi-smt", 0, 4);
  const auto report = LadderIsolator().isolate(exact(), placement, machine);
  EXPECT_EQ(report.finding, Finding::kCoreLocal);
  EXPECT_EQ(report.confidence, Confidence::kMedium);
  for (const RerunRequest& request : machine.requests) {
    EXPECT_NE(request.cpu, 4U) << "the sibling shares the core under test";
  }
}

TEST(InjectedFault, ASharedClusterCacheIsFoundNotCoreLocal) {
  // The fault is in the L2 the first efficiency cluster shares. The nearest
  // other core is in the cluster, so the fault recurs there: not core-local,
  // consistent with what the cores share. A core across the cluster would
  // have come out clean and called a cache fault core-local.
  FaultyMachine machine({4, 5, 6, 7}, 1);
  const Placement placement = place(layouts::hybrid, "fi-hybrid", 4, 2);
  const auto report = LadderIsolator().isolate(numerical(), placement, machine);
  ASSERT_EQ(machine.requests.size(), 3U);
  EXPECT_EQ(machine.requests[1].cpu, 5U);
  EXPECT_EQ(report.finding, Finding::kNotCoreLocal);
  EXPECT_EQ(report.confidence, Confidence::kHigh);
}

TEST(InjectedFault, ANodesMemoryIsFoundNotCoreLocal) {
  FaultyMachine machine({0, 1}, 1);
  const Placement placement = place(layouts::two_sockets, "fi-sockets", 0, 2);
  const auto report = LadderIsolator().isolate(exact(), placement, machine);
  EXPECT_EQ(machine.requests[1].cpu, 1U) << "the other core of node 0";
  EXPECT_EQ(report.finding, Finding::kNotCoreLocal);
}

TEST(InjectedFault, AFaultEverywhereIsNotCoreLocalWithHighConfidence) {
  FaultyMachine machine({0, 1, 2, 3}, 1);
  const Placement placement = place(layouts::symmetric, "fi-everywhere", 2, 4);
  const auto report = LadderIsolator().isolate(exact(), placement, machine);
  EXPECT_EQ(report.answers, (Answers{Answer::kYes, Answer::kYes, Answer::kYes}));
  EXPECT_EQ(report.finding, Finding::kNotCoreLocal);
  EXPECT_EQ(report.confidence, Confidence::kHigh);
}

TEST(InjectedFault, AFaultOnlyUnderConcurrencyIsConcurrencyDependent) {
  FaultyMachine machine({0, 1, 2, 3}, 2);
  const Placement placement = place(layouts::symmetric, "fi-concurrency", 1, 4);
  const auto report = LadderIsolator().isolate(numerical(), placement, machine);
  EXPECT_EQ(report.answers.single_thread, Answer::kNo);
  EXPECT_EQ(report.finding, Finding::kConcurrencyDependent);
  EXPECT_EQ(report.confidence, Confidence::kLow);
}

TEST(InjectedFault, AFaultThatDoesNotRecurIsATransientAndNothingMoreIsRerun) {
  FaultyMachine machine({}, 1);
  const Placement placement = place(layouts::symmetric, "fi-transient", 1, 4);
  const auto report = LadderIsolator().isolate(exact(), placement, machine);
  EXPECT_EQ(machine.requests.size(), 1U);
  EXPECT_EQ(report.finding, Finding::kNonReproducibleTransient);
  EXPECT_EQ(report.confidence, Confidence::kLow);
}

TEST(InjectedFault, OnASingleCoreAReproducibleFaultCannotBeLocated) {
  FaultyMachine machine({0}, 1);
  const Placement placement = place(layouts::single_core_smt, "fi-single", 0, 2);
  const auto report = LadderIsolator().isolate(exact(), placement, machine);
  EXPECT_EQ(machine.requests,
            (std::vector<RerunRequest>{{Rung::kSameCore, 0, 2}, {Rung::kSingleThread, 0, 1}}));
  EXPECT_EQ(report.finding, Finding::kReproducibleUnlocated);
  EXPECT_EQ(report.confidence, Confidence::kMedium);
}

TEST(InjectedFault, OnAHeterogeneousPackageTheMoveStaysOnTheSameKindOfCore) {
  // A big core with a fault of its own: the other core is another big one,
  // which comes out clean, and the finding names the core.
  FaultyMachine machine({6}, 1);
  const Placement placement = place(layouts::big_little, "fi-big-little", 6, 3);
  const auto report = LadderIsolator().isolate(exact(), placement, machine);
  ASSERT_GE(machine.requests.size(), 2U);
  EXPECT_GE(machine.requests[1].cpu, 4U) << "a big core";
  EXPECT_EQ(report.finding, Finding::kCoreLocal);
}

// --- T5b: a re-run that cannot be made is never a clean one ----------------

TEST(UnrunCheck, NoWorkerAnywhereIsInconclusiveNeverATransient) {
  FaultyMachine machine({}, 1, {0, 1, 2, 3});
  const Placement placement = place(layouts::symmetric, "unrun-anywhere", 0, 4);
  const auto report = LadderIsolator().isolate(exact(), placement, machine);
  EXPECT_EQ(report.answers.same_core, Answer::kUnknown);
  EXPECT_EQ(report.finding, Finding::kInconclusive)
      << "every re-run 'passed' by not running: the most alarming finding is not the answer";
  EXPECT_EQ(machine.requests.size(), 1U);
}

TEST(UnrunCheck, AnUnreachableOtherCoreLeavesTheFaultUnlocatedNotCoreLocal) {
  FaultyMachine machine({0}, 1, {1});
  const Placement placement = place(layouts::symmetric, "unrun-other", 0, 2);
  const auto report = LadderIsolator().isolate(numerical(), placement, machine);
  EXPECT_EQ(report.answers.other_core, Answer::kUnknown);
  EXPECT_EQ(report.finding, Finding::kReproducibleUnlocated)
      << "a core that could not be tried did not come out clean";
}

// --- every path ----------------------------------------------------------

constexpr RerunOutcome kOutcomes[] = {RerunOutcome::kReproduced, RerunOutcome::kClean,
                                      RerunOutcome::kNotPerformed};

/// Answers each rung from a fixed outcome per rung.
class Fixed final : public Rerunner {
 public:
  Fixed(RerunOutcome same, RerunOutcome other, RerunOutcome single)
      : same_(same), other_(other), single_(single) {}
  RerunOutcome rerun(const WrongResult& /*finding*/, const RerunRequest& request) override {
    requests.push_back(request);
    switch (request.rung) {
      case Rung::kSameCore:
        return same_;
      case Rung::kOtherCore:
        return other_;
      case Rung::kSingleThread:
        return single_;
    }
    return RerunOutcome::kNotPerformed;
  }
  std::vector<RerunRequest> requests;

 private:
  RerunOutcome same_;
  RerunOutcome other_;
  RerunOutcome single_;
};

/// The answer a step's outcome gives, as the report documents it.
Answer documented_answer(RerunOutcome outcome) {
  if (outcome == RerunOutcome::kReproduced) {
    return Answer::kYes;
  }
  return outcome == RerunOutcome::kClean ? Answer::kNo : Answer::kUnknown;
}

std::optional<Step> step_of(const IsolationReport& report, Rung rung) {
  for (const Step& step : report.steps) {
    if (step.request.rung == rung) {
      return step;
    }
  }
  return std::nullopt;
}

TEST(EveryPath, EachOutcomeOfEachRungForEitherKindOfWrongResult) {
  int paths = 0;
  for (const WrongResult& finding : {exact(), numerical()}) {
    for (const std::uint32_t threads : {1U, 3U}) {
      for (const bool has_other : {false, true}) {
        for (const RerunOutcome same : kOutcomes) {
          for (const RerunOutcome other : kOutcomes) {
            for (const RerunOutcome single : kOutcomes) {
              SCOPED_TRACE(::testing::Message()
                           << "kind " << finding.index() << " threads " << threads << " other "
                           << has_other << " outcomes " << describe(same) << "/" << describe(other)
                           << "/" << describe(single));
              ++paths;
              const Placement placement{
                  5, threads,
                  has_other ? std::vector<std::uint32_t>{9, 2} : std::vector<std::uint32_t>{}};
              Fixed rerunner(same, other, single);
              const auto report = LadderIsolator().isolate(finding, placement, rerunner);

              // The first rung, always, as the original ran.
              ASSERT_FALSE(report.steps.empty());
              EXPECT_EQ(report.steps.front(), (Step{{Rung::kSameCore, 5, threads}, same}));
              const bool climbs = same == RerunOutcome::kReproduced;

              // Another core: made exactly when the ladder climbs and one is
              // offered, on the first offered, with the original threads.
              const auto other_step = step_of(report, Rung::kOtherCore);
              EXPECT_EQ(other_step.has_value(), climbs && has_other);
              if (other_step) {
                EXPECT_EQ(*other_step, (Step{{Rung::kOtherCore, 9, threads}, other}));
              }

              // One thread: made exactly when the ladder climbs and the
              // original ran more than one.
              const auto single_step = step_of(report, Rung::kSingleThread);
              EXPECT_EQ(single_step.has_value(), climbs && threads != 1);
              if (single_step) {
                EXPECT_EQ(*single_step, (Step{{Rung::kSingleThread, 5, 1}, single}));
              }

              // The steps in rung order, each one the re-runner was asked for.
              EXPECT_EQ(report.steps.size(), rerunner.requests.size());
              for (std::size_t i = 1; i < report.steps.size(); ++i) {
                EXPECT_LT(report.steps[i - 1].request.rung, report.steps[i].request.rung);
              }

              // Each answer as documented: a step's outcome, the same-core
              // answer for a one-thread original, unknown for a rung not run.
              EXPECT_EQ(report.answers.same_core, documented_answer(same));
              EXPECT_EQ(report.answers.other_core,
                        other_step ? documented_answer(other) : Answer::kUnknown);
              Answer single_expected = Answer::kUnknown;
              if (threads == 1) {
                single_expected = documented_answer(same);
              } else if (single_step) {
                single_expected = documented_answer(single);
              }
              EXPECT_EQ(report.answers.single_thread, single_expected);

              // The table and the confidence, and the one finding a re-run
              // that did not happen must never produce.
              EXPECT_EQ(report.finding, derive_finding(report.answers));
              EXPECT_EQ(report.confidence, confidence_of(report.finding));
              if (report.finding == Finding::kNonReproducibleTransient) {
                EXPECT_EQ(same, RerunOutcome::kClean);
              }
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(paths, 2 * 2 * 2 * 27);
}

}  // namespace
}  // namespace loadforge::verification::isolation
