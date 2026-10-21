// SPDX-License-Identifier: GPL-3.0-or-later
//
// The run fingerprint read from the machine (F12), through the platform seam.
//
// THE TIERS
// ---------
// T2 forces each errno the reads can meet through FakeSyscalls, because the
// tests run as root and root is never denied. The real tree reads below show
// the kernel's own answers for the states a tree can hold: the machine this
// was captured on (tests/fixtures/fingerprint/vm-x86-64, a real machine's
// files), a missing file, a directory where a file should be, a value the
// field cannot have.
//
// LOADFORGE P7 FIXTURE BUILDER
//
// Tree below builds the trees the committed fixture cannot be: a laptop's
// power, battery, cpufreq, platform-profile and thermal files, from the formats
// the kernel documents (the committed fixture is a virtual machine that has
// none of them), and each hostile state -- missing, a directory in a file's
// place, a malformed value. "Disappears mid-run" and "counter wraps" are not
// states of a one-shot read of these files; the fingerprint is read once,
// before the run starts.

#include "controller/results/fingerprint.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "support/fake_syscalls.hpp"

namespace loadforge::results {
namespace {

using topology::Availability;

class Tree {
 public:
  explicit Tree(const std::string& name)
      : root_(std::filesystem::temp_directory_path() /
              ("loadforge-fingerprint-" + std::to_string(::getpid()) + "-" + name)) {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
    std::filesystem::create_directories(root_);
  }
  ~Tree() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  Tree(const Tree&) = delete;
  Tree& operator=(const Tree&) = delete;
  Tree(Tree&&) = delete;
  Tree& operator=(Tree&&) = delete;

  [[nodiscard]] std::string root() const { return root_.string(); }
  void write(const std::string& path, const std::string& contents) const {
    const std::filesystem::path file = root_.string() + path;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    out << contents;
  }
  void directory(const std::string& path) const {
    std::filesystem::create_directories(root_.string() + path);
  }
  void remove(const std::string& path) const {
    std::error_code ignored;
    std::filesystem::remove_all(root_.string() + path, ignored);
  }

  // A laptop on AC with every file the fingerprint reads, in the kernel's formats.
  void laptop() const {
    write("/proc/cpuinfo",
          "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Example Core i7\n"
          "microcode\t: 0xf4\n\nprocessor\t: 1\nmodel name\t: Other\nmicrocode\t: 0x1\n");
    write("/proc/sys/kernel/osrelease", "6.8.0-45-generic\n");
    write("/sys/devices/system/cpu/smt/active", "1\n");
    write("/sys/class/power_supply/ACAD/online", "1\n");
    write("/sys/class/power_supply/BAT0/status", "Charging\n");
    write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "powersave\n");
    write("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference",
          "balance_performance\n");
    write("/sys/firmware/acpi/platform_profile", "balanced\n");
    write("/sys/devices/system/cpu/intel_pstate/no_turbo", "0\n");
    write("/sys/class/thermal/thermal_zone0/policy", "step_wise\n");
    write("/sys/class/thermal/thermal_zone0/temp", "47500\n");
    write("/sys/kernel/mm/transparent_hugepage/enabled", "always [madvise] never\n");
    write("/proc/meminfo", "MemTotal: 16479424 kB\nMemAvailable: 8000000 kB\n");
    write("/proc/self/cgroup", "0::/\n");
    write("/proc/self/mountinfo",
          "35 24 0:30 / /sys/fs/cgroup rw,nosuid - cgroup2 cgroup2 rw,nsdelegate\n");
    write("/sys/fs/cgroup/cgroup.controllers", "cpuset cpu io memory pids\n");
    write("/sys/fs/cgroup/memory.max", "4294967296\n");
  }

 private:
  std::filesystem::path root_;
};

struct Read {
  FingerprintReading reading;
  std::vector<std::string> gap_fields;
  [[nodiscard]] const FingerprintGap* gap(std::string_view field) const {
    for (const FingerprintGap& gap : reading.gaps) {
      if (gap.field == field) {
        return &gap;
      }
    }
    return nullptr;
  }
};

Read read_real(const std::string& root, const FingerprintContext& context = {}) {
  platform::RealSyscalls syscalls;
  platform::FileSystem filesystem(syscalls);
  Read read{read_fingerprint(filesystem, root, context), {}};
  for (const FingerprintGap& gap : read.reading.gaps) {
    read.gap_fields.push_back(gap.field);
  }
  return read;
}

const std::vector<std::string_view> kFileFields = {"cpu_model",
                                                   "microcode",
                                                   "kernel",
                                                   "smt_enabled",
                                                   "power_source",
                                                   "battery_state",
                                                   "cpufreq_governor",
                                                   "energy_performance_preference",
                                                   "platform_profile",
                                                   "turbo_enabled",
                                                   "cooling_profile",
                                                   "huge_page_policy",
                                                   "temperature_at_start_c",
                                                   "cgroup_memory_limit_bytes"};

// --- the pure parts -----------------------------------------------------------

TEST(NonEmptyTest, EmptyIsNothingAndAnythingElseIsItself) {
  EXPECT_EQ(non_empty(""), std::nullopt);
  EXPECT_EQ(non_empty("abc"), std::optional<std::string>{"abc"});
}

TEST(CpuinfoValueTest, TheFirstCpusValueOfAKeyTrimmed) {
  const std::vector<std::string> lines = {"processor\t: 0", "model name\t: First  ", "",
                                          "model name\t: Second"};
  EXPECT_EQ(cpuinfo_value(lines, "model name"), std::optional<std::string>{"First"});
}

TEST(CpuinfoValueTest, ALongerKeyIsNotThisKeyAndAKeyWithoutAColonIsNotAKey) {
  EXPECT_EQ(cpuinfo_value({"model name\t: X"}, "model"), std::nullopt);
  EXPECT_EQ(cpuinfo_value({"microcode"}, "microcode"), std::nullopt);
  EXPECT_EQ(cpuinfo_value({"microcode   "}, "microcode"), std::nullopt);
  EXPECT_EQ(cpuinfo_value({"microcode:0x1"}, "microcode"), std::optional<std::string>{"0x1"});
  EXPECT_EQ(cpuinfo_value({"microcode\t:"}, "microcode"), std::optional<std::string>{""});
  EXPECT_EQ(cpuinfo_value({}, "microcode"), std::nullopt);
}

TEST(SelectedWordTest, TheBracketedWordAndNothingElse) {
  EXPECT_EQ(selected_word("always [madvise] never"), std::optional<std::string>{"madvise"});
  EXPECT_EQ(selected_word("[always] madvise never"), std::optional<std::string>{"always"});
  EXPECT_EQ(selected_word("always madvise [never]"), std::optional<std::string>{"never"});
  EXPECT_EQ(selected_word("always madvise never"), std::nullopt);
  EXPECT_EQ(selected_word("always [madvise never"), std::nullopt);
  EXPECT_EQ(selected_word("always madvise] never"), std::nullopt);
  EXPECT_EQ(selected_word("] always ["), std::nullopt);
  EXPECT_EQ(selected_word("[a] [b]"), std::nullopt);
  EXPECT_EQ(selected_word("[a b]]"), std::nullopt);
  EXPECT_EQ(selected_word("[[a]"), std::nullopt);
  EXPECT_EQ(selected_word("always [] never"), std::nullopt);
  EXPECT_EQ(selected_word(""), std::nullopt);
}

TEST(BuildTest, ThisBuildSaysWhatItWasBuiltWith) {
  const BuildInfo build = this_build();
  EXPECT_FALSE(build.loadforge_version.empty());
  EXPECT_FALSE(build.compiler.empty());
  EXPECT_NE(build.flags.find("-ffp-contract=off"), std::string::npos);
  if (build.git_commit.has_value()) {
    EXPECT_EQ(build.git_commit->size(), 40U) << *build.git_commit;
  }
}

// --- the machine this was captured on ------------------------------------------

TEST(FingerprintFixtureTest, ARealVirtualMachinesFilesGiveWhatItHas) {
  const Read read =
      read_real(std::string(LOADFORGE_SOURCE_DIR) + "/tests/fixtures/fingerprint/vm-x86-64");
  const Fingerprint& fingerprint = read.reading.fingerprint;
  EXPECT_EQ(fingerprint.cpu_model,
            std::optional<std::string>{"Intel(R) Xeon(R) Processor @ 2.10GHz"});
  EXPECT_EQ(fingerprint.microcode, std::optional<std::string>{"0x1"});
  EXPECT_EQ(fingerprint.kernel, std::optional<std::string>{"6.18.44-fc-v139"});
  EXPECT_EQ(fingerprint.smt_enabled, std::optional<bool>{false});
  EXPECT_EQ(fingerprint.huge_page_policy, std::optional<std::string>{"madvise"});
  EXPECT_EQ(fingerprint.cgroup_memory_limit_bytes, std::optional<std::uint64_t>{14343688192ULL});
  // What a virtual machine does not have is absent, each with its gap.
  for (const char* absent :
       {"power_source", "battery_state", "cpufreq_governor", "energy_performance_preference",
        "platform_profile", "turbo_enabled", "cooling_profile", "temperature_at_start_c"}) {
    const FingerprintGap* gap = read.gap(absent);
    ASSERT_NE(gap, nullptr) << absent;
    EXPECT_EQ(gap->why.kind, Availability::kAbsent) << absent;
    EXPECT_FALSE(gap->why.path.empty()) << absent;
  }
  EXPECT_EQ(fingerprint.power_source, std::nullopt);
  EXPECT_EQ(fingerprint.temperature_at_start_c, std::nullopt);
  EXPECT_EQ(read.reading.gaps.size(), 8U) << "no gap for a field that was read";
}

TEST(FingerprintFixtureTest, TheBuildAndTheCallersFactsAreCarriedThrough) {
  FingerprintContext context;
  context.build = {"1.2.3", "abc123", "gcc 13", "-O2"};
  fpenv::FpState fp;
  fp.flush_to_zero = true;
  context.fp_environment = fp;
  context.isa_path = simd::IsaPath::kAvx2;
  const Fingerprint fingerprint =
      read_real("/nonexistent-machine-root", context).reading.fingerprint;
  EXPECT_EQ(fingerprint.loadforge_version, "1.2.3");
  EXPECT_EQ(fingerprint.git_commit, std::optional<std::string>{"abc123"});
  EXPECT_EQ(fingerprint.compiler, "gcc 13");
  EXPECT_EQ(fingerprint.flags, "-O2");
  EXPECT_EQ(fingerprint.fp_environment, std::optional<fpenv::FpState>{fp});
  EXPECT_EQ(fingerprint.isa_path, std::optional<simd::IsaPath>{simd::IsaPath::kAvx2});
  EXPECT_EQ(fingerprint.cpu_model, std::nullopt) << "no machine, no reading";
}

// --- a laptop, field by field ----------------------------------------------------

TEST(FingerprintTreeTest, ALaptopOnAcGivesEveryField) {
  const Tree tree("laptop");
  tree.laptop();
  const Read read = read_real(tree.root());
  const Fingerprint& fingerprint = read.reading.fingerprint;
  EXPECT_EQ(fingerprint.cpu_model, std::optional<std::string>{"Example Core i7"});
  EXPECT_EQ(fingerprint.microcode, std::optional<std::string>{"0xf4"});
  EXPECT_EQ(fingerprint.kernel, std::optional<std::string>{"6.8.0-45-generic"});
  EXPECT_EQ(fingerprint.smt_enabled, std::optional<bool>{true});
  EXPECT_EQ(fingerprint.power_source, std::optional<std::string>{"ac"});
  EXPECT_EQ(fingerprint.battery_state, std::optional<std::string>{"Charging"});
  EXPECT_EQ(fingerprint.cpufreq_governor, std::optional<std::string>{"powersave"});
  EXPECT_EQ(fingerprint.energy_performance_preference,
            std::optional<std::string>{"balance_performance"});
  EXPECT_EQ(fingerprint.platform_profile, std::optional<std::string>{"balanced"});
  EXPECT_EQ(fingerprint.turbo_enabled, std::optional<bool>{true});
  EXPECT_EQ(fingerprint.cooling_profile, std::optional<std::string>{"step_wise"});
  EXPECT_EQ(fingerprint.temperature_at_start_c, std::optional<double>{47.5});
  EXPECT_EQ(fingerprint.huge_page_policy, std::optional<std::string>{"madvise"});
  EXPECT_EQ(fingerprint.cgroup_memory_limit_bytes, std::optional<std::uint64_t>{4294967296ULL});
  EXPECT_TRUE(read.reading.gaps.empty());
}

TEST(FingerprintTreeTest, TheOtherAdaptersAndBatteriesAreAsked) {
  const Tree tree("adapters");
  tree.laptop();
  tree.remove("/sys/class/power_supply/ACAD");
  tree.remove("/sys/class/power_supply/BAT0");
  tree.write("/sys/class/power_supply/AC0/online", "0\n");
  tree.write("/sys/class/power_supply/BAT1/status", "Discharging\n");
  const Fingerprint fingerprint = read_real(tree.root()).reading.fingerprint;
  EXPECT_EQ(fingerprint.power_source, std::optional<std::string>{"battery"});
  EXPECT_EQ(fingerprint.battery_state, std::optional<std::string>{"Discharging"});
  for (const char* name : {"AC", "ADP0", "ADP1"}) {
    const Tree each(std::string("adapter-") + name);
    each.laptop();
    each.remove("/sys/class/power_supply/ACAD");
    each.write(std::string("/sys/class/power_supply/") + name + "/online", "1\n");
    EXPECT_EQ(read_real(each.root()).reading.fingerprint.power_source,
              std::optional<std::string>{"ac"})
        << name;
  }
}

TEST(FingerprintTreeTest, TurboIsReadFromEitherFileAndNoTurboIsTheOppositeQuestion) {
  const Tree tree("turbo");
  tree.laptop();
  tree.write("/sys/devices/system/cpu/intel_pstate/no_turbo", "1\n");
  EXPECT_EQ(read_real(tree.root()).reading.fingerprint.turbo_enabled, std::optional<bool>{false});
  tree.remove("/sys/devices/system/cpu/intel_pstate");
  tree.write("/sys/devices/system/cpu/cpufreq/boost", "1\n");
  EXPECT_EQ(read_real(tree.root()).reading.fingerprint.turbo_enabled, std::optional<bool>{true});
  tree.write("/sys/devices/system/cpu/cpufreq/boost", "0\n");
  EXPECT_EQ(read_real(tree.root()).reading.fingerprint.turbo_enabled, std::optional<bool>{false});
}

TEST(FingerprintTreeTest, WhenNoCandidateAnswersTheGapIsTheOneThatSaysMost) {
  const Tree tree("best-gap");
  tree.laptop();
  tree.remove("/sys/devices/system/cpu/intel_pstate");
  // Nothing at all: the first candidate's absence.
  Read read = read_real(tree.root());
  ASSERT_NE(read.gap("turbo_enabled"), nullptr);
  EXPECT_EQ(read.gap("turbo_enabled")->why.kind, Availability::kAbsent);
  EXPECT_NE(read.gap("turbo_enabled")->why.path.find("intel_pstate/no_turbo"), std::string::npos);
  // The second candidate is malformed: that is more than an absence.
  tree.write("/sys/devices/system/cpu/cpufreq/boost", "maybe\n");
  read = read_real(tree.root());
  ASSERT_NE(read.gap("turbo_enabled"), nullptr);
  EXPECT_EQ(read.gap("turbo_enabled")->why.kind, Availability::kMalformed);
  EXPECT_NE(read.gap("turbo_enabled")->why.path.find("cpufreq/boost"), std::string::npos);
  EXPECT_NE(read.gap("turbo_enabled")->why.detail.find("maybe"), std::string::npos);
  // The first is malformed and the second absent: the first stays.
  tree.remove("/sys/devices/system/cpu/cpufreq");
  tree.write("/sys/devices/system/cpu/intel_pstate/no_turbo", "2\n");
  read = read_real(tree.root());
  ASSERT_NE(read.gap("turbo_enabled"), nullptr);
  EXPECT_EQ(read.gap("turbo_enabled")->why.kind, Availability::kMalformed);
  EXPECT_NE(read.gap("turbo_enabled")->why.path.find("intel_pstate/no_turbo"), std::string::npos);
  // Both malformed: the first.
  tree.write("/sys/devices/system/cpu/cpufreq/boost", "7\n");
  read = read_real(tree.root());
  EXPECT_NE(read.gap("turbo_enabled")->why.path.find("intel_pstate/no_turbo"), std::string::npos);
}

TEST(FingerprintTreeTest, EveryFileFieldMissingIsAbsentWithItsGap) {
  const Tree tree("empty");
  const Read read = read_real(tree.root());
  EXPECT_EQ(read.gap_fields.size(), kFileFields.size());
  for (const std::string_view field : kFileFields) {
    const FingerprintGap* gap = read.gap(field);
    ASSERT_NE(gap, nullptr) << field;
    EXPECT_EQ(gap->why.kind, Availability::kAbsent) << field;
  }
  const Fingerprint& fingerprint = read.reading.fingerprint;
  EXPECT_EQ(fingerprint.cpu_model, std::nullopt);
  EXPECT_EQ(fingerprint.smt_enabled, std::nullopt);
  EXPECT_EQ(fingerprint.cgroup_memory_limit_bytes, std::nullopt);
}

TEST(FingerprintTreeTest, ADirectoryWhereAFileShouldBeIsUnreadableNotAbsent) {
  const Tree tree("directories");
  tree.laptop();
  for (const char* path :
       {"/proc/cpuinfo", "/proc/sys/kernel/osrelease", "/sys/devices/system/cpu/smt/active",
        "/sys/class/power_supply/ACAD/online", "/sys/class/power_supply/BAT0/status",
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
        "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference",
        "/sys/firmware/acpi/platform_profile", "/sys/devices/system/cpu/intel_pstate/no_turbo",
        "/sys/class/thermal/thermal_zone0/policy", "/sys/class/thermal/thermal_zone0/temp",
        "/sys/kernel/mm/transparent_hugepage/enabled"}) {
    tree.remove(path);
    tree.directory(path);
  }
  const Read read = read_real(tree.root());
  for (const std::string_view field :
       {"cpu_model", "microcode", "kernel", "smt_enabled", "power_source", "battery_state",
        "cpufreq_governor", "energy_performance_preference", "platform_profile", "cooling_profile",
        "temperature_at_start_c", "huge_page_policy"}) {
    const FingerprintGap* gap = read.gap(field);
    ASSERT_NE(gap, nullptr) << field;
    EXPECT_EQ(gap->why.kind, Availability::kUnreadable) << field;
  }
  // no_turbo is unreadable and boost is absent: the gap is the unreadable one.
  const FingerprintGap* turbo = read.gap("turbo_enabled");
  ASSERT_NE(turbo, nullptr);
  EXPECT_EQ(turbo->why.kind, Availability::kUnreadable);
}

TEST(FingerprintTreeTest, AValueTheFieldCannotHaveIsMalformedAndSaysWhatWasFound) {
  const Tree tree("malformed");
  tree.laptop();
  tree.write("/proc/cpuinfo", "processor\t: 0\n");
  tree.write("/proc/sys/kernel/osrelease", "");
  tree.write("/sys/devices/system/cpu/smt/active", "2\n");
  tree.write("/sys/class/power_supply/ACAD/online", "yes\n");
  tree.write("/sys/class/power_supply/BAT0/status", "");
  tree.write("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "");
  tree.write("/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference", "");
  tree.write("/sys/firmware/acpi/platform_profile", "");
  tree.write("/sys/devices/system/cpu/intel_pstate/no_turbo", "x\n");
  tree.write("/sys/class/thermal/thermal_zone0/policy", "");
  tree.write("/sys/class/thermal/thermal_zone0/temp", "hot\n");
  tree.write("/sys/kernel/mm/transparent_hugepage/enabled", "always madvise never\n");
  const Read read = read_real(tree.root());
  EXPECT_EQ(read.gap("cpu_model")->why.kind, Availability::kAbsent) << "no line is an absence";
  EXPECT_NE(read.gap("cpu_model")->why.detail.find("model name"), std::string::npos);
  for (const std::string_view field :
       {"kernel", "smt_enabled", "power_source", "battery_state", "cpufreq_governor",
        "energy_performance_preference", "platform_profile", "turbo_enabled", "cooling_profile",
        "temperature_at_start_c", "huge_page_policy"}) {
    const FingerprintGap* gap = read.gap(field);
    ASSERT_NE(gap, nullptr) << field;
    EXPECT_EQ(gap->why.kind, Availability::kMalformed) << field;
    EXPECT_FALSE(gap->why.detail.empty()) << field;
  }
  EXPECT_NE(read.gap("smt_enabled")->why.detail.find("\"2\""), std::string::npos);
  EXPECT_NE(read.gap("temperature_at_start_c")->why.detail.find("hot"), std::string::npos);
  EXPECT_EQ(read.reading.fingerprint.temperature_at_start_c, std::nullopt) << "never a zero";
}

TEST(FingerprintTreeTest, TemperaturesAreWholeMillidegreesSignedAndExact) {
  const Tree tree("temperature");
  tree.laptop();
  for (const auto& [text, expected] : std::vector<std::pair<std::string, double>>{
           {"0", 0.0}, {"-5250", -5.25}, {"100000", 100.0}, {"1", 0.001}}) {
    tree.write("/sys/class/thermal/thermal_zone0/temp", text + "\n");
    EXPECT_EQ(read_real(tree.root()).reading.fingerprint.temperature_at_start_c,
              std::optional<double>{expected})
        << text;
  }
  for (const char* bad : {"47.5", "47500 ", "+5", "", "-", "99999999999999999999"}) {
    tree.write("/sys/class/thermal/thermal_zone0/temp", std::string(bad) + "\n");
    EXPECT_EQ(read_real(tree.root()).reading.fingerprint.temperature_at_start_c, std::nullopt)
        << bad;
  }
}

// --- the memory limit --------------------------------------------------------------

TEST(FingerprintTreeTest, NoMemoryLimitIsAbsentWithNoGapAndAnUnreadableBudgetIsAGap) {
  const Tree tree("limits");
  tree.laptop();
  tree.write("/sys/fs/cgroup/memory.max", "max\n");
  Read read = read_real(tree.root());
  EXPECT_EQ(read.reading.fingerprint.cgroup_memory_limit_bytes, std::nullopt);
  EXPECT_EQ(read.gap("cgroup_memory_limit_bytes"), nullptr) << "no limit is a reading";
  tree.remove("/proc/meminfo");
  read = read_real(tree.root());
  EXPECT_EQ(read.reading.fingerprint.cgroup_memory_limit_bytes, std::nullopt);
  const FingerprintGap* gap = read.gap("cgroup_memory_limit_bytes");
  ASSERT_NE(gap, nullptr);
  EXPECT_EQ(gap->why.kind, Availability::kAbsent);
  EXPECT_NE(gap->why.path.find("meminfo"), std::string::npos);
  EXPECT_FALSE(gap->why.detail.empty());
}

// --- every errno, at every read ------------------------------------------------------

TEST(FingerprintErrnoTest, ADeniedOpenIsDeniedAtEveryField) {
  testing::FakeSyscalls syscalls;
  syscalls.fail_open(EACCES);
  platform::FileSystem filesystem(syscalls);
  const FingerprintReading reading = read_fingerprint(filesystem, "", {});
  EXPECT_EQ(reading.gaps.size(), kFileFields.size());
  for (const FingerprintGap& gap : reading.gaps) {
    EXPECT_EQ(gap.why.kind, Availability::kDenied) << gap.field;
  }
  EXPECT_EQ(reading.fingerprint.cpu_model, std::nullopt);
  EXPECT_EQ(reading.fingerprint.smt_enabled, std::nullopt);
}

TEST(FingerprintErrnoTest, AFailingReadIsUnreadableAtEveryField) {
  testing::FakeSyscalls syscalls;
  syscalls.always_fail_read(EIO);
  platform::FileSystem filesystem(syscalls);
  const FingerprintReading reading = read_fingerprint(filesystem, "", {});
  EXPECT_EQ(reading.gaps.size(), kFileFields.size());
  for (const FingerprintGap& gap : reading.gaps) {
    EXPECT_EQ(gap.why.kind, Availability::kUnreadable) << gap.field;
  }
}

TEST(FingerprintErrnoTest, AMissingFileIsAbsentAtEveryField) {
  testing::FakeSyscalls syscalls;
  syscalls.fail_open(ENOENT);
  platform::FileSystem filesystem(syscalls);
  const FingerprintReading reading = read_fingerprint(filesystem, "", {});
  EXPECT_EQ(reading.gaps.size(), kFileFields.size());
  for (const FingerprintGap& gap : reading.gaps) {
    EXPECT_EQ(gap.why.kind, Availability::kAbsent) << gap.field;
  }
}

TEST(FingerprintErrnoTest, ADenialOnALaterCandidateIsNotHiddenByAnEarlierAbsence) {
  // No fake models one path answering ENOENT and the next EACCES, so the first
  // is a real missing file and the second a malformed one: see the tree test
  // above. Here: every candidate of a field is tried before a gap is recorded.
  testing::FakeSyscalls syscalls;
  syscalls.fail_open(ENOENT);
  platform::FileSystem filesystem(syscalls);
  (void)read_fingerprint(filesystem, "/root", {});
  EXPECT_EQ(syscalls.last_path().rfind("/root", 0), 0U) << "every path moves with the root";
}

// --- comparing two fingerprints --------------------------------------------------------

Fingerprint sample() {
  Fingerprint f;
  f.loadforge_version = "1";
  f.git_commit = "c";
  f.compiler = "gcc";
  f.flags = "-O2";
  f.cpu_model = "cpu";
  f.microcode = "0x1";
  f.kernel = "6";
  f.smt_enabled = true;
  f.power_source = "ac";
  f.battery_state = "Full";
  f.cpufreq_governor = "performance";
  f.energy_performance_preference = "performance";
  f.platform_profile = "balanced";
  f.turbo_enabled = true;
  f.cooling_profile = "step_wise";
  f.cgroup_memory_limit_bytes = 1;
  f.huge_page_policy = "madvise";
  f.temperature_at_start_c = 40.0;
  f.fp_environment = fpenv::FpState{};
  f.isa_path = simd::IsaPath::kScalar;
  return f;
}

TEST(DifferingFieldsTest, EqualFingerprintsDifferInNothing) {
  EXPECT_TRUE(differing_fields(sample(), sample()).empty());
  EXPECT_TRUE(differing_fields(Fingerprint{}, Fingerprint{}).empty());
}

TEST(DifferingFieldsTest, EachFieldIsNamedWhenItAloneDiffers) {
  using Mutation = void (*)(Fingerprint&);
  const std::vector<std::pair<std::string_view, Mutation>> mutations = {
      {"loadforge_version", [](Fingerprint& f) { f.loadforge_version = "2"; }},
      {"git_commit", [](Fingerprint& f) { f.git_commit = std::nullopt; }},
      {"compiler", [](Fingerprint& f) { f.compiler = "clang"; }},
      {"flags", [](Fingerprint& f) { f.flags = "-O3"; }},
      {"cpu_model", [](Fingerprint& f) { f.cpu_model = "other"; }},
      {"microcode", [](Fingerprint& f) { f.microcode = std::nullopt; }},
      {"kernel", [](Fingerprint& f) { f.kernel = "7"; }},
      {"smt_enabled", [](Fingerprint& f) { f.smt_enabled = false; }},
      {"power_source", [](Fingerprint& f) { f.power_source = "battery"; }},
      {"battery_state", [](Fingerprint& f) { f.battery_state = "Charging"; }},
      {"cpufreq_governor", [](Fingerprint& f) { f.cpufreq_governor = "powersave"; }},
      {"energy_performance_preference",
       [](Fingerprint& f) { f.energy_performance_preference = "power"; }},
      {"platform_profile", [](Fingerprint& f) { f.platform_profile = "low-power"; }},
      {"turbo_enabled", [](Fingerprint& f) { f.turbo_enabled = false; }},
      {"cooling_profile", [](Fingerprint& f) { f.cooling_profile = "user_space"; }},
      {"cgroup_memory_limit_bytes", [](Fingerprint& f) { f.cgroup_memory_limit_bytes = 2; }},
      {"huge_page_policy", [](Fingerprint& f) { f.huge_page_policy = "never"; }},
      {"temperature_at_start_c", [](Fingerprint& f) { f.temperature_at_start_c = 41.0; }},
      {"fp_environment",
       [](Fingerprint& f) {
         fpenv::FpState state;
         state.flush_to_zero = true;
         f.fp_environment = state;
       }},
      {"isa_path", [](Fingerprint& f) { f.isa_path = simd::IsaPath::kAvx2; }},
  };
  for (const auto& [name, mutate] : mutations) {
    Fingerprint changed = sample();
    mutate(changed);
    const std::vector<std::string_view> differing = differing_fields(sample(), changed);
    ASSERT_EQ(differing.size(), 1U) << name;
    EXPECT_EQ(differing[0], name);
  }
}

TEST(DifferingFieldsTest, OnlyTheTemperatureDescribesTheMomentAndNotTheMachine) {
  EXPECT_TRUE(is_moment_field("temperature_at_start_c"));
  EXPECT_FALSE(is_moment_field("kernel"));
  EXPECT_FALSE(is_moment_field("battery_state"));
  EXPECT_FALSE(is_moment_field(""));
}

}  // namespace
}  // namespace loadforge::results
