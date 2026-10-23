// SPDX-License-Identifier: GPL-3.0-or-later
//
// `loadforge selftest`: its arguments, its golden part against digests it is given (a match and
// each half's mismatch), its oracle part over the real registries and over a registry whose
// workload computes a wrong answer on purpose, the paths each workload is checked on, and every
// way the command refuses or fails. The services are the worker's own seam (worker/services.hpp):
// the real registries, oracles, registers and threads, with the one part a test wants to
// misbehave swapped for a double.

#include "main/selftest_command.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "main/outcome.hpp"
#include "main/selftest.hpp"
#include "platform/syscalls.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/runner.hpp"
#include "worker/services.hpp"
#include "workloads/registry/registry.hpp"
#include "workloads/workload.hpp"

namespace loadforge::cli {
namespace {

using simd::IsaPath;
using simd::IsaSet;

// --- arguments -------------------------------------------------------------------------

core::Result<SelftestOptions, std::string> parse(std::vector<std::string_view> args) {
  return parse_selftest_args(args);
}

TEST(SelftestArgs, NoneAreTheDefaults) {
  const auto parsed = parse({});
  ASSERT_TRUE(parsed.has_value());
  EXPECT_FALSE(parsed.value().isa.has_value());
  EXPECT_EQ(parsed.value().footprint_bytes, kSelftestFootprintBytes);
  EXPECT_EQ(parsed.value().seed, 1U);
  EXPECT_FALSE(parsed.value().verbose);
}

TEST(SelftestArgs, EveryOptionInAnyOrder) {
  const auto parsed = parse({"--verbose", "--seed", "77", "--footprint", "2MiB", "--isa", "scalar"});
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed.value().isa, IsaPath::kScalar);
  EXPECT_EQ(parsed.value().footprint_bytes, std::uint64_t{2} << 20U);
  EXPECT_EQ(parsed.value().seed, 77U);
  EXPECT_TRUE(parsed.value().verbose);
}

TEST(SelftestArgs, EachRefusalNamesTheArgument) {
  const std::vector<std::pair<std::vector<std::string_view>, std::string>> cases{
      {{"--frob"}, "unknown argument '--frob'"},
      {{"stress"}, "unknown argument 'stress'"},
      {{"--verbose", "--verbose"}, "--verbose given more than once"},
      {{"--seed", "1", "--seed", "2"}, "--seed given more than once"},
      {{"--isa"}, "--isa needs a value"},
      {{"--isa", "sse9"}, "--isa: 'sse9' is none of scalar, avx2, avx512, neon"},
      {{"--footprint", "lots"}, "--footprint: "},
      {{"--footprint", "50%"}, "--footprint must be a size, not a share of memory"},
      {{"--footprint", "512KiB"}, "--footprint must be at least 1MiB"},
      {{"--seed", "x"}, "--seed: expected a whole number, got 'x'"},
      {{"--seed", "5s"}, "--seed: expected a whole number, got '5s'"},
  };
  for (const auto& [args, expected] : cases) {
    const auto parsed = parse(args);
    ASSERT_FALSE(parsed.has_value()) << args.front();
    EXPECT_EQ(parsed.error().substr(0, expected.size()), expected) << parsed.error();
  }
}

// --- the paths a workload is checked on ----------------------------------------------------

constexpr IsaSet kWide{IsaPath::kScalar, IsaPath::kAvx2, IsaPath::kAvx512};

TEST(SelftestPaths, TheWidestThenScalar) {
  const SelftestPaths paths = selftest_paths(std::nullopt, kWide, kWide, kWide);
  ASSERT_EQ(paths.run.size(), 2U);
  EXPECT_EQ(paths.run[0], IsaPath::kAvx512);
  EXPECT_EQ(paths.run[1], IsaPath::kScalar);
  EXPECT_TRUE(paths.note.empty());
}

TEST(SelftestPaths, OnlyWhatTheWorkloadHas) {
  const SelftestPaths paths =
      selftest_paths(std::nullopt, kWide, kWide, IsaSet{IsaPath::kScalar, IsaPath::kAvx2});
  ASSERT_EQ(paths.run.size(), 2U);
  EXPECT_EQ(paths.run[0], IsaPath::kAvx2);
}

TEST(SelftestPaths, ScalarAloneIsCheckedOnce) {
  for (const auto& requested : {std::optional<IsaPath>{}, std::optional<IsaPath>{IsaPath::kScalar}}) {
    const SelftestPaths paths =
        selftest_paths(requested, IsaSet{IsaPath::kScalar}, kWide, kWide);
    ASSERT_EQ(paths.run.size(), 1U);
    EXPECT_EQ(paths.run[0], IsaPath::kScalar);
  }
}

TEST(SelftestPaths, TheRequestedPathAndScalar) {
  const SelftestPaths paths = selftest_paths(IsaPath::kAvx2, kWide, kWide, kWide);
  ASSERT_EQ(paths.run.size(), 2U);
  EXPECT_EQ(paths.run[0], IsaPath::kAvx2);
  EXPECT_EQ(paths.run[1], IsaPath::kScalar);
}

TEST(SelftestPaths, ARequestedPathTheWorkloadLacksIsCheckedOnScalarAndSaysSo) {
  const SelftestPaths paths =
      selftest_paths(IsaPath::kAvx2, kWide, kWide, IsaSet{IsaPath::kScalar});
  ASSERT_EQ(paths.run.size(), 1U);
  EXPECT_EQ(paths.run[0], IsaPath::kScalar);
  EXPECT_NE(paths.note.find("avx2"), std::string::npos) << paths.note;
}

// --- doubles --------------------------------------------------------------------------

/// A workload that computes what the real one does, wrong in one byte of every unit.
class Wrong final : public workloads::Workload {
 public:
  explicit Wrong(std::unique_ptr<workloads::Workload> inner) : inner_(std::move(inner)) {}
  [[nodiscard]] const workloads::Descriptor& descriptor() const noexcept override {
    return inner_->descriptor();
  }
  [[nodiscard]] const workloads::Plan& plan() const noexcept override { return inner_->plan(); }
  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    inner_->generate(index, input);
  }
  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch, worker::Executor& executor) const noexcept override {
    inner_->compute(index, input, output, scratch, executor);
    if (output.size() > 40) {
      output[40] ^= std::byte{0x10};
    }
  }

 private:
  std::unique_ptr<workloads::Workload> inner_;
};

/// The built-in registry narrowed to some names, with the faults a test wants: workloads that
/// compute wrongly, a descriptor that lists fewer paths, a name it cannot describe.
class TestRegistry final : public workloads::Registry {
 public:
  explicit TestRegistry(std::vector<std::string> names) : names_(std::move(names)) {}
  [[nodiscard]] std::vector<std::string> names() const override { return names_; }
  [[nodiscard]] core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      std::string_view name, const workloads::WorkloadConfig& config) const override {
    auto created = inner_.create(name, config);
    if (!created.has_value() || !wrong) {
      return created;
    }
    std::unique_ptr<workloads::Workload> workload = std::make_unique<Wrong>(std::move(created).value());
    return workload;
  }
  [[nodiscard]] core::Result<workloads::Descriptor, workloads::WorkloadError> descriptor(
      std::string_view name) const override {
    return inner_.descriptor(name);
  }

  bool wrong = false;

 private:
  std::vector<std::string> names_;
  workloads::BuiltinRegistry inner_;
};

/// A runner that refuses the call a test names.
class Refusing final : public verification::SliceRunner {
 public:
  [[nodiscard]] core::Result<verification::SliceReport, verification::RunnerError> run_slice(
      const verification::SliceRequest& /*request*/) override {
    if (fail_slice) {
      return verification::RunnerError{verification::RunnerFault::kFinished, "slice refused"};
    }
    return verification::SliceReport{};
  }
  [[nodiscard]] core::Result<verification::SliceReport, verification::RunnerError> finish()
      override {
    return verification::RunnerError{verification::RunnerFault::kFinished, "finish refused"};
  }
  [[nodiscard]] const verification::Tally& total() const noexcept override { return tally_; }
  [[nodiscard]] const verification::MemoryLock& memory_lock() const noexcept override {
    return lock_;
  }

  static inline bool fail_slice = false;

 private:
  verification::Tally tally_;
  verification::MemoryLock lock_;
};

core::Result<std::unique_ptr<verification::SliceRunner>, verification::RunnerError> make_refusing(
    const verification::RunnerSetup& /*setup*/) {
  std::unique_ptr<verification::SliceRunner> runner = std::make_unique<Refusing>();
  return runner;
}

class FixedProbe final : public simd::IsaProbe {
 public:
  explicit FixedProbe(IsaSet set) : set_(set) {}
  [[nodiscard]] IsaSet supported() const override { return set_; }

 private:
  IsaSet set_;
};

constexpr ExpectedDigests kRight{selftest::kExpectedCorpusDigest, selftest::kExpectedDigest};

struct Run {
  int code = 0;
  std::string out;
  std::string err;
};

Run run_over(const worker::Services& services, const SelftestOptions& options,
             const ExpectedDigests& expected) {
  platform::RealSyscalls syscalls;
  std::ostringstream out;
  std::ostringstream err;
  Run run;
  run.code = run_selftest(syscalls, services, options, expected, out, err);
  run.out = out.str();
  run.err = err.str();
  return run;
}

bool has(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

// --- the golden part -------------------------------------------------------------------------

TEST(SelftestExpected, TheCommittedDigestsAreWhatThisTreeComputes) {
  // The expected values are committed so that they do not move with the code that computes
  // them. When the corpus or a scalar kernel changes on purpose these fail: put the printed
  // values into main/selftest.hpp in the change that moved them.
  std::ostringstream corpus;
  corpus << std::hex << selftest::corpus_digest();
  std::ostringstream full;
  full << std::hex << selftest::core_digest();
  EXPECT_EQ(selftest::corpus_digest(), selftest::kExpectedCorpusDigest)
      << "kExpectedCorpusDigest is now 0x" << corpus.str();
  EXPECT_EQ(selftest::core_digest(), selftest::kExpectedDigest)
      << "kExpectedDigest is now 0x" << full.str();
  EXPECT_EQ(selftest::workloads_digest(selftest::kExpectedCorpusDigest),
            selftest::kExpectedDigest);
}

TEST(SelftestExpected, TheHexTheDigestSwitchPrintsIsTheExpectedDigest) {
  std::ostringstream hex;
  hex << std::hex << std::setfill('0') << std::setw(16) << selftest::kExpectedDigest;
  EXPECT_EQ(selftest::core_digest_hex(), hex.str());
}

TEST(Selftest, GoldenMatchPassesBothHalves) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  const TestRegistry registry({});
  services.workloads = &registry;
  const Run run = run_over(services, SelftestOptions{}, kRight);
  EXPECT_EQ(run.code, 0) << run.out;
  EXPECT_TRUE(has(run.out, "PASS  golden corpus: the configuration-parsing digest")) << run.out;
  EXPECT_TRUE(has(run.out, "PASS  golden kernels: the scalar workload-kernel digest")) << run.out;
  EXPECT_TRUE(has(run.out, "selftest passed: 2 of 2 checks")) << run.out;
  EXPECT_TRUE(run.err.empty());
}

TEST(Selftest, AKernelDigestThatDiffersIsNamedAndFails) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  const TestRegistry registry({});
  services.workloads = &registry;
  const ExpectedDigests wrong{selftest::kExpectedCorpusDigest, selftest::kExpectedDigest + 1};
  const Run run = run_over(services, SelftestOptions{}, wrong);
  EXPECT_EQ(run.code, kExitSelftestFailed) << run.out;
  EXPECT_TRUE(has(run.out, "PASS  golden corpus")) << run.out;
  EXPECT_TRUE(has(run.out, "FAIL  golden kernels: the scalar workload-kernel digest differ: "
                           "expected "))
      << run.out;
  EXPECT_TRUE(has(run.out, "selftest FAILED: 1 of 2 checks failed")) << run.out;
  EXPECT_TRUE(has(run.out, "not trustworthy for a hardware verdict")) << run.out;
}

TEST(Selftest, ACorpusDigestThatDiffersFailsOnlyTheCorpus) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  const TestRegistry registry({});
  services.workloads = &registry;
  const std::uint64_t basis = selftest::kExpectedCorpusDigest + 1;
  const ExpectedDigests wrong{basis, selftest::workloads_digest(basis)};
  const Run run = run_over(services, SelftestOptions{}, wrong);
  EXPECT_EQ(run.code, kExitSelftestFailed) << run.out;
  EXPECT_TRUE(has(run.out, "FAIL  golden corpus: the configuration-parsing digest differ: "))
      << run.out;
  EXPECT_TRUE(has(run.out, "PASS  golden kernels")) << run.out;
}

// --- the oracle part -------------------------------------------------------------------------

TEST(Selftest, EveryWorkloadIsCheckedAndPasses) {
  const worker::ProductionServices production;
  SelftestOptions options;
  options.isa = IsaPath::kScalar;
  const Run run = run_over(production.services(), options, kRight);
  EXPECT_EQ(run.code, 0) << run.out;
  for (const std::string& name : workloads::BuiltinRegistry{}.names()) {
    EXPECT_TRUE(has(run.out, "PASS  " + name + " [scalar]: 4 unit(s) computed, 4 checked by the oracle"))
        << name << "\n"
        << run.out;
  }
  EXPECT_FALSE(has(run.out, "FAIL")) << run.out;
}

TEST(Selftest, AWorkloadWhoseOracleFindsAFaultFailsTheCommand) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  TestRegistry registry({"compression", "sort"});
  registry.wrong = true;
  services.workloads = &registry;
  SelftestOptions options;
  options.isa = IsaPath::kScalar;
  const Run run = run_over(services, options, kRight);
  EXPECT_EQ(run.code, kExitSelftestFailed) << run.out;
  EXPECT_TRUE(has(run.out, "FAIL  compression [scalar]: ")) << run.out;
  EXPECT_TRUE(has(run.out, "finding(s), the first: ExactBitCorruption")) << run.out;
  EXPECT_TRUE(has(run.out, "selftest FAILED: 4 of 4 checks failed")) << "golden checks pass too\n"
                                                                    << run.out;
}

TEST(Selftest, VerboseShowsThePathsTheTallyAndEveryFindingInFull) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  TestRegistry registry({"compression"});
  registry.wrong = true;
  services.workloads = &registry;
  SelftestOptions options;
  options.isa = IsaPath::kScalar;
  options.verbose = true;
  const Run run = run_over(services, options, kRight);
  EXPECT_TRUE(has(run.out, "paths this build holds: scalar")) << run.out;
  EXPECT_TRUE(has(run.out, "paths this CPU can run: ")) << run.out;
  EXPECT_TRUE(has(run.out, "4 unit(s) computed")) << run.out;
  EXPECT_TRUE(has(run.out, "  check: ")) << "the record in full\n" << run.out;
}

TEST(Selftest, WithoutVerboseTheTalliesAndRecordsAreNotPrinted) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  TestRegistry registry({"compression"});
  registry.wrong = true;
  services.workloads = &registry;
  SelftestOptions options;
  options.isa = IsaPath::kScalar;
  const Run run = run_over(services, options, kRight);
  EXPECT_FALSE(has(run.out, "paths this build holds")) << run.out;
  EXPECT_FALSE(has(run.out, "  check: ")) << run.out;
}

TEST(Selftest, AWorkloadThatCannotBeSetUpFailsItsCheckNamingWhy) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  const TestRegistry registry({"compression"});
  services.workloads = &registry;
  SelftestOptions options;
  options.isa = IsaPath::kScalar;
  options.footprint_bytes = 1;
  const Run run = run_over(services, options, kRight);
  EXPECT_EQ(run.code, kExitSelftestFailed) << run.out;
  EXPECT_TRUE(has(run.out, "FAIL  compression [scalar]: could not be set up: ")) << run.out;
}

class Ghosts final : public workloads::Registry {
 public:
  [[nodiscard]] std::vector<std::string> names() const override { return {"ghost"}; }
  [[nodiscard]] core::Result<std::unique_ptr<workloads::Workload>, workloads::WorkloadError> create(
      std::string_view /*name*/, const workloads::WorkloadConfig& /*config*/) const override {
    return workloads::WorkloadError{workloads::WorkloadFault::kUnknownWorkload, "no ghosts"};
  }
  [[nodiscard]] core::Result<workloads::Descriptor, workloads::WorkloadError> descriptor(
      std::string_view /*name*/) const override {
    return workloads::WorkloadError{workloads::WorkloadFault::kUnknownWorkload, "no ghosts"};
  }
};

TEST(Selftest, ANameTheRegistryWillNotDescribeFailsItsCheck) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  const Ghosts registry;
  services.workloads = &registry;
  const Run run = run_over(services, SelftestOptions{}, kRight);
  EXPECT_EQ(run.code, kExitSelftestFailed) << run.out;
  EXPECT_TRUE(has(run.out, "FAIL  ghost: the registry would not describe it: unknown workload: no ghosts"))
      << run.out;
}

TEST(Selftest, ARunnerThatRefusesAFailsTheCheckWithItsWords) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  const TestRegistry registry({"compression"});
  services.workloads = &registry;
  services.make_runner = &make_refusing;
  SelftestOptions options;
  options.isa = IsaPath::kScalar;
  Refusing::fail_slice = true;
  const Run slice = run_over(services, options, kRight);
  Refusing::fail_slice = false;
  const Run finish = run_over(services, options, kRight);
  EXPECT_TRUE(has(slice.out, "FAIL  compression [scalar]: the runner refused: slice refused"))
      << slice.out;
  EXPECT_TRUE(has(finish.out, "FAIL  compression [scalar]: the runner refused: finish refused"))
      << finish.out;
  EXPECT_EQ(slice.code, kExitSelftestFailed);
  EXPECT_EQ(finish.code, kExitSelftestFailed);
}

TEST(Selftest, ARequestedPathThisCpuCannotRunIsRefusedBeforeAnyCheck) {
  const worker::ProductionServices production;
  worker::Services services = production.services();
  const FixedProbe scalar_only{IsaSet{IsaPath::kScalar}};
  services.isa = &scalar_only;
  SelftestOptions options;
  options.isa = IsaPath::kAvx2;  // never compiled on ARM64, never supported by this probe
  const Run run = run_over(services, options, kRight);
  EXPECT_EQ(run.code, 1);
  EXPECT_TRUE(run.out.empty()) << run.out;
  EXPECT_TRUE(has(run.err, "loadforge selftest: --isa avx2: ")) << run.err;
}

// --- the command -----------------------------------------------------------------------------

int command(std::vector<std::string_view> args, std::string& out, std::string& err) {
  std::ostringstream o;
  std::ostringstream e;
  const int code = selftest_command(args, o, e);
  out = o.str();
  err = e.str();
  return code;
}

TEST(SelftestCommand, AUsageErrorIsTwoAndWritesNothingToOut) {
  std::string out;
  std::string err;
  EXPECT_EQ(command({"--frob"}, out, err), 2);
  EXPECT_TRUE(out.empty());
  EXPECT_EQ(err, "loadforge selftest: unknown argument '--frob'\n");
}

TEST(SelftestCommand, AnIsaThisMachineCannotRunIsRefusedWithOne) {
  // One of the paths is never runnable here: the other architecture's.
  for (const char* path : {"neon", "avx512"}) {
    std::string out;
    std::string err;
    const int code = command({"--isa", path}, out, err);
    if (code == 1) {
      EXPECT_TRUE(has(err, std::string{"loadforge selftest: --isa "} + path)) << err;
      EXPECT_TRUE(out.empty());
      return;
    }
  }
  FAIL() << "neither neon nor avx512 was refused";
}

}  // namespace
}  // namespace loadforge::cli
