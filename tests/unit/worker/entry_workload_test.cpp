// SPDX-License-Identifier: GPL-3.0-or-later
//
// The worker entry point with a real workload to run, and the re-run entry
// point: the arguments that describe a unit of work (parsed and refused by
// name, and built back into an argv that parses to the same thing), the
// worker's exit for each way a verified run ends, and each exit of a re-run.

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/fake_syscalls.hpp"
#include "unit/controller/consumer_models.hpp"
#include "unit/worker/model_runner.hpp"
#include "verification/oracle/registry/registry.hpp"
#include "verification/record.hpp"
#include "worker/entry.hpp"
#include "worker/frames.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"
#include "worker/services.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::worker {
namespace {

using loadforge::ipc::HeartbeatRegion;
using loadforge::platform::PipeEnds;
using loadforge::testing::FakeSyscalls;

constexpr std::int64_t kMs = 1'000'000;

std::vector<std::string_view> views_of(const std::vector<std::string>& strings) {
  std::vector<std::string_view> views;
  for (const std::string& s : strings) {
    views.emplace_back(s);
  }
  return views;
}

WorkloadArgs a_unit() {
  WorkloadArgs unit;
  unit.workload = "compression";
  unit.seed = 0xFEEDFACECAFEBEEFULL;
  unit.footprint_bytes = std::uint64_t{1} << 20U;
  unit.threads = 3;
  unit.decomposition = 2;
  unit.isa = simd::IsaPath::kAvx2;
  unit.duty = verification::DutyCycle{4, 9};
  unit.line_bytes = 128;
  unit.physical_core = 5;
  unit.numa_node = 1;
  return unit;
}

WorkerArgs a_worker(const WorkloadArgs& unit) {
  WorkerArgs args;
  args.slot = 1;
  args.workers = 3;
  args.heartbeat_fd = 100;
  args.record_fd = 201;
  args.control_fd = 202;
  args.supervisor_pid = 4000;
  args.pacing = Pacing{5000 * kMs, 50 * kMs, 500 * kMs};
  args.cpus = {2};
  args.workload = unit;
  return args;
}

std::vector<std::string> tail_of(const WorkerArgs& args) {
  std::vector<std::string> argv = argv_for("/usr/bin/loadforge", args);
  argv.erase(argv.begin(), argv.begin() + 2);
  return argv;
}

std::string refusal_of(const std::vector<std::string>& argv) {
  auto parsed = parse_args(views_of(argv));
  EXPECT_FALSE(parsed.has_value());
  return parsed.has_value() ? std::string{"(accepted)"} : parsed.error();
}

std::vector<std::string> with(std::vector<std::string> argv, std::string_view key,
                              std::string value) {
  for (std::size_t index = 0; index + 1 < argv.size(); index += 2) {
    if (argv[index] == key) {
      argv[index + 1] = std::move(value);
      return argv;
    }
  }
  argv.emplace_back(key);
  argv.push_back(std::move(value));
  return argv;
}

std::vector<std::string> without(std::vector<std::string> argv, std::string_view key) {
  for (std::size_t index = 0; index + 1 < argv.size(); index += 2) {
    if (argv[index] == key) {
      argv.erase(argv.begin() + static_cast<std::ptrdiff_t>(index),
                 argv.begin() + static_cast<std::ptrdiff_t>(index) + 2);
      break;
    }
  }
  return argv;
}

// --- a worker's unit of work -------------------------------------------------------

TEST(WorkloadArgsTest, EveryFieldSurvivesTheRoundTripThroughTheArgv) {
  const WorkerArgs args = a_worker(a_unit());
  auto parsed = parse_args(views_of(tail_of(args)));
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value(), args);
  ASSERT_TRUE(parsed.value().workload.has_value());
  EXPECT_EQ(*parsed.value().workload, a_unit());

  WorkerArgs bare = args;
  bare.workload->physical_core.reset();
  bare.workload->numa_node.reset();
  auto without_place = parse_args(views_of(tail_of(bare)));
  ASSERT_TRUE(without_place.has_value()) << without_place.error();
  EXPECT_EQ(without_place.value(), bare);
  const std::vector<std::string> bare_argv = tail_of(bare);
  EXPECT_EQ(std::count(bare_argv.begin(), bare_argv.end(), "--physical-core"), 0);
}

TEST(WorkloadArgsTest, ANullWorkerCarriesNoUnit) {
  WorkerArgs args = a_worker(a_unit());
  args.workload.reset();
  args.cpus.clear();
  auto parsed = parse_args(views_of(tail_of(args)));
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_FALSE(parsed.value().workload.has_value());
}

TEST(WorkloadArgsTest, EveryKeyOfTheUnitIsRequiredWithTheWorkloadAndRefusedWithout) {
  const std::vector<std::string> argv = tail_of(a_worker(a_unit()));
  for (const std::string_view key :
       {"--seed", "--footprint-bytes", "--threads", "--decomposition", "--isa", "--oracle-period",
        "--integrity-period", "--line-bytes"}) {
    EXPECT_EQ(refusal_of(without(argv, key)), std::string{key} + ": required with --workload");
  }
  std::vector<std::string> no_workload = without(argv, "--workload");
  EXPECT_EQ(refusal_of(no_workload), "--seed: needs --workload");
  for (const std::string_view key : {"--seed", "--footprint-bytes", "--threads", "--decomposition",
                                     "--isa", "--oracle-period", "--integrity-period",
                                     "--line-bytes"}) {
    no_workload = without(no_workload, key);
  }
  EXPECT_EQ(refusal_of(no_workload), "--physical-core: needs --workload");
  no_workload = without(no_workload, "--physical-core");
  EXPECT_EQ(refusal_of(no_workload), "--numa-node: needs --workload");
}

TEST(WorkloadArgsTest, EveryValueHasADomainAndItsBoundaryIsRefusedByName) {
  const std::vector<std::string> argv = tail_of(a_worker(a_unit()));
  EXPECT_EQ(refusal_of(with(argv, "--workload", "Compression")),
            "--workload: 'Compression' is not a workload name");
  EXPECT_NE(refusal_of(with(argv, "--seed", "-1")).find("--seed: expected a whole non-negative"),
            std::string::npos);
  EXPECT_EQ(refusal_of(with(argv, "--footprint-bytes", "0")), "--footprint-bytes: must be at least 1");
  EXPECT_NE(refusal_of(with(argv, "--footprint-bytes", "x")).find("--footprint-bytes: expected"),
            std::string::npos);
  EXPECT_EQ(refusal_of(with(argv, "--threads", "0")), "--threads: must be at least 1");
  EXPECT_NE(refusal_of(with(argv, "--threads", "1025")).find("--threads: 1025 is past the limit"),
            std::string::npos);
  EXPECT_NE(refusal_of(with(argv, "--threads", "z")).find("--threads: expected"),
            std::string::npos);
  EXPECT_NE(refusal_of(with(argv, "--decomposition", "4294967296"))
                .find("--decomposition: 4294967296 is past the limit"),
            std::string::npos);
  EXPECT_NE(refusal_of(with(argv, "--decomposition", "q")).find("--decomposition: expected"),
            std::string::npos);
  EXPECT_EQ(refusal_of(with(argv, "--isa", "sse")),
            "--isa: 'sse' is none of scalar, avx2, avx512, neon");
  for (const char* line : {"4", "0", "100", "8192"}) {
    EXPECT_NE(refusal_of(with(argv, "--line-bytes", line)).find("--line-bytes: "), std::string::npos)
        << line;
  }
  EXPECT_EQ(refusal_of(with(argv, "--line-bytes", "100")),
            "--line-bytes: must be a power of two of at least 8");
  EXPECT_NE(refusal_of(with(argv, "--line-bytes", "x")).find("--line-bytes: expected"),
            std::string::npos);
  EXPECT_NE(refusal_of(with(argv, "--oracle-period", "x")).find("--oracle-period: expected"),
            std::string::npos);
  EXPECT_NE(refusal_of(with(argv, "--integrity-period", "4294967296"))
                .find("--integrity-period: 4294967296 is past the limit"),
            std::string::npos);
  EXPECT_NE(refusal_of(with(argv, "--physical-core", "x")).find("--physical-core: expected"),
            std::string::npos);
  EXPECT_NE(refusal_of(with(argv, "--numa-node", "4294967296")).find("--numa-node: 4294967296"),
            std::string::npos);
  // The boundaries that are accepted.
  for (const char* line : {"8", "4096"}) {
    EXPECT_TRUE(parse_args(views_of(with(argv, "--line-bytes", line))).has_value()) << line;
  }
  EXPECT_TRUE(parse_args(views_of(with(argv, "--threads", "1024"))).has_value());
  EXPECT_TRUE(parse_args(views_of(with(argv, "--oracle-period", "0"))).has_value())
      << "a period of 0 is the contract's none";
}

TEST(WorkloadArgsTest, EqualityLooksAtEveryField) {
  const WorkerArgs a = a_worker(a_unit());
  const auto differs = [&](auto change) {
    WorkerArgs b = a;
    change(*b.workload);
    return !(a == b);
  };
  EXPECT_FALSE(a != a);
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.workload = "sort"; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.seed = 1; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.footprint_bytes = 1; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.threads = 1; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.decomposition = 1; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.isa = simd::IsaPath::kScalar; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.duty.oracle_period = 1; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.line_bytes = 64; }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.physical_core.reset(); }));
  EXPECT_TRUE(differs([](WorkloadArgs& u) { u.numa_node = 9; }));
  WorkerArgs none = a;
  none.workload.reset();
  EXPECT_NE(a, none);
}

// --- a re-run's arguments ------------------------------------------------------------

RerunArgs a_rerun() {
  RerunArgs args;
  args.unit = a_unit();
  args.unit.duty = verification::DutyCycle{1, 1};
  args.unit.physical_core.reset();
  args.unit.numa_node.reset();
  args.workload_version = 3;
  args.iteration = 123456789012;
  args.cpu = 6;
  return args;
}

std::vector<std::string> rerun_tail(const RerunArgs& args) {
  std::vector<std::string> argv = rerun_argv_for("/usr/bin/loadforge", args);
  EXPECT_EQ(argv[1], kRerunSwitch);
  argv.erase(argv.begin(), argv.begin() + 2);
  return argv;
}

std::string rerun_refusal(const std::vector<std::string>& argv) {
  auto parsed = parse_rerun_args(views_of(argv));
  EXPECT_FALSE(parsed.has_value());
  return parsed.has_value() ? std::string{"(accepted)"} : parsed.error();
}

TEST(RerunArgsTest, TheArgvForAReRunParsesBackToTheSameArguments) {
  const RerunArgs args = a_rerun();
  auto parsed = parse_rerun_args(views_of(rerun_tail(args)));
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed.value(), args);
}

TEST(RerunArgsTest, EveryKeyIsRequiredAndAWorkersOwnKeysAreNotAccepted) {
  const std::vector<std::string> argv = rerun_tail(a_rerun());
  for (const std::string_view key :
       {"--workload", "--seed", "--footprint-bytes", "--threads", "--decomposition", "--isa",
        "--line-bytes", "--version", "--iteration", "--cpu"}) {
    EXPECT_EQ(rerun_refusal(without(argv, key)), std::string{key} + ": required");
  }
  EXPECT_EQ(rerun_refusal(with(argv, "--oracle-period", "1")),
            "--oracle-period: not a worker argument");
  EXPECT_EQ(rerun_refusal(with(argv, "--slot", "1")), "--slot: not a worker argument");
}

TEST(RerunArgsTest, EveryValueIsRefusedByName) {
  const std::vector<std::string> argv = rerun_tail(a_rerun());
  EXPECT_NE(rerun_refusal(with(argv, "--workload", "A")).find("--workload: "), std::string::npos);
  EXPECT_NE(rerun_refusal(with(argv, "--version", "x")).find("--version: expected"),
            std::string::npos);
  EXPECT_NE(rerun_refusal(with(argv, "--iteration", "-3")).find("--iteration: expected"),
            std::string::npos);
  EXPECT_NE(rerun_refusal(with(argv, "--cpu", "4294967296")).find("--cpu: 4294967296"),
            std::string::npos);
  EXPECT_TRUE(parse_rerun_args(views_of(with(argv, "--iteration", "18446744073709551615")))
                  .has_value())
      << "any unit index is a unit";
}

TEST(RerunArgsTest, EqualityLooksAtEveryField) {
  const RerunArgs a = a_rerun();
  RerunArgs b = a;
  EXPECT_EQ(a, b);
  b.unit.seed = 1;
  EXPECT_NE(a, b);
  b = a;
  b.workload_version = 9;
  EXPECT_NE(a, b);
  b = a;
  b.iteration = 9;
  EXPECT_NE(a, b);
  b = a;
  b.cpu = 9;
  EXPECT_NE(a, b);
}

// --- running a unit of work -----------------------------------------------------------

constexpr std::uint16_t kStarted = static_cast<std::uint16_t>(ipc::RecordType::kStarted);
constexpr std::uint16_t kFinished = static_cast<std::uint16_t>(ipc::RecordType::kFinished);
constexpr std::uint16_t kError = static_cast<std::uint16_t>(ipc::RecordType::kError);

/// The real workloads, except that every unit's output is corrupted and the
/// oracle can be made to answer NotChecked: the ways a re-run goes wrong.
class DecoratedWorkload final : public workloads::Workload {
 public:
  DecoratedWorkload(std::unique_ptr<workloads::Workload> inner, bool corrupt, std::uint32_t version)
      : inner_(std::move(inner)), corrupt_(corrupt), descriptor_(inner_->descriptor()) {
    descriptor_.version = version;
  }
  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return descriptor_;
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return inner_->plan(); }
  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    inner_->generate(index, input);
  }
  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch, Executor& executor) const noexcept override {
    inner_->compute(index, input, output, scratch, executor);
    if (corrupt_) {
      output[40] ^= std::byte{0x10};
    }
  }

 private:
  std::unique_ptr<workloads::Workload> inner_;
  bool corrupt_;
  workloads::Descriptor descriptor_;
};

class DecoratedRegistry final : public workloads::Registry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override { return inner.names(); }
  [[nodiscard]] core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      std::string_view name, const workloads::WorkloadConfig& config) const override {
    auto made = inner.create(name, config);
    if (!made.has_value()) {
      return made.error();
    }
    std::unique_ptr<workloads::Workload> wrapped = std::make_unique<DecoratedWorkload>(
        std::move(made).value(), corrupt, version);
    return wrapped;
  }
  workloads::BuiltinRegistry inner;
  bool corrupt = false;
  std::uint32_t version = 1;
};

class UncheckingOracle final : public verification::Oracle {
 public:
  explicit UncheckingOracle(std::string name) : name_(std::move(name)) {}
  [[nodiscard]] std::string_view workload() const noexcept override { return name_; }
  [[nodiscard]] verification::Contract contract() const noexcept override {
    return verification::Contract::kExact;
  }
  [[nodiscard]] verification::Verdict check(const verification::UnitEvidence& /*e*/) const override {
    return verification::NotChecked{"scripted", "the script says so", 0};
  }

 private:
  std::string name_;
};

class UncheckingOracles final : public verification::OracleRegistry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override { return {"compression"}; }
  [[nodiscard]] core::Result<std::unique_ptr<verification::Oracle>, verification::OracleError>
  create(std::string_view name, const workloads::WorkloadConfig& /*config*/,
         const workloads::Plan& /*plan*/) const override {
    std::unique_ptr<verification::Oracle> oracle =
        std::make_unique<UncheckingOracle>(std::string{name});
    return oracle;
  }
};

/// A runner whose calls can fail, for the paths the real one has no reason to
/// take.
bool g_slice_fails = false;
bool g_finish_fails = false;
class FailingRunner final : public verification::SliceRunner {
 public:
  core::Result<verification::SliceReport, verification::RunnerError> run_slice(
      const verification::SliceRequest& /*r*/) override {
    if (g_slice_fails) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "scripted halt"};
    }
    return verification::SliceReport{};
  }
  core::Result<verification::SliceReport, verification::RunnerError> finish() override {
    if (g_finish_fails) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "scripted finish"};
    }
    return verification::SliceReport{};
  }
  [[nodiscard]] const verification::Tally& total() const noexcept override { return total_; }

 private:
  verification::Tally total_;
};
core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError>
make_failing(const verification::RunnerSetup& /*setup*/) {
  std::unique_ptr<verification::SliceRunner> runner = std::make_unique<FailingRunner>();
  return runner;
}

struct Doubles {
  DecoratedRegistry workloads;
  verification::BuiltinOracleRegistry oracles;
  UncheckingOracles unchecking;
  controller_testing::ModelFp<true, false> fp;
  PosixThreads threads;
  controller_testing::ModelProbe probe{simd::IsaSet{simd::IsaPath::kScalar}};

  [[nodiscard]] Services services(RunnerFactory factory = &worker_testing::ModelRunner::make,
                                  bool unchecked = false) const {
    Services services;
    services.workloads = &workloads;
    services.oracles = unchecked ? static_cast<const verification::OracleRegistry*>(&unchecking)
                                 : &oracles;
    services.fp = const_cast<controller_testing::ModelFp<true, false>*>(&fp);
    services.threads = const_cast<PosixThreads*>(&threads);
    services.isa = &probe;
    services.make_runner = factory;
    return services;
  }
};

struct Rig {
  FakeSyscalls syscalls;
  std::optional<HeartbeatRegion> region;
  PipeEnds records;
  PipeEnds control;
  std::ostringstream err;
  Doubles doubles;

  Rig() {
    auto created = HeartbeatRegion::create(syscalls, 2);
    EXPECT_TRUE(created.has_value());
    region.emplace(std::move(created).value());
    EXPECT_TRUE(region->publish(ipc::kControllerSlot, ipc::Heartbeat{0, 1, 1, 1}).has_value());
    records = syscalls.create_pipe().value();
    control = syscalls.create_pipe().value();
    syscalls.advance_clocks_per_read(10 * kMs);
  }

  WorkerArgs args() {
    WorkloadArgs unit;
    unit.workload = "compression";
    unit.seed = 5;
    unit.footprint_bytes = std::uint64_t{1} << 20U;
    unit.threads = 1;
    WorkerArgs args;
    args.slot = 0;
    args.workers = 2;
    args.heartbeat_fd = region->fd();
    args.record_fd = records.write_fd;
    args.control_fd = control.read_fd;
    args.supervisor_pid = FakeSyscalls::kFakeSupervisorPid;
    args.pacing = Pacing{60 * kMs, 10 * kMs, 100'000 * kMs};
    args.workload = unit;
    return args;
  }

  int run(const WorkerArgs& args, const Services& services) {
    const std::vector<std::string> argv = tail_of(args);
    return worker::run(syscalls, views_of(argv), err, services);
  }

  [[nodiscard]] std::vector<ipc::Record> received() const {
    std::vector<ipc::Record> out;
    ipc::RecordReader reader;
    std::vector<std::byte> raw;
    for (const char c : syscalls.pipe_contents(records.write_fd)) {
      raw.push_back(static_cast<std::byte>(c));
    }
    reader.feed(raw);
    while (true) {
      auto next = reader.next();
      EXPECT_TRUE(next.has_value());
      if (!next.has_value() || !next.value().has_value()) {
        break;
      }
      out.push_back(*next.value());
    }
    return out;
  }

  [[nodiscard]] static std::string text_of(const ipc::Record& record) {
    std::string text;
    for (const std::byte b : record.payload) {
      text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
    }
    return text;
  }
};

TEST(VerifiedWorkerRun, ARealUnitOfWorkRunsToTheEndAndSaysWhatItChecked) {
  Rig rig;
  EXPECT_EQ(rig.run(rig.args(), rig.doubles.services()), static_cast<int>(WorkerExit::kCompleted));
  EXPECT_EQ(rig.err.str(), "");
  const std::vector<ipc::Record> frames = rig.received();
  ASSERT_GE(frames.size(), 3U);
  EXPECT_EQ(frames.front().type, kStarted);
  EXPECT_EQ(frames.back().type, kFinished);
  const std::string summary = Rig::text_of(frames.back());
  EXPECT_NE(summary.find("the duration elapsed after "), std::string::npos) << summary;
  EXPECT_NE(summary.find(" unit(s), "), std::string::npos) << summary;
  EXPECT_NE(summary.find("; oracle checked "), std::string::npos) << summary;
  EXPECT_NE(summary.find(" sampled block(s)"), std::string::npos) << summary;
  EXPECT_EQ(summary.find("correct"), std::string::npos) << "a sample is never called correct";
  bool tally = false;
  for (const ipc::Record& frame : frames) {
    tally = tally || frame.type == kTallyFrameType;
  }
  EXPECT_TRUE(tally);
}

TEST(VerifiedWorkerRun, AnUnrunnableUnitIsASetupFailureThatNamesTheStageAndIsSentOnThePipe) {
  Rig rig;
  WorkerArgs args = rig.args();
  args.workload->workload = "no_such";
  EXPECT_EQ(rig.run(args, rig.doubles.services()), static_cast<int>(WorkerExit::kSetupFailed));
  EXPECT_NE(rig.err.str().find("loadforge --worker: workload: unknown workload"),
            std::string::npos)
      << rig.err.str();
  const std::vector<ipc::Record> frames = rig.received();
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frames[1].type, kError);
}

TEST(VerifiedWorkerRun, ARunnerThatStoppedItselfIsCompletedWithTheReasonInTheSummary) {
  Rig rig;
  g_slice_fails = true;
  EXPECT_EQ(rig.run(rig.args(), rig.doubles.services(&make_failing)),
            static_cast<int>(WorkerExit::kCompleted));
  g_slice_fails = false;
  const std::string summary = Rig::text_of(rig.received().back());
  EXPECT_NE(summary.find("the verification runner stopped the run"), std::string::npos) << summary;
  EXPECT_NE(summary.find("; runner: scripted halt"), std::string::npos) << summary;
}

TEST(VerifiedWorkerRun, AControllerThatIsGoneIsLostNotCompleted) {
  Rig rig;
  ASSERT_TRUE(rig.syscalls.close(rig.control.write_fd).has_value());
  EXPECT_EQ(rig.run(rig.args(), rig.doubles.services()),
            static_cast<int>(WorkerExit::kControllerLost));
  EXPECT_NE(rig.err.str().find("controller lost: the controller's control pipe hung up after 0 "
                               "unit(s)"),
            std::string::npos)
      << rig.err.str();
}

TEST(VerifiedWorkerRun, AFailureInsideTheLoopIsASetupFailure) {
  Rig rig;
  rig.syscalls.fail_poll(EBADF);
  EXPECT_EQ(rig.run(rig.args(), rig.doubles.services()),
            static_cast<int>(WorkerExit::kSetupFailed));
  EXPECT_NE(rig.err.str().find("control pipe"), std::string::npos) << rig.err.str();
}

TEST(VerifiedWorkerRun, TheProductionServicesRunTheSameWorker) {
  Rig rig;
  const std::vector<std::string> argv = tail_of(rig.args());
  EXPECT_EQ(worker::run(rig.syscalls, views_of(argv), rig.err),
            static_cast<int>(WorkerExit::kCompleted))
      << rig.err.str();
}

// --- the re-run ---------------------------------------------------------------------------

struct ReRig {
  FakeSyscalls syscalls;
  std::ostringstream err;
  Doubles doubles;

  RerunArgs args() {
    RerunArgs args;
    args.unit.workload = "compression";
    args.unit.seed = 5;
    args.unit.footprint_bytes = std::uint64_t{1} << 20U;
    args.unit.threads = 1;
    args.workload_version = 1;
    args.iteration = 10;
    args.cpu = 0;
    return args;
  }

  int run(const RerunArgs& args, const Services& services) {
    const std::vector<std::string> argv = rerun_tail(args);
    return run_rerun(syscalls, views_of(argv), err, services);
  }
};

TEST(ReRunEntry, ACleanUnitIsClean) {
  ReRig rig;
  EXPECT_EQ(rig.run(rig.args(), rig.doubles.services()), static_cast<int>(RerunExit::kClean));
  EXPECT_EQ(rig.err.str(), "");
  EXPECT_EQ(rig.syscalls.current_affinity().cpus.count(), 1U) << "placed where it was asked";
}

TEST(ReRunEntry, AUnitThatComesOutWrongAgainIsReproducedAndTheRecordIsPrinted) {
  ReRig rig;
  rig.doubles.workloads.corrupt = true;
  EXPECT_EQ(rig.run(rig.args(), rig.doubles.services()), static_cast<int>(RerunExit::kReproduced));
  EXPECT_NE(rig.err.str().find("ExactBitCorruption"), std::string::npos) << rig.err.str();
}

TEST(ReRunEntry, ACheckThatCouldNotBeMadeIsNotAReproductionAndNotACleanResult) {
  ReRig rig;
  EXPECT_EQ(rig.run(rig.args(), rig.doubles.services(&worker_testing::ModelRunner::make, true)),
            static_cast<int>(RerunExit::kNotPerformed));
  EXPECT_NE(rig.err.str().find("VerificationNotPerformed"), std::string::npos) << rig.err.str();
}

TEST(ReRunEntry, EveryWayTheReRunCannotBeMadeIsNotPerformedAndSaysWhy) {
  {
    ReRig rig;
    const std::vector<std::string_view> bad{"--cpu", "0"};
    EXPECT_EQ(run_rerun(rig.syscalls, bad, rig.err, rig.doubles.services()),
              static_cast<int>(RerunExit::kBadArguments));
    EXPECT_EQ(rig.err.str(), "loadforge --rerun: --workload: required\n");
  }
  {
    ReRig rig;
    RerunArgs args = rig.args();
    args.cpu = 4096;
    EXPECT_EQ(rig.run(args, rig.doubles.services()), static_cast<int>(RerunExit::kNotPerformed));
    EXPECT_NE(rig.err.str().find("loadforge --rerun: cpu: "), std::string::npos) << rig.err.str();
  }
  {
    ReRig rig;
    platform::CpuMask only_one;
    only_one.cpus.set(1);
    rig.syscalls.permit_cpus(only_one);
    EXPECT_EQ(rig.run(rig.args(), rig.doubles.services()),
              static_cast<int>(RerunExit::kNotPerformed));
    EXPECT_NE(rig.err.str().find("loadforge --rerun: cpu: "), std::string::npos) << rig.err.str();
  }
  {
    ReRig rig;
    RerunArgs args = rig.args();
    args.unit.workload = "no_such";
    EXPECT_EQ(rig.run(args, rig.doubles.services()), static_cast<int>(RerunExit::kNotPerformed));
    EXPECT_NE(rig.err.str().find("workload: unknown workload"), std::string::npos) << rig.err.str();
  }
  {
    ReRig rig;
    rig.doubles.workloads.version = 2;  // This build's bits are not the finding's.
    EXPECT_EQ(rig.run(rig.args(), rig.doubles.services()),
              static_cast<int>(RerunExit::kNotPerformed));
    EXPECT_NE(rig.err.str().find("the finding is of version 1 of compression, this build has version 2"),
              std::string::npos)
        << rig.err.str();
  }
  {
    ReRig rig;
    g_slice_fails = true;
    EXPECT_EQ(rig.run(rig.args(), rig.doubles.services(&make_failing)),
              static_cast<int>(RerunExit::kNotPerformed));
    g_slice_fails = false;
    EXPECT_NE(rig.err.str().find("scripted halt"), std::string::npos);
  }
  {
    ReRig rig;
    g_finish_fails = true;
    EXPECT_EQ(rig.run(rig.args(), rig.doubles.services(&make_failing)),
              static_cast<int>(RerunExit::kNotPerformed));
    g_finish_fails = false;
    EXPECT_NE(rig.err.str().find("scripted finish"), std::string::npos);
  }
}

}  // namespace
}  // namespace loadforge::worker
