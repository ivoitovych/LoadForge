// SPDX-License-Identifier: GPL-3.0-or-later
//
// T10, M5 exit criterion 3: a worker that is rotated for a long time grows
// nothing and loses nothing. The worker's loop runs for real in a thread (real
// runner, real executor, real workloads and oracles, real pipes) while the
// test plays the controller: it sends the commands of a mixed schedule at their
// times, for LOADFORGE_SOAK_SECONDS (default 20; the nightly job sets hours).
//
// What a soak can see that a short run cannot: a rotation that leaks a working
// set, a descriptor, or a thread; a tally that stops counting or goes backwards
// across a replacement; a count that wraps. So, around the whole run: the
// resident set (sampled at the end of every cycle of phases, and the largest
// of the later cycles compared with the largest of the first), the open
// descriptors and the threads are measured, every acknowledgement must be a
// Done, every tally frame must be no lower than the one before, and no unit
// may have been found wrong or left unchecked.
//
// The second test is the schedule alone: five hours of it, made, checked and
// walked, which costs nothing and fails the day a count could overflow.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "platform/clock.hpp"
#include "platform/fpenv/real_control.hpp"
#include "platform/isa/probe.hpp"
#include "platform/syscalls.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "verification/record.hpp"
#include "worker/control.hpp"
#include "worker/entry.hpp"
#include "worker/frames.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/scheduler.hpp"
#include "worker/services.hpp"
#include "worker/verified_run.hpp"
#include "workloads/mixed/mixed.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::worker {
namespace {

namespace fs = std::filesystem;

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::uint64_t kFootprint = std::uint64_t{8} << 20U;

std::int64_t soak_seconds() {
  const char* text = std::getenv("LOADFORGE_SOAK_SECONDS");  // NOLINT(concurrency-mt-unsafe)
  return text == nullptr ? 20 : std::strtoll(text, nullptr, 10);
}

/// A field of /proc/self/status, in the unit the kernel prints it.
std::int64_t status_field(const std::string& name) {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with(name + ":")) {
      return std::strtoll(line.c_str() + name.size() + 1, nullptr, 10);
    }
  }
  return -1;
}

std::int64_t open_descriptors() {
  std::int64_t count = 0;
  for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
    (void)entry;
    ++count;
  }
  return count;
}

TEST(MixedSoak, ARotatedWorkerGrowsNothingAndLosesNoVerification) {
  const std::int64_t seconds = soak_seconds();
  ASSERT_GT(seconds, 0) << "LOADFORGE_SOAK_SECONDS asks for no time";
  const workloads::BuiltinRegistry workloads;
  const verification::BuiltinOracleRegistry oracles;

  mixed::MixedOptions options;
  options.workers = 1;
  options.seed = 99;
  options.duration_ns = seconds * kSecond;
  options.rotation_ns = kSecond / 4;
  options.footprint_bytes = kFootprint;
  const auto roster = mixed::roster_of(workloads, kFootprint);
  ASSERT_GE(roster.size(), 8U);
  const auto made = mixed::make_mixed(roster, options);
  ASSERT_TRUE(made.has_value()) << made.error();
  const mixed::Schedule& schedule = made.value();

  const std::int64_t threads_before = status_field("Threads");
  const std::int64_t descriptors_before = open_descriptors();

  platform::RealSyscalls syscalls;
  auto created = ipc::HeartbeatRegion::create(syscalls, 1);
  ASSERT_TRUE(created.has_value());
  ipc::HeartbeatRegion region = std::move(created).value();
  platform::PipeEnds records = syscalls.create_pipe().value();
  platform::PipeEnds control = syscalls.create_pipe().value();
  platform::Clock clock(syscalls);
  ASSERT_TRUE(
      region
          .publish(ipc::kControllerSlot,
                   ipc::Heartbeat{clock.now().value().running_ns + 100'000 * kSecond, 1, 1, 1})
          .has_value());
  fpenv::RealFpControl fp;
  PosixThreads threads;
  const auto probe = isa::make_native_probe();
  Services services;
  services.workloads = &workloads;
  services.oracles = &oracles;
  services.fp = &fp;
  services.threads = &threads;
  services.isa = probe.get();
  services.make_runner = &verification::make_slice_runner;

  WorkerArgs args;
  args.slot = 0;
  args.workers = 1;
  args.pacing = Pacing{(seconds + 5) * kSecond, 20'000'000, 100'000 * kSecond};
  WorkloadArgs unit;
  unit.workload = schedule.start.at(0).workload;
  unit.seed = 5;
  unit.footprint_bytes = schedule.start.at(0).footprint_bytes;
  unit.threads = 1;
  unit.lock = verification::LockPolicy::kNone;

  core::Result<VerifiedOutcome, WorkerError> result{WorkerError{"not run", ""}};
  std::thread worker([&] {
    ipc::RecordWriter writer(syscalls, records.write_fd);
    const Session session{&region, 0, control.read_fd, args.pacing, &writer};
    VerifiedRun run(syscalls, writer, session, args, unit, services);
    result = run.run();
  });

  const auto begun = std::chrono::steady_clock::now();
  std::uint32_t sequence = 0;
  std::size_t next_step = 0;
  std::vector<std::int64_t> resident_at_cycle_end;
  std::size_t sent = 0;
  while (std::chrono::steady_clock::now() - begun < std::chrono::seconds(seconds)) {
    const std::int64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begun)
                                     .count();
    while (next_step < schedule.steps.size() && schedule.steps[next_step].at_ns <= elapsed) {
      for (const mixed::WorkerCommands& each : schedule.steps[next_step].workers) {
        for (const Command& command : each.commands) {
          const std::vector<std::byte> payload = encode(ControlMessage{++sequence, command});
          const std::vector<std::byte> frame = ipc::encode(kControlFrameType, payload).value();
          ASSERT_TRUE(syscalls.write(control.write_fd, frame).has_value());
          ++sent;
        }
      }
      if (next_step % 5 == 4) {  // the end of a cycle of the five phases
        resident_at_cycle_end.push_back(status_field("VmRSS"));
      }
      ++next_step;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const std::vector<std::byte> stop = encode(ControlMessage{++sequence, Stop{}});
  ASSERT_TRUE(
      syscalls.write(control.write_fd, ipc::encode(kControlFrameType, stop).value()).has_value());
  worker.join();
  ASSERT_TRUE(syscalls.close(records.write_fd).has_value());

  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().outcome.reason, StopReason::kStopRequested);

  std::size_t answered = 0;
  std::uint64_t previous_units = 0;
  std::uint64_t last_units = 0;
  std::size_t findings = 0;
  ipc::RecordSource source(syscalls, records.read_fd);
  while (!source.at_end()) {
    auto drained = source.drain();
    ASSERT_TRUE(drained.has_value());
    for (const ipc::Record& record : drained.value().records) {
      if (record.type == kControlAckFrameType) {
        const ControlAck ack = decode_ack(record.payload).value();
        EXPECT_EQ(ack.status, AckStatus::kDone) << ack.detail;
        ++answered;
      } else if (record.type == kTallyFrameType) {
        const auto tally = decode_tally(record.payload).value();
        EXPECT_GE(tally.units_computed, previous_units) << "the tally ran backwards";
        previous_units = tally.units_computed;
        last_units = tally.units_computed;
        EXPECT_EQ(tally.oracle_failed, 0U);
        EXPECT_EQ(tally.integrity_failed, 0U);
      } else if (record.type == verification::kErrorRecordFrameType) {
        ++findings;
      }
    }
  }
  EXPECT_EQ(answered, sent + 1) << "every message, and the Stop, was answered";
  EXPECT_EQ(findings, 0U) << "nothing wrong and nothing left unchecked";
  EXPECT_GT(last_units, 0U);
  EXPECT_LT(last_units, std::uint64_t{1} << 62U) << "a count that is near wrapping";

  EXPECT_TRUE(syscalls.close(records.read_fd).has_value());
  EXPECT_TRUE(syscalls.close(control.read_fd).has_value());
  EXPECT_TRUE(syscalls.close(control.write_fd).has_value());
  EXPECT_TRUE(region.release().has_value());
  EXPECT_EQ(status_field("Threads"), threads_before) << "a thread outlived the worker";
  EXPECT_EQ(open_descriptors(), descriptors_before) << "a descriptor outlived the worker";

  // The resident set: the largest of the later cycles may not exceed the
  // largest of the first by more than the footprint of one more working set.
  if (resident_at_cycle_end.size() >= 6) {
    const std::size_t third = resident_at_cycle_end.size() / 3;
    const auto cut = static_cast<std::ptrdiff_t>(third);
    const std::int64_t early =
        *std::max_element(resident_at_cycle_end.begin(), resident_at_cycle_end.begin() + cut);
    const std::int64_t late =
        *std::max_element(resident_at_cycle_end.end() - static_cast<std::ptrdiff_t>(third),
                          resident_at_cycle_end.end());
    EXPECT_LE(late, early + static_cast<std::int64_t>(kFootprint >> 10U) * 2)
        << "the resident set grew from " << early << " kB to " << late << " kB across rotations";
  }
}

TEST(MixedSoak, AFiveHourScheduleIsMadeWholeAndKeepsEveryCountInRange) {
  const workloads::BuiltinRegistry workloads;
  const auto roster = mixed::roster_of(workloads, std::uint64_t{64} << 20U);
  mixed::MixedOptions options;
  options.workers = 64;
  options.seed = 7;
  options.duration_ns = std::int64_t{5} * 3600 * kSecond;
  options.rotation_ns = 10 * kSecond;
  options.footprint_bytes = std::uint64_t{64} << 20U;
  options.placements = {{0}, {1}, {2}, {3}};
  const auto made = mixed::make_mixed(roster, options);
  ASSERT_TRUE(made.has_value()) << made.error();
  const mixed::Schedule& schedule = made.value();
  EXPECT_FALSE(mixed::problem(schedule, 64).has_value());
  EXPECT_EQ(schedule.steps.size(), 5U * 3600U / 10U - 1U)
      << "a rotation every ten seconds after the start";
  EXPECT_LE(schedule.steps.back().at_ns, options.duration_ns);
  std::set<std::string> phases;
  std::uint64_t commands = 0;
  for (const mixed::Step& step : schedule.steps) {
    phases.insert(step.phase);
    for (const mixed::WorkerCommands& each : step.workers) {
      commands += each.commands.size();
    }
  }
  EXPECT_EQ(phases.size(), schedule.steps.size()) << "no phase label repeats in five hours";
  EXPECT_LT(commands, mixed::kMaxCommands);
  EXPECT_GT(commands, schedule.steps.size()) << "every step told somebody something";
  const auto again = mixed::make_mixed(roster, options);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(mixed::digest(again.value()), mixed::digest(schedule)) << "the same five hours twice";
}

}  // namespace
}  // namespace loadforge::worker
