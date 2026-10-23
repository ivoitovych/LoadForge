// SPDX-License-Identifier: GPL-3.0-or-later
//
// The data a schedule is made of: how two of them compare, the labels, what is
// wrong with one that is to run a number of workers, and its digest.

#include "workloads/mixed/schedule.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "controller/journal/journal.hpp"
#include "worker/control.hpp"

namespace loadforge::mixed {
namespace {

Schedule a_schedule() {
  Schedule schedule;
  schedule.workers = 2;
  schedule.start = {Start{"fft", 1U << 20U}, Start{"sort", 4U << 20U}};
  schedule.phase = "r0 mixed";
  schedule.workload = "fft+sort";
  Step first;
  first.at_ns = 1000;
  first.phase = "r1 compute-heavy";
  first.workload = "fft";
  first.workers = {WorkerCommands{0, {worker::Switch{"fft", 7, 1U << 20U, 0, {1, 1}}}},
                   WorkerCommands{1, {worker::Pause{}, worker::Resume{}}}};
  Step second;
  second.at_ns = 2000;
  second.phase = "r2 move";
  second.workload = "fft";
  second.workers = {WorkerCommands{0, {worker::Rebind{{1, 2}}}},
                    WorkerCommands{1, {worker::SetDuty{500, 1'000'000'000}, worker::Stop{}}}};
  schedule.steps = {first, second};
  return schedule;
}

TEST(ScheduleEquality, EveryFieldOfEveryPartIsCompared) {
  const Schedule base = a_schedule();
  EXPECT_EQ(base, a_schedule());
  Schedule other = base;
  other.workers = 3;
  EXPECT_NE(base, other);
  other = base;
  other.start[0].workload = "x";
  EXPECT_NE(base, other);
  other = base;
  other.start[1].footprint_bytes = 1;
  EXPECT_NE(base, other);
  other = base;
  other.phase = "x";
  EXPECT_NE(base, other);
  other = base;
  other.workload = "x";
  EXPECT_NE(base, other);
  other = base;
  other.steps.pop_back();
  EXPECT_NE(base, other);
  other = base;
  other.steps[0].at_ns = 1;
  EXPECT_NE(base, other);
  other = base;
  other.steps[0].phase = "x";
  EXPECT_NE(base, other);
  other = base;
  other.steps[0].workload = "x";
  EXPECT_NE(base, other);
  other = base;
  other.steps[0].workers.pop_back();
  EXPECT_NE(base, other);
  other = base;
  other.steps[0].workers[0].slot = 1;
  EXPECT_NE(base, other);
  other = base;
  other.steps[0].workers[1].commands.pop_back();
  EXPECT_NE(base, other);
  other = base;
  other.steps[0].workers[1].commands[0] = worker::Resume{};
  EXPECT_NE(base, other);
  other = base;
  std::get<worker::Switch>(other.steps[0].workers[0].commands[0]).seed = 8;
  EXPECT_NE(base, other) << "a command is compared within its alternative";
}

TEST(ScheduleLabels, ALabelIsCutToTheJournalsLimitAndNoFurther) {
  EXPECT_EQ(bounded("short"), "short");
  EXPECT_EQ(bounded(""), "");
  EXPECT_EQ(bounded(std::string(journal::kMaxLabelBytes, 'a')),
            std::string(journal::kMaxLabelBytes, 'a'));
  EXPECT_EQ(bounded(std::string(journal::kMaxLabelBytes + 1, 'a')),
            std::string(journal::kMaxLabelBytes, 'a'));
  EXPECT_EQ(bounded(std::string(500, 'a')).size(), journal::kMaxLabelBytes);
}

TEST(ScheduleProblem, AValidScheduleHasNone) {
  EXPECT_FALSE(problem(a_schedule(), 2).has_value());
  Schedule none;
  none.workers = 4;
  EXPECT_FALSE(problem(none, 4).has_value())
      << "no start (the workers start as configured), no steps";
}

TEST(ScheduleProblem, EachWayAScheduleFailsToFitTheRunIsNamed) {
  EXPECT_EQ(problem(a_schedule(), 3).value(), "the schedule is for 2 worker(s) and the run has 3");
  {
    Schedule schedule = a_schedule();
    schedule.start.pop_back();
    EXPECT_EQ(problem(schedule, 2).value(),
              "the schedule names a start for some workers and not for all");
  }
  {
    Schedule schedule = a_schedule();
    schedule.phase = std::string(journal::kMaxLabelBytes + 1, 'p');
    EXPECT_NE(problem(schedule, 2).value().find("first phase or workload label"),
              std::string::npos);
    schedule = a_schedule();
    schedule.workload = std::string(journal::kMaxLabelBytes + 1, 'w');
    EXPECT_NE(problem(schedule, 2).value().find("first phase or workload label"),
              std::string::npos);
  }
  {
    Schedule schedule = a_schedule();
    schedule.start[1].workload = "";
    EXPECT_EQ(problem(schedule, 2).value(),
              "the start of worker 1 names no workload of a valid name or no footprint");
    schedule = a_schedule();
    schedule.start[0].workload = "Not A Name";
    EXPECT_NE(problem(schedule, 2).value().find("the start of worker 0"), std::string::npos);
    schedule = a_schedule();
    schedule.start[0].footprint_bytes = 0;
    EXPECT_NE(problem(schedule, 2).value().find("the start of worker 0"), std::string::npos);
  }
  {
    Schedule schedule = a_schedule();
    schedule.steps[1].phase = schedule.steps[0].phase;
    EXPECT_EQ(problem(schedule, 2).value(),
              "the step \"r1 compute-heavy\" repeats the label of an earlier step");
  }
  {
    Schedule schedule = a_schedule();
    schedule.steps[1].at_ns = 500;
    EXPECT_EQ(problem(schedule, 2).value(), "the step \"r2 move\" is not after the one before it");
    schedule.steps[1].at_ns = 1000;
    EXPECT_FALSE(problem(schedule, 2).has_value()) << "two steps may share an instant";
  }
  {
    Schedule schedule = a_schedule();
    schedule.steps[0].at_ns = -1;
    EXPECT_NE(problem(schedule, 2).value().find("not after the one before it"), std::string::npos)
        << "nothing before the run";
  }
  {
    Schedule schedule = a_schedule();
    schedule.steps[0].phase = std::string(journal::kMaxLabelBytes + 1, 'p');
    EXPECT_NE(problem(schedule, 2).value().find("a label past the journal's limit"),
              std::string::npos);
    schedule = a_schedule();
    schedule.steps[0].workload = std::string(journal::kMaxLabelBytes + 1, 'w');
    EXPECT_NE(problem(schedule, 2).value().find("a label past the journal's limit"),
              std::string::npos);
  }
  {
    Schedule schedule = a_schedule();
    schedule.steps[0].workers[1].slot = 2;
    EXPECT_EQ(problem(schedule, 2).value(), "the step \"r1 compute-heavy\" names worker 2 of 2");
  }
  {
    Schedule schedule = a_schedule();
    schedule.steps[1].workers[1].commands = {worker::SetDuty{0, 1}};
    EXPECT_EQ(problem(schedule, 2).value(),
              "the step \"r2 move\" sends worker 1 a set duty that breaks the rules of its "
              "command");
  }
}

TEST(ScheduleDigest, IsTheSameForTheSameScheduleAndDiffersForEveryChange) {
  const Schedule base = a_schedule();
  EXPECT_EQ(digest(base), digest(a_schedule()));
  Schedule other = base;
  const auto differs = [&base](const Schedule& changed) { return digest(changed) != digest(base); };
  other.workers = 3;
  EXPECT_TRUE(differs(other));
  other = base;
  other.start[0].workload = "x";
  EXPECT_TRUE(differs(other));
  other = base;
  other.start[1].footprint_bytes += 1;
  EXPECT_TRUE(differs(other));
  other = base;
  other.phase += "x";
  EXPECT_TRUE(differs(other));
  other = base;
  other.workload += "x";
  EXPECT_TRUE(differs(other));
  other = base;
  other.steps[0].at_ns += 1;
  EXPECT_TRUE(differs(other));
  other = base;
  other.steps[0].phase += "x";
  EXPECT_TRUE(differs(other));
  other = base;
  other.steps[0].workload += "x";
  EXPECT_TRUE(differs(other));
  other = base;
  other.steps[0].workers[0].slot = 1;
  EXPECT_TRUE(differs(other));
  other = base;
  std::get<worker::Switch>(other.steps[0].workers[0].commands[0]).decomposition = 1;
  EXPECT_TRUE(differs(other));
  other = base;
  std::get<worker::Switch>(other.steps[0].workers[0].commands[0]).duty.integrity_period = 2;
  EXPECT_TRUE(differs(other));
  other = base;
  std::get<worker::Switch>(other.steps[0].workers[0].commands[0]).duty.oracle_period = 2;
  EXPECT_TRUE(differs(other));
  other = base;
  std::get<worker::Switch>(other.steps[0].workers[0].commands[0]).footprint_bytes += 1;
  EXPECT_TRUE(differs(other));
  other = base;
  std::get<worker::Rebind>(other.steps[1].workers[0].commands[0]).cpus.push_back(3);
  EXPECT_TRUE(differs(other));
  other = base;
  std::get<worker::SetDuty>(other.steps[1].workers[1].commands[0]).busy_permille = 501;
  EXPECT_TRUE(differs(other));
  other = base;
  std::get<worker::SetDuty>(other.steps[1].workers[1].commands[0]).period_ns += 1;
  EXPECT_TRUE(differs(other));
  other = base;
  other.steps[0].workers[1].commands[0] = worker::Resume{};
  EXPECT_TRUE(differs(other));
}

TEST(ScheduleDigest, AnEmptyScheduleHasADigestToo) { EXPECT_NE(digest(Schedule{}), 0U); }

}  // namespace
}  // namespace loadforge::mixed
