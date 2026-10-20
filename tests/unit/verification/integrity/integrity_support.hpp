// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_INTEGRITY_INTEGRITY_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_INTEGRITY_INTEGRITY_SUPPORT_HPP

// The input-integrity stream's own doubles (ADR 0003): a workload, an oracle
// and an executor of each kind the runner consumes, written from their
// contracts and run against those contracts' suites in runner_test.cpp; the
// misbehaving wrappers the fault-injection tier drives them through; and a
// system-call decorator that fails the n-th call of a kind, which the shared
// fake cannot, so that every mapping the runner makes can be refused alone.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "platform/syscalls.hpp"
#include "primitives/rng/philox.hpp"
#include "support/fake_syscalls.hpp"
#include "verification/oracle.hpp"
#include "verification/runner.hpp"
#include "worker/executor.hpp"
#include "workloads/workload.hpp"

namespace loadforge::integrity_testing {

using Bytes = std::vector<std::byte>;

// ---------------------------------------------------------------------------
// Executors.
// ---------------------------------------------------------------------------

class SerialExecutor final : public worker::Executor {
 public:
  [[nodiscard]] std::size_t threads() const noexcept override { return 1; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    for (std::size_t index = 0; index < count; ++index) {
      task(context, index, 0);
    }
  }
};

/// A thread per slot for every run, joined before run returns: slot s takes
/// the indices s, s + n, ... in descending order, so neither the thread nor
/// the order matches a serial run.
class ThreadedExecutor final : public worker::Executor {
 public:
  explicit ThreadedExecutor(std::size_t threads) : threads_(threads) {}
  [[nodiscard]] std::size_t threads() const noexcept override { return threads_; }
  void run(std::size_t count, Task task, void* context) noexcept override {
    std::vector<std::thread> pool;
    pool.reserve(threads_);
    for (std::size_t slot = 0; slot < threads_; ++slot) {
      pool.emplace_back([this, slot, count, task, context] {
        for (std::size_t index = count; index-- > 0;) {
          if (index % threads_ == slot) {
            task(context, index, slot);
          }
        }
      });
    }
    for (std::thread& thread : pool) {
      thread.join();
    }
  }

 private:
  std::size_t threads_;
};

// ---------------------------------------------------------------------------
// Workloads.
// ---------------------------------------------------------------------------

/// rotor, version 3, exact: output[i] = rotl(input[i], 3) XOR key(index), with
/// key(index) the low byte of index * 0x9E3779B97F4A7C15. Input is Philox
/// stream `index` of the seed. 200 bytes in and out -- not a multiple of any
/// line, so every stride is rounded -- and 24 bytes of scratch per slot, which
/// each chunk stages its bytes through. The unit is cut into `decomposition`
/// chunks over the executor; bytes are independent, so the output is the
/// same at any thread count and decomposition: BitExact.
class RotorWorkload final : public workloads::Workload {
 public:
  static constexpr std::uint64_t kInputBytes = 200;
  static constexpr std::uint64_t kOutputBytes = 200;
  static constexpr std::uint64_t kScratchBytes = 24;

  static std::uint8_t key(std::uint64_t index) {
    return static_cast<std::uint8_t>((index * 0x9E3779B97F4A7C15ULL) >> 56U);
  }

  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    if (config.isa != simd::IsaPath::kScalar) {
      return workloads::WorkloadError{
          workloads::WorkloadFault::kConfiguration,
          "rotor has no kernel for " + std::string(simd::describe(config.isa))};
    }
    const std::uint32_t decomposition = config.decomposition == 0 ? 4 : config.decomposition;
    if (decomposition > 8) {
      return workloads::WorkloadError{workloads::WorkloadFault::kConfiguration,
                                      "rotor takes 1 to 8 chunks"};
    }
    const std::uint64_t units = config.footprint_bytes / (kInputBytes + kOutputBytes);
    if (units == 0) {
      return workloads::WorkloadError{
          workloads::WorkloadFault::kFootprintTooSmall,
          "no 400-byte unit in " + std::to_string(config.footprint_bytes) + " bytes"};
    }
    return std::unique_ptr<workloads::Workload>(new RotorWorkload(
        config.seed, workloads::Plan{kInputBytes, kOutputBytes, kScratchBytes, units, decomposition,
                                     kInputBytes + kOutputBytes, kInputBytes}));
  }

  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    rng::fill_stream(seed_, index, input);
  }

  // Defined once, in integrity_support.cpp (journal §1.22): an inline
  // definition in this header would be one COMDAT copy per test unit, and the
  // discarded copies of its executor lambda would read as never executed.
  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch, worker::Executor& executor) const noexcept override;

 private:
  RotorWorkload(std::uint64_t seed, const workloads::Plan& plan)
      : descriptor_{"rotor", 3, workloads::DeterminismClass::kBitExact,
                    verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_(plan),
        seed_(seed) {}

  workloads::Descriptor descriptor_;
  workloads::Plan plan_;
  std::uint64_t seed_;
};

/// mean, version 1, bounded: the mean of the 64 input bytes as a
/// little-endian double, summed in chunks over the executor. Every partial
/// sum is an integer far below 2^53, so the mean is exact in any order; it is
/// declared BoundedResidual so that the runner's bounded path has a workload.
class MeanWorkload final : public workloads::Workload {
 public:
  static constexpr std::uint64_t kInputBytes = 64;
  static constexpr std::uint64_t kOutputBytes = 8;

  static core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      const workloads::WorkloadConfig& config) {
    if (config.isa != simd::IsaPath::kScalar) {
      return workloads::WorkloadError{
          workloads::WorkloadFault::kConfiguration,
          "mean has no kernel for " + std::string(simd::describe(config.isa))};
    }
    const std::uint64_t units = config.footprint_bytes / (kInputBytes + kOutputBytes);
    if (units == 0) {
      return workloads::WorkloadError{workloads::WorkloadFault::kFootprintTooSmall,
                                      "no 72-byte unit"};
    }
    return std::unique_ptr<workloads::Workload>(
        new MeanWorkload(config.seed, workloads::Plan{kInputBytes, kOutputBytes, 0, units, 2,
                                                      kInputBytes + kOutputBytes, kInputBytes}));
  }

  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return plan_; }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    rng::fill_stream(seed_, index, input);
  }

  // Defined once, in integrity_support.cpp, for the reason RotorWorkload's is.
  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch, worker::Executor& executor) const noexcept override;

 private:
  MeanWorkload(std::uint64_t seed, const workloads::Plan& plan)
      : descriptor_{"mean", 1, workloads::DeterminismClass::kBoundedResidual,
                    verification::Contract::kBounded, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_(plan),
        seed_(seed) {}

  workloads::Descriptor descriptor_;
  workloads::Plan plan_;
  std::uint64_t seed_;
};

// ---------------------------------------------------------------------------
// Oracles, written from the two specifications above and not from the
// workloads' code.
// ---------------------------------------------------------------------------

/// rotor's specification, byte by byte.
class RotorOracle final : public verification::Oracle {
 public:
  [[nodiscard]] std::string_view workload() const noexcept override { return "rotor"; }
  [[nodiscard]] verification::Contract contract() const noexcept override {
    return verification::Contract::kExact;
  }
  [[nodiscard]] verification::Verdict check(
      const verification::UnitEvidence& evidence) const override {
    if (evidence.input.size() != RotorWorkload::kInputBytes ||
        evidence.output.size() != RotorWorkload::kOutputBytes) {
      return verification::NotChecked{"rotor unit", "evidence is not 200 bytes in and out", 0};
    }
    Bytes expected(evidence.output.size());
    for (std::size_t at = 0; at < expected.size(); ++at) {
      const auto value = std::to_integer<unsigned>(evidence.input[at]);
      const unsigned rotated = ((value << 3U) | (value >> 5U)) & 0xFFU;
      expected[at] = static_cast<std::byte>(rotated ^ RotorWorkload::key(evidence.index));
    }
    std::optional<std::size_t> first;
    std::size_t last = 0;
    std::uint64_t popcount = 0;
    for (std::size_t at = 0; at < expected.size(); ++at) {
      const auto delta = std::to_integer<unsigned>(expected[at] ^ evidence.output[at]);
      if (delta != 0) {
        first = first.value_or(at);
        last = at;
        popcount += static_cast<std::uint64_t>(std::popcount(delta));
      }
    }
    if (!first.has_value()) {
      return verification::Agreement{1};
    }
    verification::Difference difference;
    difference.domain = verification::Domain::kStored;
    difference.first_offset = *first;
    difference.last_offset = last;
    difference.popcount = popcount;
    difference.window_offset = *first / 8 * 8;
    const std::size_t end = std::min<std::size_t>(expected.size(), difference.window_offset + 64);
    for (std::size_t at = difference.window_offset; at < end; ++at) {
      difference.expected.push_back(expected[at]);
      difference.actual.push_back(evidence.output[at]);
    }
    return verification::ExactMismatch{"output against rotl(input, 3) XOR key", difference, 1};
  }
};

/// mean's specification: the output is the input bytes' mean, to within the
/// spacing of doubles at 256 times two -- the sum is exact, so anything
/// further is wrong.
class MeanOracle final : public verification::Oracle {
 public:
  static constexpr double kTolerance = 0x1p-44;

  [[nodiscard]] std::string_view workload() const noexcept override { return "mean"; }
  [[nodiscard]] verification::Contract contract() const noexcept override {
    return verification::Contract::kBounded;
  }
  [[nodiscard]] verification::Verdict check(
      const verification::UnitEvidence& evidence) const override {
    if (evidence.input.size() != MeanWorkload::kInputBytes ||
        evidence.output.size() != MeanWorkload::kOutputBytes) {
      return verification::NotChecked{"mean unit", "evidence is not 64 bytes in and 8 out", 0};
    }
    double sum = 0.0;
    for (const std::byte byte : evidence.input) {
      sum += std::to_integer<int>(byte);
    }
    std::uint64_t bits = 0;
    for (std::size_t byte = 0; byte < 8; ++byte) {
      bits |= std::to_integer<std::uint64_t>(evidence.output[byte]) << (8 * byte);
    }
    const double residual = std::abs(std::bit_cast<double>(bits) - sum / 64.0);
    if (residual <= kTolerance) {
      return verification::Agreement{1};
    }
    return verification::BoundedMismatch{"|output - mean|", residual, kTolerance,
                                         std::nullopt,      0,        1};
  }
};

// ---------------------------------------------------------------------------
// Misbehaving doubles for the fault-injection tier. These break the workload
// contract on purpose -- they change the memory the runner holds -- which is
// what a test double may do and a workload never may.
// ---------------------------------------------------------------------------

/// Forwards to a real workload and, at chosen moments, changes the memory of
/// the unit in flight: the slot's input after it was written, the slot's
/// input while compute() reads it, the output after compute() wrote it.
///
/// The slot is told from the regeneration buffer by order: the runner's first
/// generate() of a new index writes the slot (runner.hpp, step 1), and the
/// span it was given is remembered so that a later hook changes the very
/// bytes the runner will compare.
class TamperingWorkload final : public workloads::Workload {
 public:
  using Hook = std::function<void(std::uint64_t index, std::span<std::byte> bytes)>;

  explicit TamperingWorkload(const workloads::Workload& inner) : inner_(&inner) {}

  Hook after_write;     ///< The slot's input, just written.
  Hook during_compute;  ///< The slot's input, before compute() reads it.
  Hook after_compute;   ///< The slot's output, just computed.

  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return inner_->descriptor();
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return inner_->plan(); }

  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    inner_->generate(index, input);
    if (!last_index_.has_value() || *last_index_ != index) {
      last_index_ = index;
      slot_ = input;
      if (after_write) {
        after_write(index, input);
      }
    }
  }

  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch, worker::Executor& executor) const noexcept override {
    if (during_compute) {
      during_compute(index, slot_);
    }
    inner_->compute(index, input, output, scratch, executor);
    if (after_compute) {
      after_compute(index, output);
    }
  }

 private:
  const workloads::Workload* inner_;
  mutable std::optional<std::uint64_t> last_index_;
  mutable std::span<std::byte> slot_;
};

/// Forwards to a real oracle, except for the units it has been told to answer
/// some other way: a verdict of its choosing, or a throw.
class ScriptedOracle final : public verification::Oracle {
 public:
  using Answer = std::function<verification::Verdict(const verification::UnitEvidence&)>;

  explicit ScriptedOracle(const verification::Oracle& inner) : inner_(&inner) {}

  std::map<std::uint64_t, Answer> answers;
  mutable std::vector<std::uint64_t> checked;

  [[nodiscard]] std::string_view workload() const noexcept override { return inner_->workload(); }
  [[nodiscard]] verification::Contract contract() const noexcept override {
    return inner_->contract();
  }
  [[nodiscard]] verification::Verdict check(
      const verification::UnitEvidence& evidence) const override {
    checked.push_back(evidence.index);
    if (const auto found = answers.find(evidence.index); found != answers.end()) {
      return found->second(evidence);
    }
    return inner_->check(evidence);
  }

 private:
  const verification::Oracle* inner_;
};

/// An oracle with a name and contract of the test's choosing, for the
/// runner's refusals.
class NamedOracle final : public verification::Oracle {
 public:
  NamedOracle(std::string name, verification::Contract contract)
      : name_(std::move(name)), contract_(contract) {}
  [[nodiscard]] std::string_view workload() const noexcept override { return name_; }
  [[nodiscard]] verification::Contract contract() const noexcept override { return contract_; }
  [[nodiscard]] verification::Verdict check(
      const verification::UnitEvidence& /*evidence*/) const override {
    return verification::Agreement{1};
  }

 private:
  std::string name_;
  verification::Contract contract_;
};

/// A workload of any plan and name, which computes nothing in particular:
/// for the runner's refusals and its boundaries, never for its findings.
class ShapedWorkload final : public workloads::Workload {
 public:
  ShapedWorkload(std::string name, const workloads::Plan& plan)
      : descriptor_{std::move(name), 1, workloads::DeterminismClass::kBitExact,
                    verification::Contract::kExact, simd::IsaSet{simd::IsaPath::kScalar}},
        plan_(plan) {}
  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return plan_; }
  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    rng::fill_stream(1, index, input);
  }
  void compute(std::uint64_t /*index*/, std::span<const std::byte> input,
               std::span<std::byte> output, std::span<std::byte> /*scratch*/,
               worker::Executor& /*executor*/) const noexcept override {
    for (std::size_t at = 0; at < output.size(); ++at) {
      output[at] = input[at % input.size()];
    }
  }

 private:
  workloads::Descriptor descriptor_;
  workloads::Plan plan_;
};

// ---------------------------------------------------------------------------
// The system-call seam, failing the n-th call of one kind.
// ---------------------------------------------------------------------------

/// Forwards every call to a FakeSyscalls, except that the n-th call (from 1)
/// of a memory call can be made to fail with an errno, and every mapping the
/// fake hands out is recorded in order, so a test can reach the memory a
/// runner holds -- the working set first, then the control state's copies.
class SteppedSyscalls final : public platform::Syscalls {
 public:
  enum class Call : std::uint8_t { kMap, kBind, kPopulate, kLock };

  struct Mapped {
    std::byte* address = nullptr;
    std::size_t bytes = 0;
  };

  testing::FakeSyscalls fake;

  /// The n-th call of `call` fails with `error`; every other succeeds.
  void fail(Call call, int nth, int error) { failures_[call] = std::pair{nth, error}; }

  [[nodiscard]] const std::vector<Mapped>& mapped() const { return mapped_; }

  [[nodiscard]] std::span<std::byte> mapping(std::size_t which) const {
    return {mapped_.at(which).address, mapped_.at(which).bytes};
  }

  core::Result<void*, platform::SyscallError> map_anonymous(std::size_t length,
                                                            int extra_flags) override {
    if (auto failed = injected(Call::kMap, "mmap")) {
      return *failed;
    }
    auto result = fake.map_anonymous(length, extra_flags);
    if (result.has_value()) {
      mapped_.push_back(Mapped{static_cast<std::byte*>(result.value()), length});
    }
    return result;
  }
  core::Result<core::Ok, platform::SyscallError> bind_memory(
      void* address, std::size_t length, const platform::NodeMask& nodes) override {
    if (auto failed = injected(Call::kBind, "mbind")) {
      return *failed;
    }
    return fake.bind_memory(address, length, nodes);
  }
  core::Result<core::Ok, platform::SyscallError> advise_memory(void* address, std::size_t length,
                                                               int advice) override {
    if (auto failed = injected(Call::kPopulate, "madvise")) {
      return *failed;
    }
    return fake.advise_memory(address, length, advice);
  }
  core::Result<core::Ok, platform::SyscallError> lock_memory(void* address,
                                                             std::size_t length) override {
    if (auto failed = injected(Call::kLock, "mlock")) {
      return *failed;
    }
    return fake.lock_memory(address, length);
  }
  core::Result<core::Ok, platform::SyscallError> unmap(void* address, std::size_t length) override {
    return fake.unmap(address, length);
  }

  // Everything else is the fake's, unchanged.
  core::Result<int, platform::SyscallError> open_read(const std::string& path) override {
    return fake.open_read(path);
  }
  core::Result<std::size_t, platform::SyscallError> read(int fd, char* buffer,
                                                         std::size_t size) override {
    return fake.read(fd, buffer, size);
  }
  core::Result<core::Ok, platform::SyscallError> close(int fd) override { return fake.close(fd); }
  core::Result<std::size_t, platform::SyscallError> read_link(const std::string& path, char* buffer,
                                                              std::size_t size) override {
    return fake.read_link(path, buffer, size);
  }
  core::Result<pid_t, platform::SyscallError> fork_process() override {
    return fake.fork_process();
  }
  platform::SyscallError exec(const std::string& path,
                              const std::vector<std::string>& argv) override {
    return fake.exec(path, argv);
  }
  core::Result<core::Ok, platform::SyscallError> set_parent_death_signal(int signal) override {
    return fake.set_parent_death_signal(signal);
  }
  pid_t parent_pid() override { return fake.parent_pid(); }
  void start_session() override { fake.start_session(); }
  core::Result<platform::Reaped, platform::SyscallError> wait_any() override {
    return fake.wait_any();
  }
  core::Result<core::Ok, platform::SyscallError> send_signal(pid_t pid, int signal) override {
    return fake.send_signal(pid, signal);
  }
  core::Result<platform::TimeSpec, platform::SyscallError> read_clock(int clock_id) override {
    return fake.read_clock(clock_id);
  }
  core::Result<platform::CpuMask, platform::SyscallError> get_affinity(pid_t pid) override {
    return fake.get_affinity(pid);
  }
  core::Result<core::Ok, platform::SyscallError> set_affinity(
      pid_t pid, const platform::CpuMask& mask) override {
    return fake.set_affinity(pid, mask);
  }
  core::Result<int, platform::SyscallError> create_memory_file(const std::string& name,
                                                               std::size_t length) override {
    return fake.create_memory_file(name, length);
  }
  core::Result<void*, platform::SyscallError> map_shared(int fd, std::size_t length) override {
    return fake.map_shared(fd, length);
  }
  core::Result<core::Ok, platform::SyscallError> inherit_across_exec(int fd) override {
    return fake.inherit_across_exec(fd);
  }
  core::Result<platform::PipeEnds, platform::SyscallError> create_pipe() override {
    return fake.create_pipe();
  }
  core::Result<std::size_t, platform::SyscallError> write(
      int fd, std::span<const std::byte> bytes) override {
    return fake.write(fd, bytes);
  }
  core::Result<std::vector<platform::Readiness>, platform::SyscallError> poll_readable(
      const std::vector<int>& fds, int timeout_ms) override {
    return fake.poll_readable(fds, timeout_ms);
  }
  core::Result<core::Ok, platform::SyscallError> ignore_signal(int signal) override {
    return fake.ignore_signal(signal);
  }
  pid_t process_id() override { return fake.process_id(); }
  core::Result<std::optional<platform::Reaped>, platform::SyscallError> reap_any_nonblocking()
      override {
    return fake.reap_any_nonblocking();
  }

 private:
  std::optional<platform::SyscallError> injected(Call call, std::string_view name) {
    const int seen = ++calls_[call];
    const auto found = failures_.find(call);
    if (found != failures_.end() && found->second.first == seen) {
      return platform::SyscallError{found->second.second, name, "injected"};
    }
    return std::nullopt;
  }

  std::map<Call, std::pair<int, int>> failures_;
  std::map<Call, int> calls_;
  std::vector<Mapped> mapped_;
};

// ---------------------------------------------------------------------------
// A runner's parts, wired.
// ---------------------------------------------------------------------------

inline workloads::WorkloadConfig rotor_config(std::uint64_t seed = 7) {
  return workloads::WorkloadConfig{seed, std::uint64_t{8} * 400, 0, simd::IsaPath::kScalar};
}

inline std::unique_ptr<workloads::Workload> make_rotor(std::uint64_t seed = 7) {
  return std::move(RotorWorkload::create(rotor_config(seed))).value();
}

inline std::unique_ptr<workloads::Workload> make_mean(std::uint64_t seed = 5) {
  return std::move(MeanWorkload::create(workloads::WorkloadConfig{seed, std::uint64_t{4} * 72, 0,
                                                                  simd::IsaPath::kScalar}))
      .value();
}

/// A setup for `workload` checked by `oracle`, over `syscalls`.
inline verification::RunnerSetup setup_for(const workloads::Workload& workload,
                                           const verification::Oracle& oracle,
                                           worker::Executor& executor, platform::Syscalls& syscalls,
                                           verification::DutyCycle duty = {1, 1}) {
  verification::RunnerSetup setup;
  setup.workload = &workload;
  setup.oracle = &oracle;
  setup.executor = &executor;
  setup.syscalls = &syscalls;
  setup.duty = duty;
  setup.line_bytes = 64;
  setup.base.logical_cpu = 2;
  setup.base.physical_core = 1;
  setup.base.workload = workload.descriptor().name;
  setup.base.workload_version = workload.descriptor().version;
  setup.base.seed = 7;
  setup.base.isa = simd::IsaPath::kScalar;
  return setup;
}

inline std::unique_ptr<verification::SliceRunner> make_runner(
    const verification::RunnerSetup& setup) {
  auto made = verification::make_slice_runner(setup);
  EXPECT_TRUE(made.has_value()) << (made.has_value() ? "" : made.error().detail);
  return made.has_value() ? std::move(made).value() : nullptr;
}

inline verification::SliceReport slice(verification::SliceRunner& runner, std::uint64_t units) {
  auto report = runner.run_slice({units});
  EXPECT_TRUE(report.has_value());
  return report.has_value() ? std::move(report).value() : verification::SliceReport{};
}

}  // namespace loadforge::integrity_testing

#endif
