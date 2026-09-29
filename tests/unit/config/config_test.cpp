// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every way a configuration can be refused, each with the message it
// produces (docs/IMPLEMENTATION.md §4, M1 exit criterion 5), and the round
// trip that shows a parsed configuration can be written back and read again.

#include "config/config.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "core/byte_size.hpp"
#include "core/duration.hpp"

namespace loadforge::config {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kMinimal = R"(
schema_version = 1

[run]
mode     = "stress"
duration = "30s"

[[workload]]
name    = "null"
threads = "all"
)";

constexpr std::string_view kFull = R"(
schema_version = 1

[run]
mode     = "benchmark"
duration = "20m"

[limits]
max_temperature_c = 95
memory            = "70%"

[[workload]]
name    = "compression"
threads = 8
enabled = true
memory  = "8GiB"

[[workload]]
name    = "graph"
threads = "all"
enabled = false
)";

Config parsed(std::string_view text) {
  auto result = parse(text);
  if (!result.has_value()) {
    ADD_FAILURE() << describe(result.error());
    return Config{};
  }
  return result.value();
}

ConfigError refused(std::string_view text) {
  auto result = parse(text);
  if (result.has_value()) {
    ADD_FAILURE() << "accepted a document that should have been refused";
    return ConfigError{};
  }
  return result.error();
}

// --- words -------------------------------------------------------------------

TEST(ConfigWordsTest, ModesRoundTripThroughTheirNamesAndTheTableIsTotal) {
  for (const Mode mode : {Mode::kBenchmark, Mode::kStress, Mode::kExplore}) {
    EXPECT_EQ(parse_mode(describe(mode)), mode);
  }
  EXPECT_EQ(describe(Mode::kBenchmark), "benchmark");
  EXPECT_EQ(describe(static_cast<Mode>(99)), "unrecognised mode");
  EXPECT_FALSE(parse_mode("Stress").has_value()) << "case matters; it is a keyword";
  EXPECT_FALSE(parse_mode("").has_value());
}

TEST(ConfigWordsTest, ThreadsDistinguishAllFromACount) {
  EXPECT_TRUE(Threads::all().is_all());
  EXPECT_FALSE(Threads::count(4).is_all());
  EXPECT_EQ(Threads::count(4).value(), 4U);
  EXPECT_EQ(Threads::all(), Threads::all());
  EXPECT_NE(Threads::all(), Threads::count(1));
}

TEST(ConfigWordsTest, AnErrorRendersOnlyThePartsThatApply) {
  EXPECT_EQ(describe(ConfigError{"quick.toml", "run.duration", 7, "bad"}),
            "quick.toml:7: run.duration: bad");
  EXPECT_EQ(describe(ConfigError{"quick.toml", "workload", 0, "missing"}),
            "quick.toml: workload: missing");
  EXPECT_EQ(describe(ConfigError{"quick.toml", "", 3, "TOML syntax: x"}),
            "quick.toml:3: TOML syntax: x");
  EXPECT_EQ(describe(ConfigError{"", "limits.memory", 0, "too much"}), "limits.memory: too much");
  EXPECT_EQ(describe(ConfigError{"", "", 0, "bare"}), "bare");
}

// --- accepted documents ------------------------------------------------------

TEST(ConfigParseTest, TheMinimalDocumentParsesWithEveryOptionalAbsent) {
  const Config config = parsed(kMinimal);
  EXPECT_EQ(config.run.mode, Mode::kStress);
  EXPECT_EQ(config.run.duration, 30s);
  EXPECT_FALSE(config.limits.max_temperature_c.has_value());
  EXPECT_FALSE(config.limits.memory.has_value());
  ASSERT_EQ(config.workloads.size(), 1U);
  EXPECT_EQ(config.workloads[0].name, "null");
  EXPECT_TRUE(config.workloads[0].threads.is_all());
  EXPECT_TRUE(config.workloads[0].enabled) << "enabled unless said otherwise";
  EXPECT_FALSE(config.workloads[0].memory.has_value());
}

TEST(ConfigParseTest, TheFullDocumentParsesEveryField) {
  const Config config = parsed(kFull);
  EXPECT_EQ(config.run.mode, Mode::kBenchmark);
  EXPECT_EQ(config.run.duration, 20min);
  EXPECT_EQ(config.limits.max_temperature_c, 95);
  EXPECT_EQ(config.limits.memory, core::ByteSize::percent(70).value());
  ASSERT_EQ(config.workloads.size(), 2U);
  EXPECT_EQ(config.workloads[0].name, "compression");
  EXPECT_EQ(config.workloads[0].threads, Threads::count(8));
  EXPECT_TRUE(config.workloads[0].enabled);
  EXPECT_EQ(config.workloads[0].memory, core::ByteSize::bytes(std::uint64_t{8} << 30));
  EXPECT_EQ(config.workloads[1].name, "graph");
  EXPECT_TRUE(config.workloads[1].threads.is_all());
  EXPECT_FALSE(config.workloads[1].enabled);
}

TEST(ConfigParseTest, AnEmptyLimitsTableIsTheSameAsNone) {
  const Config config = parsed(
      "schema_version = 1\n[run]\nmode = \"stress\"\nduration = \"1s\"\n[limits]\n"
      "[[workload]]\nname = \"null\"\nthreads = 1\n");
  EXPECT_EQ(config.limits, Limits{});
}

TEST(ConfigParseTest, TheOriginNamesTheDocumentInEveryError) {
  auto result = parse("schema_version = 2\n", "runs/night.toml");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().origin, "runs/night.toml");
  EXPECT_EQ(describe(result.error()).substr(0, 17), "runs/night.toml:1");
}

// --- whole-document refusals -------------------------------------------------

TEST(ConfigRefusalTest, MalformedTomlIsRefusedWithTheParsersOwnWordsAndLine) {
  const ConfigError error = refused("schema_version = 1\n[run\nmode = ");
  EXPECT_EQ(error.key, "");
  EXPECT_EQ(error.line, 2U);
  EXPECT_EQ(error.message.substr(0, 13), "TOML syntax: ");
  EXPECT_GT(error.message.size(), 13U) << "the parser's description follows";
}

TEST(ConfigRefusalTest, AnEmptyDocumentIsRefusedForTheSchemaVersionItLacks) {
  // Empty is not malformed and not default: it is a document with nothing in
  // it, and the first thing it lacks is named.
  const ConfigError error = refused("");
  EXPECT_EQ(error.key, "schema_version");
  EXPECT_EQ(error.line, 0U);
  EXPECT_EQ(error.message, "schema_version is required; this build reads schema 1");
}

TEST(ConfigRefusalTest, AnUnknownTopLevelKeyIsRefusedByName) {
  const ConfigError error = refused("schema_version = 1\nduration = \"5h\"\n");
  EXPECT_EQ(error.key, "duration");
  EXPECT_EQ(error.line, 2U);
  EXPECT_EQ(error.message, "unknown key; this build knows: schema_version, run, limits, workload");
}

TEST(ConfigRefusalTest, SchemaVersionMustBeAnIntegerThisBuildReads) {
  ConfigError error = refused("schema_version = \"1\"\n");
  EXPECT_EQ(error.key, "schema_version");
  EXPECT_EQ(error.message, "expected an integer, found string");

  error = refused("schema_version = true\n");
  EXPECT_EQ(error.message, "expected an integer, found boolean")
      << "toml++ would read true as 1; the schema must not";

  error = refused("schema_version = 2\n");
  EXPECT_EQ(error.line, 1U);
  EXPECT_EQ(error.message, "schema 2 is not one this build reads (it reads 1)");

  error = refused("schema_version = 0\n");
  EXPECT_EQ(error.message, "schema 0 is not one this build reads (it reads 1)");
}

// --- [run] -------------------------------------------------------------------

TEST(ConfigRefusalTest, TheRunTableIsRequiredAndMustBeATable) {
  ConfigError error = refused("schema_version = 1\n");
  EXPECT_EQ(error.key, "run");
  EXPECT_EQ(error.message, "required table is missing");

  error = refused("schema_version = 1\nrun = \"stress\"\n");
  EXPECT_EQ(error.key, "run");
  EXPECT_EQ(error.line, 2U);
  EXPECT_EQ(error.message, "expected a table, found string");
}

TEST(ConfigRefusalTest, RunRefusesUnknownKeysMissingValuesAndWrongTypes) {
  ConfigError error = refused("schema_version = 1\n[run]\nmode = \"stress\"\nduraton = \"5h\"\n");
  EXPECT_EQ(error.key, "run.duraton");
  EXPECT_EQ(error.line, 4U);
  EXPECT_EQ(error.message, "unknown key; this build knows: mode, duration");

  error = refused("schema_version = 1\n[run]\nduration = \"5h\"\n");
  EXPECT_EQ(error.key, "run.mode");
  EXPECT_EQ(error.message, "required value is missing");

  error = refused("schema_version = 1\n[run]\nmode = 3\nduration = \"5h\"\n");
  EXPECT_EQ(error.key, "run.mode");
  EXPECT_EQ(error.message, "expected a string, found integer");

  error = refused("schema_version = 1\n[run]\nmode = \"soak\"\nduration = \"5h\"\n");
  EXPECT_EQ(error.key, "run.mode");
  EXPECT_EQ(error.line, 3U);
  EXPECT_EQ(error.message, "mode must be one of benchmark, stress, explore");

  error = refused("schema_version = 1\n[run]\nmode = \"stress\"\n");
  EXPECT_EQ(error.key, "run.duration");
  EXPECT_EQ(error.message, "required value is missing");

  error = refused("schema_version = 1\n[run]\nmode = \"stress\"\nduration = 5\n");
  EXPECT_EQ(error.key, "run.duration");
  EXPECT_EQ(error.message, "expected a string, found integer");
}

TEST(ConfigRefusalTest, RunDurationMustParseAndBePositive) {
  ConfigError error = refused("schema_version = 1\n[run]\nmode = \"stress\"\nduration = \"5\"\n");
  EXPECT_EQ(error.key, "run.duration");
  EXPECT_EQ(error.line, 4U);
  EXPECT_EQ(error.message, "value needs a unit suffix");

  error = refused("schema_version = 1\n[run]\nmode = \"stress\"\nduration = \"5 fortnights\"\n");
  EXPECT_EQ(error.message, "unit suffix is not recognised");

  error = refused("schema_version = 1\n[run]\nmode = \"stress\"\nduration = \"0s\"\n");
  EXPECT_EQ(error.message, "duration must be longer than zero");
}

TEST(ConfigParseTest, TheShortestDurationIsOneMillisecond) {
  const Config config = parsed(
      "schema_version = 1\n[run]\nmode = \"stress\"\nduration = \"1ms\"\n"
      "[[workload]]\nname = \"null\"\nthreads = 1\n");
  EXPECT_EQ(config.run.duration, 1ms);
}

// --- [limits] ----------------------------------------------------------------

constexpr std::string_view kRunAndWorkload =
    "[run]\nmode = \"stress\"\nduration = \"1s\"\n[[workload]]\nname = \"null\"\nthreads = 1\n";

std::string with_limits(std::string_view limits) {
  return "schema_version = 1\n" + std::string{kRunAndWorkload} + "[limits]\n" + std::string{limits};
}

TEST(ConfigRefusalTest, LimitsMustBeATableWithKnownKeys) {
  ConfigError error = refused("schema_version = 1\nlimits = 95\n" + std::string{kRunAndWorkload});
  EXPECT_EQ(error.key, "limits");
  EXPECT_EQ(error.message, "expected a table, found integer");

  error = refused(with_limits("max_temprature_c = 95\n"));
  EXPECT_EQ(error.key, "limits.max_temprature_c");
  EXPECT_EQ(error.line, 9U);
  EXPECT_EQ(error.message, "unknown key; this build knows: max_temperature_c, memory");
}

TEST(ConfigRefusalTest, TemperatureLimitHasAStatedDomain) {
  ConfigError error = refused(with_limits("max_temperature_c = \"95\"\n"));
  EXPECT_EQ(error.key, "limits.max_temperature_c");
  EXPECT_EQ(error.message, "expected an integer, found string");
  error = refused(with_limits("max_temperature_c = 95.0\n"));
  EXPECT_EQ(error.message, "expected an integer, found floating-point");

  for (const std::string_view value : {"0", "-5", "151", "950"}) {
    error = refused(with_limits("max_temperature_c = " + std::string{value} + "\n"));
    EXPECT_EQ(error.key, "limits.max_temperature_c") << value;
    EXPECT_EQ(error.message, "must be between 1 and 150 degrees Celsius") << value;
  }
  EXPECT_EQ(parsed(with_limits("max_temperature_c = 1\n")).limits.max_temperature_c, 1);
  EXPECT_EQ(parsed(with_limits("max_temperature_c = 150\n")).limits.max_temperature_c, 150);
}

TEST(ConfigRefusalTest, MemoryLimitMustBeAByteSizeAndNotZero) {
  ConfigError error = refused(with_limits("memory = 8\n"));
  EXPECT_EQ(error.key, "limits.memory");
  EXPECT_EQ(error.message, "expected a string such as \"8GiB\" or \"70%\", found integer");

  error = refused(with_limits("memory = \"8GB\"\n"));
  EXPECT_EQ(error.message, "unit suffix is not recognised");

  error = refused(with_limits("memory = \"120%\"\n"));
  EXPECT_EQ(error.message, "value is out of the accepted range");

  error = refused(with_limits("memory = \"0\"\n"));
  EXPECT_EQ(error.line, 9U);
  EXPECT_EQ(error.message, "memory must be more than zero bytes");

  EXPECT_EQ(parsed(with_limits("memory = \"1\"\n")).limits.memory, core::ByteSize::bytes(1));
  EXPECT_EQ(parsed(with_limits("memory = \"100%\"\n")).limits.memory,
            core::ByteSize::percent(100).value());
}

// --- [[workload]] ------------------------------------------------------------

constexpr std::string_view kSchemaAndRun =
    "schema_version = 1\n[run]\nmode = \"stress\"\nduration = \"1s\"\n";

std::string with_workload(std::string_view body) {
  return std::string{kSchemaAndRun} + "[[workload]]\n" + std::string{body};
}

TEST(ConfigRefusalTest, AtLeastOneWorkloadIsRequiredAndEachMustBeATable) {
  ConfigError error = refused(kSchemaAndRun);
  EXPECT_EQ(error.key, "workload");
  EXPECT_EQ(error.line, 0U);
  EXPECT_EQ(error.message, "at least one [[workload]] is required");

  // A top-level key goes before [run], or TOML files it under [run].
  const std::string run_only{kSchemaAndRun.substr(19)};
  error = refused("schema_version = 1\nworkload = []\n" + run_only);
  EXPECT_EQ(error.key, "workload");
  EXPECT_EQ(error.line, 2U);
  EXPECT_EQ(error.message, "at least one [[workload]] is required");

  error = refused("schema_version = 1\nworkload = \"null\"\n" + run_only);
  EXPECT_EQ(error.message, "expected an array of tables ([[workload]]), found string");

  error =
      refused("schema_version = 1\nworkload = [{ name = \"null\", threads = 1 }, 7]\n" + run_only);
  EXPECT_EQ(error.key, "workload[1]");
  EXPECT_EQ(error.line, 2U);
  EXPECT_EQ(error.message, "expected a table, found integer");
}

TEST(ConfigRefusalTest, WorkloadRefusesUnknownKeysAndAMissingOrEmptyName) {
  ConfigError error = refused(with_workload("name = \"null\"\nthreads = 1\nthread = 2\n"));
  EXPECT_EQ(error.key, "workload[0].thread");
  EXPECT_EQ(error.line, 8U);
  EXPECT_EQ(error.message, "unknown key; this build knows: name, threads, enabled, memory");

  error = refused(with_workload("threads = 1\n"));
  EXPECT_EQ(error.key, "workload[0].name");
  EXPECT_EQ(error.message, "required value is missing");

  error = refused(with_workload("name = 7\nthreads = 1\n"));
  EXPECT_EQ(error.key, "workload[0].name");
  EXPECT_EQ(error.message, "expected a string, found integer");

  error = refused(with_workload("name = \"\"\nthreads = 1\n"));
  EXPECT_EQ(error.key, "workload[0].name");
  EXPECT_EQ(error.line, 6U);
  EXPECT_EQ(error.message, "name must not be empty");
}

TEST(ConfigRefusalTest, ThreadsIsAPositiveIntegerOrAll) {
  ConfigError error = refused(with_workload("name = \"null\"\n"));
  EXPECT_EQ(error.key, "workload[0].threads");
  EXPECT_EQ(error.line, 0U);
  EXPECT_EQ(error.message, "required value is missing");

  for (const std::string_view value : {"\"some\"", "\"ALL\"", "0", "-1", "4294967296"}) {
    error = refused(with_workload("name = \"null\"\nthreads = " + std::string{value} + "\n"));
    EXPECT_EQ(error.key, "workload[0].threads") << value;
    EXPECT_EQ(error.line, 7U) << value;
    EXPECT_EQ(error.message, "threads must be a positive integer or \"all\"") << value;
  }

  error = refused(with_workload("name = \"null\"\nthreads = true\n"));
  EXPECT_EQ(error.message, "threads must be a positive integer or \"all\", found boolean");

  EXPECT_EQ(parsed(with_workload("name = \"null\"\nthreads = 1\n")).workloads[0].threads,
            Threads::count(1));
  EXPECT_EQ(parsed(with_workload("name = \"null\"\nthreads = 4294967295\n")).workloads[0].threads,
            Threads::count(std::numeric_limits<std::uint32_t>::max()))
      << "the last count a mask-sized integer can hold";
}

TEST(ConfigRefusalTest, EnabledMustBeABooleanAndMemoryAByteSize) {
  ConfigError error = refused(with_workload("name = \"null\"\nthreads = 1\nenabled = \"yes\"\n"));
  EXPECT_EQ(error.key, "workload[0].enabled");
  EXPECT_EQ(error.line, 8U);
  EXPECT_EQ(error.message, "expected true or false, found string");
  error = refused(with_workload("name = \"null\"\nthreads = 1\nenabled = 1\n"));
  EXPECT_EQ(error.message, "expected true or false, found integer")
      << "toml++ would read 1 as true; the schema must not";

  error = refused(with_workload("name = \"null\"\nthreads = 1\nmemory = \"lots\"\n"));
  EXPECT_EQ(error.key, "workload[0].memory");
  EXPECT_EQ(error.message, "value does not begin with a valid number");

  const Config config =
      parsed(with_workload("name = \"null\"\nthreads = 1\nenabled = false\nmemory = \"512MiB\"\n"));
  EXPECT_FALSE(config.workloads[0].enabled);
  EXPECT_EQ(config.workloads[0].memory, core::ByteSize::bytes(std::uint64_t{512} << 20));
}

TEST(ConfigRefusalTest, TheSecondWorkloadsFaultNamesTheSecondWorkload) {
  const ConfigError error = refused(
      with_workload("name = \"null\"\nthreads = 1\n[[workload]]\nname = \"null\"\nthreads = 0\n"));
  EXPECT_EQ(error.key, "workload[1].threads");
  EXPECT_EQ(error.line, 10U);
}

// --- round trip --------------------------------------------------------------

TEST(ConfigRoundTripTest, WhatIsWrittenParsesBackToTheSameConfiguration) {
  for (const std::string_view text : {kMinimal, kFull}) {
    const Config original = parsed(text);
    const std::string rendered = to_toml(original);
    auto reparsed = parse(rendered, "rendered");
    ASSERT_TRUE(reparsed.has_value()) << describe(reparsed.error()) << "\n" << rendered;
    EXPECT_EQ(reparsed.value(), original) << rendered;
  }
}

TEST(ConfigRoundTripTest, TheRenderingIsTheDocumentAReaderExpects) {
  const std::string rendered = to_toml(parsed(kFull));
  EXPECT_NE(rendered.find("schema_version = 1"), std::string::npos);
  EXPECT_NE(rendered.find("[run]"), std::string::npos);
  EXPECT_NE(rendered.find("duration = '20m'"), std::string::npos);
  EXPECT_NE(rendered.find("memory = '70%'"), std::string::npos);
  EXPECT_NE(rendered.find("[[workload]]"), std::string::npos);
  EXPECT_NE(rendered.find("threads = 'all'"), std::string::npos);
  EXPECT_NE(rendered.find("threads = 8"), std::string::npos);
  EXPECT_NE(rendered.find("memory = '8GiB'"), std::string::npos);
}

TEST(ConfigRoundTripTest, EqualityNoticesEveryField) {
  const Config base = parsed(kFull);
  Config changed = base;
  changed.run.duration = 21min;
  EXPECT_NE(changed, base);
  changed = base;
  changed.limits.max_temperature_c = 96;
  EXPECT_NE(changed, base);
  changed = base;
  changed.run.mode = Mode::kExplore;
  EXPECT_NE(changed, base);
  changed = base;
  changed.limits.memory = std::nullopt;
  EXPECT_NE(changed, base);
  changed = base;
  changed.workloads[1].enabled = true;
  EXPECT_NE(changed, base);
  changed = base;
  changed.workloads[1].threads = Threads::count(2);
  EXPECT_NE(changed, base);
  changed = base;
  changed.workloads[1].memory = core::ByteSize::bytes(1);
  EXPECT_NE(changed, base);
  changed = base;
  changed.workloads[0].name = "compressor";
  EXPECT_NE(changed, base);
  EXPECT_EQ(loadforge::config::Run{}, loadforge::config::Run{});
  EXPECT_NE(Workload{}, base.workloads[0]);
}

// --- files (T8) --------------------------------------------------------------

class ConfigFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory = std::filesystem::temp_directory_path() /
                ("loadforge-config-" + std::to_string(::getpid()) + "-" +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path write(const std::string& name, std::string_view contents) {
    const std::filesystem::path path = directory / name;
    std::ofstream out(path);
    out << contents;
    return path;
  }
  std::filesystem::path directory;
};

TEST_F(ConfigFileTest, LoadsARealFileAndNamesItInErrors) {
  const auto good = load(write("quick.toml", kMinimal));
  ASSERT_TRUE(good.has_value()) << describe(good.error());
  EXPECT_EQ(good.value(), parsed(kMinimal));

  const std::filesystem::path bad_path = write("bad.toml", "schema_version = 2\n");
  const auto bad = load(bad_path);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().origin, bad_path.string());
  EXPECT_EQ(bad.error().line, 1U);
}

TEST_F(ConfigFileTest, AFileLongerThanOneReadChunkIsReadWhole) {
  // load() reads in 4 KiB pieces; a document that spans more than one is the
  // only thing that takes the loop round twice. The padding is a comment, so
  // the parsed result is the minimal configuration.
  std::string padded{kMinimal};
  padded += "\n# ";
  padded.append(8192, 'x');
  padded += "\n";
  const auto result = load(write("padded.toml", padded));
  ASSERT_TRUE(result.has_value()) << describe(result.error());
  EXPECT_EQ(result.value(), parsed(kMinimal));
}

TEST_F(ConfigFileTest, AMissingFileIsRefusedAsUnopenable) {
  const std::filesystem::path path = directory / "absent.toml";
  const auto result = load(path);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().origin, path.string());
  EXPECT_EQ(result.error().key, "");
  EXPECT_EQ(result.error().message, "cannot open the file");
}

TEST_F(ConfigFileTest, ADirectoryOpensButCannotBeRead) {
  // Linux opens a directory for reading and fails the read (journal: the
  // seam's T8 established the same for open(2)); an ifstream reports that as
  // a bad stream. It is not "cannot open", and it is not an empty document.
  const auto result = load(directory);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().origin, directory.string());
  EXPECT_EQ(result.error().message, "cannot read the file");
}

TEST_F(ConfigFileTest, AnEmptyFileIsAnEmptyDocumentNotAMissingOne) {
  const auto result = load(write("empty.toml", ""));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().key, "schema_version");
  EXPECT_EQ(result.error().message, "schema_version is required; this build reads schema 1");
}

// --- memory resolution -------------------------------------------------------

TEST(MemoryBudgetTest, TheCeilingIsTheLowerOfAvailableAndTheCgroupLimit) {
  EXPECT_EQ(ceiling_of(MemoryBudget{1000, std::nullopt}), 1000U);
  EXPECT_EQ(ceiling_of(MemoryBudget{1000, 600}), 600U);
  EXPECT_EQ(ceiling_of(MemoryBudget{1000, 5000}), 1000U)
      << "a cgroup limit above what is available does not raise the ceiling";
  EXPECT_EQ(MemoryBudget{}, MemoryBudget{});
  EXPECT_NE((MemoryBudget{1, std::nullopt}), (MemoryBudget{2, std::nullopt}));
  EXPECT_NE((MemoryBudget{1, 5}), (MemoryBudget{1, 6}));
}

TEST(MemoryBudgetTest, AShareIsAShareOfTheCeilingRoundedDown) {
  const MemoryBudget budget{1000, 150};
  EXPECT_EQ(resolve_memory(core::ByteSize::percent(50).value(), budget, "limits.memory").value(),
            75U);
  EXPECT_EQ(resolve_memory(core::ByteSize::percent(100).value(), budget, "limits.memory").value(),
            150U);
  EXPECT_EQ(resolve_memory(core::ByteSize::percent(1).value(), MemoryBudget{199, std::nullopt},
                           "limits.memory")
                .value(),
            1U)
      << "1.99 rounds down";
  // The product would overflow a 64-bit multiply; the split arithmetic does not.
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(resolve_memory(core::ByteSize::percent(100).value(), MemoryBudget{kMax, std::nullopt},
                           "limits.memory")
                .value(),
            kMax);
  EXPECT_EQ(resolve_memory(core::ByteSize::percent(50).value(), MemoryBudget{kMax, std::nullopt},
                           "limits.memory")
                .value(),
            kMax / 2);
}

TEST(MemoryBudgetTest, AnAbsoluteSizeIsTakenAsIsOrRefusedNeverClamped) {
  const MemoryBudget budget{1000, std::nullopt};
  EXPECT_EQ(resolve_memory(core::ByteSize::bytes(1000), budget, "limits.memory").value(), 1000U)
      << "exactly the ceiling is allowed";
  auto refused = resolve_memory(core::ByteSize::bytes(1001), budget, "workload[0].memory");
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().key, "workload[0].memory");
  EXPECT_EQ(refused.error().message, "requests 1001 bytes but the machine can offer 1000");
  EXPECT_EQ(describe(refused.error()),
            "workload[0].memory: requests 1001 bytes but the machine can offer 1000");
}

}  // namespace
}  // namespace loadforge::config
