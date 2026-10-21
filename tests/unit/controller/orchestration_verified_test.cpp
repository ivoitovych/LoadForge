// SPDX-License-Identifier: GPL-3.0-or-later
//
// The controller with workloads that verify themselves: what it tells each
// worker, what it keeps of what each worker sends, and what it refuses to
// believe. The workers are modelled by the fake's pipes; the frames are the
// real codec's, because that is what a worker puts on the wire.

#include <gtest/gtest.h>

#include <csignal>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "controller/orchestration.hpp"
#include "platform/fpenv/environment.hpp"
#include "primitives/rng/philox.hpp"
#include "support/fake_syscalls.hpp"
#include "unit/controller/consumer_models.hpp"
#include "unit/controller/machine_tree.hpp"
#include "verification/record.hpp"
#include "worker/entry.hpp"
#include "worker/frames.hpp"
#include "worker/ipc/record.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::controller {
namespace {

using loadforge::platform::Reaped;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;
constexpr pid_t kFirstPid = 1001;
constexpr pid_t kSecondPid = 1002;
constexpr int kRecordRead0 = 200;
constexpr int kRecordWrite0 = 201;
constexpr int kControlRead0 = 202;
constexpr int kRecordRead1 = 204;
constexpr int kRecordWrite1 = 205;
constexpr int kControlRead1 = 206;

class Watcher final : public Reporter {
 public:
  void run_starting(const RunPlan& plan) override {
    plans.push_back(plan);
    if (on_starting) {
      on_starting();
    }
  }
  void event(std::int64_t /*elapsed_ns*/, std::string_view line) override {
    if (on_event) {
      on_event(std::string{line});
    }
  }
  void run_finished(const RunReport& report) override { reports.push_back(report); }
  void isolation_finished(const RunReport& /*report*/) override {}
  std::vector<RunPlan> plans;
  std::vector<RunReport> reports;
  std::function<void()> on_starting;
  std::function<void(const std::string&)> on_event;
};

std::string frame_of(std::uint16_t type, const std::vector<std::byte>& payload) {
  auto encoded = ipc::encode(type, payload);
  EXPECT_TRUE(encoded.has_value());
  std::string text;
  for (const std::byte b : encoded.value()) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return text;
}

verification::ExactBitCorruption a_wrong_result(const std::string& workload = "compression") {
  verification::ExactBitCorruption record;
  record.provenance.workload = workload;
  record.provenance.iteration = 3;
  record.provenance.logical_cpu = 0;
  record.check = "decoded output against input";
  return record;
}

verification::VerificationNotPerformed a_check_not_made() {
  verification::VerificationNotPerformed record;
  record.provenance.workload = "compression";
  record.what = "oracle over compression units";
  record.why = "no unit was sampled";
  return record;
}

verification::Tally a_tally(std::uint64_t units) {
  verification::Tally tally;
  tally.units_computed = units;
  tally.oracle_checked = units;
  tally.oracle_blocks = units * 2;
  tally.integrity_checked = units;
  return tally;
}

struct Rig {
  FakeSyscalls syscalls;
  Watcher reporter;
  config::Config config;
  RunOptions options;
  workloads::BuiltinRegistry registry;
  controller_testing::ModelProbe probe{simd::IsaSet{simd::IsaPath::kScalar}};
  std::vector<FakeSyscalls::WaitStep> reaps;

  explicit Rig(const std::string& name = "compression") {
    config.run.mode = config::Mode::kStress;
    config.run.duration = core::Duration{50};
    config.workloads.push_back(config::Workload{name, config::Threads::count(1), true, {}});
    config.workloads.push_back(config::Workload{name, config::Threads::count(1), true, {}});
    options.executable = "/opt/loadforge";
    options.interval_ns = 10 * kMs;
    options.stale_after_ns = 100 * kMs;
    options.grace = Grace{20 * kMs, 20 * kMs};
    options.verification.registry = &registry;
    options.verification.isa_probe = &probe;
    options.verification.line_bytes = 64;
    options.verification.cpus = {0, 1};
    options.verification.footprint_bytes = std::uint64_t{2} << 20U;
    syscalls.advance_clocks_per_read(10 * kMs);
    syscalls.script_forks({{kFirstPid, 0}, {kSecondPid, 0}});
    reporter.on_starting = [this] {
      for (const int fd : {kRecordWrite0, kControlRead0, kRecordWrite1, kControlRead1}) {
        syscalls.worker_holds_end(fd);
      }
    };
  }

  core::Result<RunReport, ControllerError> run() {
    Controller controller(syscalls, reporter);
    return controller.run(config, options);
  }

  void deliver(std::size_t slot, std::uint16_t type, const std::vector<std::byte>& payload) {
    syscalls.deliver_to_pipe(slot == 0 ? kRecordRead0 : kRecordRead1, frame_of(type, payload));
  }
  void deliver_record(std::size_t slot, const verification::ErrorRecord& record) {
    deliver(slot, verification::kErrorRecordFrameType, verification::encode(record));
  }
  void deliver_tally(std::size_t slot, const verification::Tally& tally) {
    deliver(slot, worker::kTallyFrameType, worker::encode_tally(tally));
  }
  void deliver_finished(std::size_t slot) {
    const std::string text = "done";
    std::vector<std::byte> payload;
    for (const char c : text) {
      payload.push_back(static_cast<std::byte>(c));
    }
    deliver(slot, static_cast<std::uint16_t>(ipc::RecordType::kFinished), payload);
  }
  void worker_exits(std::size_t slot) {
    syscalls.worker_releases_end(slot == 0 ? kRecordWrite0 : kRecordWrite1);
    syscalls.worker_releases_end(slot == 0 ? kControlRead0 : kControlRead1);
    reaps.push_back(FakeSyscalls::WaitStep{Reaped{slot == 0 ? kFirstPid : kSecondPid, 0}, 0});
    syscalls.script_nonblocking_reaps(reaps);
  }
  /// Both workers deliver nothing more and exit, once the run has started.
  void both_exit_after(const std::function<void()>& deliveries) {
    reporter.on_event = [this, deliveries](const std::string& line) {
      if (line.substr(0, 7) == "spawned") {
        deliveries();
        deliver_finished(0);
        deliver_finished(1);
        worker_exits(0);
        worker_exits(1);
      }
    };
  }
};

TEST(ControllerVerified, TheBannerStatesEveryWorkersPlanTheRegistersAndTheCapabilities) {
  Rig rig;
  controller_testing::ModelFp<true, false> fp;
  controller_testing::ModelSource source;
  rig.options.fp = &fp;
  rig.options.sources = {&source};
  rig.both_exit_after([] {});
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_EQ(rig.reporter.plans.size(), 1U);
  const RunPlan& plan = rig.reporter.plans[0];
  ASSERT_EQ(plan.workloads.size(), 2U);
  EXPECT_NE(plan.workloads[0].find("compression: 1 thread(s) on cpu 0, footprint 2MiB"),
            std::string::npos)
      << plan.workloads[0];
  EXPECT_NE(plan.workloads[1].find("on cpu 1"), std::string::npos) << plan.workloads[1];
  EXPECT_EQ(plan.environment,
            "floating point (asserted at startup): rounding to nearest, ties to even, "
            "flush-to-zero off, denormals-are-zero off");
  EXPECT_NE(plan.capabilities.find("model.temperature"), std::string::npos) << plan.capabilities;
  EXPECT_NE(plan.capabilities.find("run as a user that may read it"), std::string::npos)
      << "an absence carries its remedy";
  EXPECT_TRUE(source.probed()) << "probed before the first fork";
}

TEST(ControllerVerified, WithoutRegistersOrSourcesTheBannerSaysNothingOfThem) {
  Rig rig("first");  // Not a registered workload: the null workload of M1.
  rig.both_exit_after([] {});
  ASSERT_TRUE(rig.run().has_value());
  const RunPlan& plan = rig.reporter.plans[0];
  EXPECT_EQ(plan.workloads,
            (std::vector<std::string>{"first: null workload", "first: null workload"}));
  EXPECT_TRUE(plan.environment.empty());
  EXPECT_TRUE(plan.capabilities.empty());
}

/// Registers that ignore writes, so that establishing the required state fails.
class StuckFp final : public fpenv::FpControl {
 public:
  [[nodiscard]] fpenv::FpState read() const override {
    fpenv::FpState state = fpenv::required_state(true, false);
    state.flush_to_zero = true;
    return state;
  }
  void write(const fpenv::FpState& /*state*/) override {}
};

TEST(ControllerVerified, ARunDoesNotStartOnRegistersItCannotPutRight) {
  Rig rig;
  StuckFp fp;
  rig.options.fp = &fp;
  auto result = rig.run();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "floating-point environment");
  EXPECT_NE(result.error().detail.find("flush_to_zero"), std::string::npos)
      << result.error().detail;
  EXPECT_EQ(rig.syscalls.fork_count(), 0);
}

TEST(ControllerVerified, AWorkloadThatCannotBePlannedRefusesTheRunBeforeAnyFork) {
  Rig rig;
  rig.options.verification.footprint_bytes = 1;
  auto result = rig.run();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().stage, "workload");
  EXPECT_NE(result.error().detail.find("compression: footprint too small"), std::string::npos)
      << result.error().detail;
  EXPECT_EQ(rig.syscalls.fork_count(), 0);
  EXPECT_TRUE(rig.syscalls.all_descriptors_closed());
}

TEST(ControllerVerified, EachWorkerIsHandedItsUnitItsCpusAndItsOwnSeed) {
  Rig rig;
  rig.options.verification.seed = 5;
  rig.options.verification.requested_isa = simd::IsaPath::kScalar;
  rig.options.verification.duty = verification::DutyCycle{2, 4};
  rig.syscalls.script_forks({{0, 0}});  // This process is the first worker; its exec fails.
  rig.syscalls.set_parent_pid(FakeSyscalls::kFakePid);
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  ASSERT_TRUE(result.value().worker_exit_code.has_value());
  std::vector<std::string_view> tail;
  const std::vector<std::string>& argv = rig.syscalls.last_exec_argv();
  for (std::size_t at = 2; at < argv.size(); ++at) {
    tail.emplace_back(argv[at]);
  }
  auto parsed = worker::parse_args(tail);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value().cpus, (std::vector<std::uint32_t>{0}));
  ASSERT_TRUE(parsed.value().workload.has_value());
  const worker::WorkloadArgs& unit = *parsed.value().workload;
  EXPECT_EQ(unit.workload, "compression");
  EXPECT_EQ(unit.seed, 5U);
  EXPECT_EQ(unit.threads, 1U);
  EXPECT_EQ(unit.isa, simd::IsaPath::kScalar);
  EXPECT_EQ(unit.duty, (verification::DutyCycle{2, 4}));
  EXPECT_EQ(unit.line_bytes, 64U);
}

TEST(ControllerVerified, AWorkersTallyAndErrorRecordsAreKeptAndCounted) {
  Rig rig;
  rig.both_exit_after([&rig] {
    rig.deliver_tally(0, a_tally(5));
    rig.deliver_record(0, a_wrong_result());
    rig.deliver_record(0, a_check_not_made());
    rig.deliver_tally(0, a_tally(9));
    rig.deliver_tally(1, a_tally(4));
  });
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const WorkerReport& first = result.value().workers[0];
  EXPECT_EQ(first.fate, WorkerFate::kCompleted);
  EXPECT_TRUE(first.tally_seen);
  EXPECT_EQ(first.tally, a_tally(9)) << "the last of the cumulative tallies";
  EXPECT_EQ(first.wrong_results, 1U);
  EXPECT_EQ(first.not_performed, 1U);
  ASSERT_EQ(first.findings.size(), 2U);
  EXPECT_EQ(first.findings[0].record, verification::ErrorRecord{a_wrong_result()});
  EXPECT_EQ(first.findings[1].record, verification::ErrorRecord{a_check_not_made()});
  ASSERT_TRUE(first.work.has_value());
  EXPECT_EQ(first.work->descriptor.name, "compression");
  const WorkerReport& second = result.value().workers[1];
  EXPECT_EQ(second.tally, a_tally(4));
  EXPECT_EQ(second.wrong_results, 0U);
  EXPECT_TRUE(second.findings.empty());
  EXPECT_EQ(wrong_result_count(result.value()), 1U);
  EXPECT_EQ(not_performed_count(result.value()), 1U);
  EXPECT_TRUE(all_clean(result.value())) << "both completed: what they found is not their fate";
}

TEST(ControllerVerified, ARunKeepsTheFirstFindingsAndCountsTheRest) {
  Rig rig;
  rig.options.grace = Grace{5000 * kMs, 5000 * kMs};  // Time enough to read it all, on this clock.
  rig.both_exit_after([&rig] {
    for (std::size_t each = 0; each < kMaxFindingsKept + 3; ++each) {
      rig.deliver_record(0, a_wrong_result());
    }
  });
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  const WorkerReport& first = result.value().workers[0];
  EXPECT_EQ(first.findings.size(), kMaxFindingsKept);
  EXPECT_EQ(first.findings_dropped, 3U);
  EXPECT_EQ(first.wrong_results, kMaxFindingsKept + 3) << "the count is exact";
}

/// What a worker that does not work sends, each of which must end the channel
/// and make no finding (docs/IMPLEMENTATION.md §4: garbage in must never become
/// a hardware verdict out).
struct Garbage {
  std::string name;
  std::function<void(Rig&)> send;
  std::string reason;
};

TEST(ControllerVerified, WhatIsNotARecordTheContractAllowsIsACorruptChannelNeverAFinding) {
  const std::vector<Garbage> garbage = {
      {"an error record that is not an encoding",
       [](Rig& rig) {
         rig.deliver(0, verification::kErrorRecordFrameType,
                     std::vector<std::byte>(40, std::byte{0x7}));
       },
       "an error record that does not decode"},
      {"an error record of a version nobody speaks",
       [](Rig& rig) {
         auto bytes = verification::encode(a_wrong_result());
         bytes[0] = std::byte{99};
         rig.deliver(0, verification::kErrorRecordFrameType, bytes);
       },
       "an error record that does not decode (unknown format version)"},
      {"a record the contract forbids",
       [](Rig& rig) {
         verification::ExactBitCorruption invalid = a_wrong_result();
         invalid.check.clear();
         rig.deliver_record(0, invalid);
       },
       "an error record that does not decode (invalid record)"},
      {"a record about another workload",
       [](Rig& rig) { rig.deliver_record(0, a_wrong_result("sort")); },
       "an error record about workload 'sort' from a worker that runs 'compression'"},
      {"a tally of the wrong size",
       [](Rig& rig) { rig.deliver(0, worker::kTallyFrameType, std::vector<std::byte>(3)); },
       "tally frame: 3 byte(s)"},
      {"a tally that goes backwards",
       [](Rig& rig) {
         rig.deliver_tally(0, a_tally(9));
         rig.deliver_tally(0, a_tally(2));
       },
       "a tally that went backwards"},
      {"a tally whose oracle blocks go backwards alone",
       [](Rig& rig) {
         rig.deliver_tally(0, a_tally(9));
         verification::Tally lower = a_tally(9);
         lower.oracle_blocks = 1;
         rig.deliver_tally(0, lower);
       },
       "a tally that went backwards"},
      {"a tally whose input checks go backwards alone",
       [](Rig& rig) {
         rig.deliver_tally(0, a_tally(9));
         verification::Tally lower = a_tally(9);
         lower.integrity_checked = 1;
         rig.deliver_tally(0, lower);
       },
       "a tally that went backwards"},
  };
  for (const Garbage& each : garbage) {
    Rig rig;
    rig.both_exit_after([&rig, &each] { each.send(rig); });
    auto result = rig.run();
    ASSERT_TRUE(result.has_value()) << each.name << ": " << describe(result.error());
    const WorkerReport& first = result.value().workers[0];
    EXPECT_EQ(first.fate, WorkerFate::kChannelCorrupt) << each.name;
    EXPECT_EQ(first.wrong_results, 0U) << each.name;
    EXPECT_TRUE(first.findings.empty()) << each.name;
    bool said = false;
    for (const std::string& note : first.records) {
      said = said || note.find("record channel: " + each.reason) != std::string::npos;
    }
    EXPECT_TRUE(said) << each.name;
    EXPECT_FALSE(all_clean(result.value())) << each.name;
  }
}

TEST(ControllerVerified, ARecordFromAWorkerThatRunsNothingIsNotBelieved) {
  Rig rig("first");
  rig.both_exit_after([&rig] { rig.deliver_record(0, a_wrong_result()); });
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kChannelCorrupt);
  EXPECT_EQ(result.value().workers[0].wrong_results, 0U);
}

TEST(ControllerVerified,
     AnErrorRecordWithNoWorkloadInItsProvenanceStillBelongsToTheWorkerThatSentIt) {
  Rig rig;
  verification::VerificationNotPerformed bare;
  bare.what = "the bracket";
  bare.why = "no unit was bracketed";
  rig.both_exit_after([&rig, &bare] { rig.deliver_record(0, bare); });
  auto result = rig.run();
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value().workers[0].not_performed, 1U);
  EXPECT_EQ(result.value().workers[0].fate, WorkerFate::kCompleted);
}

/// A frame with one byte of the whole frame (header, checksum or payload) changed.
std::string with_one_byte_changed(std::string frame, std::uint64_t pick) {
  const auto at = static_cast<std::size_t>(pick % frame.size());
  frame[at] = static_cast<char>(static_cast<unsigned char>(frame[at]) ^ (1U << (pick / 7 % 8)));
  return frame;
}

TEST(ControllerVerified, NoByteOfARecordFrameChangedInTransitMakesAFinding) {
  // The IPC fuzz of docs/IMPLEMENTATION.md §4 extended to the record types: a
  // whole frame with exactly one bit flipped is refused by the frame's checksum,
  // wherever the bit is, and nothing the worker sent after it is read.
  const std::string good =
      frame_of(verification::kErrorRecordFrameType, verification::encode(a_wrong_result()));
  for (std::uint64_t pick = 0; pick < 400; ++pick) {
    Rig rig;
    const std::string bad = with_one_byte_changed(good, pick);
    rig.both_exit_after([&rig, &bad] {
      rig.syscalls.deliver_to_pipe(kRecordRead0, bad);
      rig.deliver_tally(0, a_tally(1));
    });
    auto result = rig.run();
    ASSERT_TRUE(result.has_value()) << pick << ": " << describe(result.error());
    const WorkerReport& first = result.value().workers[0];
    EXPECT_TRUE(first.findings.empty()) << pick;
    EXPECT_EQ(first.wrong_results, 0U) << pick;
    EXPECT_FALSE(first.tally_seen) << pick << ": nothing after a bad frame is read";
    EXPECT_FALSE(is_clean(first.fate)) << pick;
  }
}

TEST(ControllerVerified, WhateverBytesFillAWholeFrameNoInvalidRecordIsEverKept) {
  // Well-framed garbage: valid checksums around random payloads, of every
  // record type the controller reads and of types it does not.
  for (std::uint64_t stream = 0; stream < 300; ++stream) {
    std::vector<std::byte> payload(stream % 130);
    rng::fill_stream(7, stream, payload);
    if (stream % 3 == 0 && payload.size() >= 2) {
      payload[0] = std::byte{verification::kRecordFormatVersion};
      payload[1] = static_cast<std::byte>(1 + stream % 3);
    }
    // Every fifth a tally, every fifth (the next) of a type nobody reads, the
    // rest error records.
    std::uint16_t type = verification::kErrorRecordFrameType;
    if (stream % 5 == 0) {
      type = worker::kTallyFrameType;
    } else if (stream % 5 == 1) {
      type = static_cast<std::uint16_t>(stream % 64);
    }
    Rig rig;
    rig.both_exit_after([&rig, &payload, type] { rig.deliver(0, type, payload); });
    auto result = rig.run();
    ASSERT_TRUE(result.has_value()) << stream << ": " << describe(result.error());
    for (const FindingReport& finding : result.value().workers[0].findings) {
      EXPECT_TRUE(verification::is_valid(finding.record)) << stream;
    }
  }
}

TEST(ControllerVerified, TheNewReportFieldsAreComparedByEveryMember) {
  const WorkerReport a;
  WorkerReport b;
  EXPECT_EQ(a, b);
  b.tally.units_computed = 1;
  EXPECT_NE(a, b);
  b = a;
  b.tally_seen = true;
  EXPECT_NE(a, b);
  b = a;
  b.work = WorkerWork{};
  EXPECT_NE(a, b);
  b = a;
  b.findings.emplace_back();
  EXPECT_NE(a, b);
  b = a;
  b.wrong_results = 1;
  EXPECT_NE(a, b);
  b = a;
  b.not_performed = 1;
  EXPECT_NE(a, b);
  b = a;
  b.findings_dropped = 1;
  EXPECT_NE(a, b);

  const WorkerWork work;
  EXPECT_TRUE(work == WorkerWork{}) << "a run described twice the same way is the same run";
  const auto differs = [&](auto change) {
    WorkerWork other = work;
    change(other);
    return !(work == other);
  };
  EXPECT_TRUE(differs([](WorkerWork& w) { w.descriptor.name = "x"; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.isa = simd::IsaPath::kNeon; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.decomposition = 1; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.duty.oracle_period = 9; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.logical_bytes = 1; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.threads = 1; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.cpu = 1; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.seed = 1; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.footprint_bytes = 1; }));
  EXPECT_TRUE(differs([](WorkerWork& w) { w.line_bytes = 1; }));

  const FindingReport f;
  FindingReport g;
  EXPECT_EQ(f, g);
  g.not_isolated = "x";
  EXPECT_NE(f, g);
  g = f;
  g.isolation = verification::IsolationReport{};
  EXPECT_NE(f, g);
  g = f;
  g.record = a_wrong_result();
  EXPECT_NE(f, g);

  const RunPlan p;
  RunPlan q;
  q.workloads.emplace_back("x");
  EXPECT_NE(p, q);
  q = p;
  q.environment = "x";
  EXPECT_NE(p, q);
  q = p;
  q.capabilities = "x";
  EXPECT_NE(p, q);

  const RunOptions o;
  RunOptions r;
  EXPECT_EQ(o, r);
  r.verification.seed = 3;
  EXPECT_NE(o, r);
  r = o;
  controller_testing::ModelFp<true, false> fp;
  r.fp = &fp;
  EXPECT_NE(o, r);
  r = o;
  controller_testing::ModelSource source;
  r.sources = {&source};
  EXPECT_NE(o, r);
}

}  // namespace
}  // namespace loadforge::controller
