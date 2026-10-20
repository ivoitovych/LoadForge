// SPDX-License-Identifier: GPL-3.0-or-later
//
// The stack of stack.hpp over the real kernel: real system calls, the real
// /proc and /sys, the real monotonic clock.

#ifndef LOADFORGE_TESTS_INTEGRATION_M3_STACK_REAL_HPP
#define LOADFORGE_TESTS_INTEGRATION_M3_STACK_REAL_HPP

#include "platform/clock.hpp"
#include "platform/syscalls.hpp"
#include "stack.hpp"

namespace loadforge::m3 {

/// Stack's rounds, stamped by the machine's own clock instead of a counter.
/// The Stack of stack.hpp serves fixtures through the fake; this one owns a
/// real seam and a rig over it.
class RealStack {
 public:
  RealStack() : files_(syscalls_), clock_(syscalls_) {}

  void start(std::size_t capacity = 100'000) {
    auto made = cli::make_rig(syscalls_, files_, cli::MachineRoots{}, capacity);
    ASSERT_TRUE(made.has_value()) << made.error().detail;
    rig_ = std::move(made).value();
  }

  void round() {
    const auto now = clock_.now();
    ASSERT_TRUE(now.has_value());
    const auto answer = rig_->sampler().round(now.value().running_ns);
    ASSERT_TRUE(answer.has_value()) << answer.error().detail;
    if (rounds_ == 0) {
      first_ = now.value().running_ns;
    }
    last_ = now.value().running_ns;
    ++rounds_;
  }

  [[nodiscard]] const telemetry::CapabilityReport& capabilities() const {
    return rig_->capabilities();
  }
  [[nodiscard]] statistics::LoadSignature signature() const {
    const auto taken = summarizer_.summarize(rig_->timeline(), cli::run_window(sampling()));
    EXPECT_TRUE(taken.has_value());
    return taken.value();
  }
  [[nodiscard]] results::RunResult result() const {
    config::Config config;
    config.workloads.push_back(config::Workload{"null", config::Threads::all(), true, {}});
    controller::RunReport report;
    report.elapsed_ns = last_ - first_;
    report.sampling = sampling();
    cli::ResultInputs inputs;
    inputs.config = &config;
    inputs.report = &report;
    inputs.capabilities = rig_->capabilities();
    inputs.timeline = &rig_->timeline();
    inputs.summarizer = &summarizer_;
    auto assembled = cli::assemble_result(inputs);
    EXPECT_TRUE(assembled.has_value());
    return assembled.value();
  }

 private:
  [[nodiscard]] controller::SamplingReport sampling() const {
    controller::SamplingReport sampling;
    sampling.enabled = true;
    sampling.rounds = rounds_;
    sampling.first_round_ns = first_;
    sampling.last_round_ns = last_;
    return sampling;
  }

  platform::RealSyscalls syscalls_;
  platform::FileSystem files_;
  platform::Clock clock_;
  std::unique_ptr<cli::Rig> rig_;
  statistics::CompensatedSummarizer summarizer_;
  std::int64_t first_ = 0;
  std::int64_t last_ = 0;
  std::uint64_t rounds_ = 0;
};

}  // namespace loadforge::m3

#endif
