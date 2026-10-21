// SPDX-License-Identifier: GPL-3.0-or-later
//
// T5 and T5b across a rotation. The worker's loop runs for real (real runner,
// real executor, real registers, real pipes) in a thread, a workload is told to
// switch half-way through, and the faults are injected into the workloads it
// runs: an output corrupted on every fifth unit (T5), and oracles that decline
// to judge anything (T5b). What is asserted is which classification is produced,
// about which workload, from which unit -- the rotation changes what the worker
// runs and must change nothing about what it finds.

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "platform/fpenv/real_control.hpp"
#include "platform/isa/probe.hpp"
#include "platform/syscalls.hpp"
#include "verification/oracle.hpp"
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
#include "workloads/registry/registry.hpp"
#include "workloads/workload.hpp"

namespace loadforge::worker {
namespace {

using workloads::Workload;

/// A workload whose output is corrupted on every fifth unit, after it is computed.
class Corrupting final : public Workload {
 public:
  explicit Corrupting(std::unique_ptr<Workload> inner) : inner_(std::move(inner)) {}
  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return inner_->descriptor();
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return inner_->plan(); }
  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    inner_->generate(index, input);
  }
  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch, Executor& executor) const noexcept override {
    inner_->compute(index, input, output, scratch, executor);
    if (index % 5 == 0) {
      // A high bit of every fourth byte: an oracle that judges a sample finds
      // one, and one that tolerates a residual is not within its tolerance.
      for (std::size_t at = 0; at < output.size(); at += 4) {
        output[at] ^= std::byte{0x40};
      }
    }
  }

 private:
  std::unique_ptr<Workload> inner_;
};

class CorruptingRegistry final : public workloads::Registry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override { return inner_.names(); }
  [[nodiscard]] core::Result<std::unique_ptr<Workload>, workloads::WorkloadError> create(
      std::string_view name, const workloads::WorkloadConfig& config) const override {
    auto made = inner_.create(name, config);
    if (!made.has_value()) {
      return made.error();
    }
    std::unique_ptr<Workload> workload = std::make_unique<Corrupting>(std::move(made).value());
    return workload;
  }
  [[nodiscard]] core::Result<workloads::Descriptor, workloads::WorkloadError> descriptor(
      std::string_view name) const override {
    return inner_.descriptor(name);
  }

 private:
  workloads::BuiltinRegistry inner_;
};

/// An oracle that judges nothing.
class Unchecking final : public verification::Oracle {
 public:
  explicit Unchecking(std::unique_ptr<verification::Oracle> inner) : inner_(std::move(inner)) {}
  [[nodiscard]] std::string_view workload() const noexcept override { return inner_->workload(); }
  [[nodiscard]] verification::Contract contract() const noexcept override {
    return inner_->contract();
  }
  [[nodiscard]] verification::Verdict check(
      const verification::UnitEvidence& /*evidence*/) const override {
    return verification::NotChecked{"the unit", "this oracle declines to judge"};
  }

 private:
  std::unique_ptr<verification::Oracle> inner_;
};

class UncheckingRegistry final : public verification::OracleRegistry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override { return inner_.names(); }
  [[nodiscard]] core::Result<std::unique_ptr<verification::Oracle>, verification::OracleError>
  create(std::string_view name, const workloads::WorkloadConfig& config,
         const workloads::Plan& plan) const override {
    auto made = inner_.create(name, config, plan);
    if (!made.has_value()) {
      return made.error();
    }
    std::unique_ptr<verification::Oracle> oracle =
        std::make_unique<Unchecking>(std::move(made).value());
    return oracle;
  }

 private:
  verification::BuiltinOracleRegistry inner_;
};

/// A worker run in a thread, with the controller's part played by the test.
class RotatingWorker {
 public:
  RotatingWorker(const workloads::Registry& workloads,
                 const verification::OracleRegistry& oracles) {
    auto region = ipc::HeartbeatRegion::create(syscalls_, 1);
    EXPECT_TRUE(region.has_value());
    region_.emplace(std::move(region).value());
    records_ = syscalls_.create_pipe().value();
    control_ = syscalls_.create_pipe().value();
    // The controller's own beat, fresh for as long as the test lasts.
    platform::Clock clock(syscalls_);
    const std::int64_t far = clock.now().value().running_ns + std::int64_t{3600} * 1'000'000'000;
    EXPECT_TRUE(region_->publish(ipc::kControllerSlot, ipc::Heartbeat{far, 1, 1, 1}).has_value());
    services_.workloads = &workloads;
    services_.oracles = &oracles;
    services_.fp = &fp_;
    services_.threads = &threads_;
    services_.isa = probe_.get();
    services_.make_runner = &verification::make_slice_runner;
  }

  void start(std::string_view workload, std::int64_t duration_ns) {
    args_.slot = 0;
    args_.workers = 1;
    args_.pacing = Pacing{duration_ns, 20'000'000, 100'000'000'000};
    unit_.workload = std::string{workload};
    unit_.seed = 5;
    unit_.footprint_bytes = std::uint64_t{2} << 20U;
    unit_.threads = 1;
    unit_.lock = verification::LockPolicy::kNone;
    thread_ = std::thread([this] {
      ipc::RecordWriter writer(syscalls_, records_.write_fd);
      const Session session{&*region_, 0, control_.read_fd, args_.pacing, &writer};
      VerifiedRun run(syscalls_, writer, session, args_, unit_, services_);
      result_ = run.run();
    });
  }

  void send(const ControlMessage& message) {
    const std::vector<std::byte> payload = encode(message);
    const std::vector<std::byte> frame = ipc::encode(kControlFrameType, payload).value();
    EXPECT_TRUE(syscalls_.write(control_.write_fd, frame).has_value());
  }

  void finish() {
    thread_.join();
    EXPECT_TRUE(syscalls_.close(records_.write_fd).has_value());
  }

  /// Everything the worker wrote, decoded.
  struct Seen {
    std::vector<verification::ErrorRecord> findings;
    std::vector<ControlAck> answers;
    verification::Tally last_tally;
  };

  Seen seen() {
    Seen out;
    ipc::RecordSource source(syscalls_, records_.read_fd);
    while (!source.at_end()) {
      auto drained = source.drain();
      EXPECT_TRUE(drained.has_value());
      if (!drained.has_value()) {
        break;
      }
      for (const ipc::Record& record : drained.value().records) {
        if (record.type == verification::kErrorRecordFrameType) {
          out.findings.push_back(verification::decode(record.payload).value());
        } else if (record.type == kControlAckFrameType) {
          out.answers.push_back(decode_ack(record.payload).value());
        } else if (record.type == kTallyFrameType) {
          out.last_tally = decode_tally(record.payload).value();
        }
      }
    }
    return out;
  }

  [[nodiscard]] const core::Result<VerifiedOutcome, WorkerError>& result() const { return result_; }

 private:
  platform::RealSyscalls syscalls_;
  std::optional<ipc::HeartbeatRegion> region_;
  platform::PipeEnds records_;
  platform::PipeEnds control_;
  fpenv::RealFpControl fp_;
  PosixThreads threads_;
  std::unique_ptr<simd::IsaProbe> probe_ = isa::make_native_probe();
  Services services_;
  WorkerArgs args_;
  WorkloadArgs unit_;
  std::thread thread_;
  core::Result<VerifiedOutcome, WorkerError> result_{WorkerError{"not run", ""}};
};

std::string workload_of(const verification::ErrorRecord& record) {
  return std::visit([](const auto& each) { return each.provenance.workload.value_or("none"); },
                    record);
}

std::uint64_t iteration_of(const verification::ErrorRecord& record) {
  return std::visit([](const auto& each) { return each.provenance.iteration.value_or(0); }, record);
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

TEST(RotationFaults,
     ACorruptionAfterASwitchIsClassifiedAgainstTheWorkloadSwitchedToFromTheNextUnit) {
  const CorruptingRegistry workloads;
  const verification::BuiltinOracleRegistry oracles;
  RotatingWorker worker(workloads, oracles);
  worker.start("compression", 2'500'000'000);
  sleep_ms(600);
  worker.send({1, Switch{"fft", 9, std::uint64_t{2} << 20U, 0, {1, 1}}});
  sleep_ms(900);
  worker.send({2, Stop{}});
  worker.finish();
  ASSERT_TRUE(worker.result().has_value()) << describe(worker.result().error());
  EXPECT_EQ(worker.result().value().outcome.reason, StopReason::kStopRequested);
  const auto seen = worker.seen();
  ASSERT_EQ(seen.answers.size(), 2U);
  EXPECT_EQ(seen.answers[0], (ControlAck{1, AckStatus::kDone, ""}));

  std::uint64_t last_before = 0;
  std::uint64_t first_after = UINT64_MAX;
  std::size_t before = 0;
  std::size_t after = 0;
  for (const verification::ErrorRecord& record : seen.findings) {
    if (!verification::is_wrong_result(record)) {
      continue;
    }
    if (workload_of(record) == "compression") {
      ++before;
      last_before = std::max(last_before, iteration_of(record));
    } else {
      EXPECT_EQ(workload_of(record), "fft");
      ++after;
      first_after = std::min(first_after, iteration_of(record));
    }
  }
  EXPECT_GT(before, 0U) << "found before the switch";
  EXPECT_GT(after, 0U)
      << "and found after it, about the workload the worker was switched to; units "
      << seen.last_tally.units_computed << " findings " << seen.findings.size() << " answers "
      << seen.answers.size();
  EXPECT_GT(first_after, last_before)
      << "the units run on across the switch: none is numbered twice, none is skipped back";
  EXPECT_GE(seen.last_tally.units_computed, before) << "the tally ran on across it";
  EXPECT_GT(seen.last_tally.oracle_failed, 0U);
}

TEST(RotationFaults, ACheckThatCannotBeMadeAfterASwitchIsNotPerformedNeverClean) {
  const workloads::BuiltinRegistry workloads;
  const UncheckingRegistry oracles;
  RotatingWorker worker(workloads, oracles);
  worker.start("compression", 2'500'000'000);
  sleep_ms(600);
  worker.send({1, Switch{"fft", 9, std::uint64_t{2} << 20U, 0, {1, 1}}});
  sleep_ms(900);
  worker.send({2, Stop{}});
  worker.finish();
  ASSERT_TRUE(worker.result().has_value()) << describe(worker.result().error());
  const auto seen = worker.seen();
  std::size_t compression = 0;
  std::size_t fft = 0;
  std::size_t about_the_whole_run = 0;
  for (const verification::ErrorRecord& record : seen.findings) {
    EXPECT_FALSE(verification::is_wrong_result(record));
    const auto& unrun = std::get<verification::VerificationNotPerformed>(record);
    if (unrun.what.starts_with("oracle over ")) {
      ++about_the_whole_run;
      EXPECT_EQ(unrun.what, "oracle over fft units") << "stated once, by the run's last runner";
    } else if (workload_of(record) == "compression") {
      ++compression;
    } else if (workload_of(record) == "fft") {
      ++fft;
    }
  }
  EXPECT_GT(compression, 0U);
  EXPECT_GT(fft, 0U);
  EXPECT_EQ(about_the_whole_run, 1U);
  EXPECT_EQ(seen.last_tally.oracle_blocks, 0U);
  EXPECT_EQ(seen.last_tally.oracle_failed, 0U);
}

}  // namespace
}  // namespace loadforge::worker
