// SPDX-License-Identifier: GPL-3.0-or-later
//
// `--output json,csv`: the formats named, the files written, and every way
// writing can fail.

#include "main/result_files.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "controller/results/result.hpp"

namespace loadforge::cli {
namespace {

namespace fs = std::filesystem;

/// A directory for one test, removed with it.
class ScratchDirectory {
 public:
  ScratchDirectory()
      : path_(fs::temp_directory_path() / ("loadforge-result-files-" + std::to_string(::getpid()) +
                                           "-" + std::to_string(counter_++))) {
    fs::create_directories(path_);
  }
  ~ScratchDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory(ScratchDirectory&&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(ScratchDirectory&&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  static inline int counter_ = 0;
  fs::path path_;
};

std::string slurp(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A sink that keeps what it is given and refuses on request.
class RecordingSink final : public ResultSink {
 public:
  core::Result<core::Ok, std::string> store(const std::string& path,
                                            const std::string& bytes) override {
    if (!refuse_path.empty() && path == refuse_path) {
      return std::string{"refused " + path};
    }
    stored.emplace_back(path, bytes);
    return core::Ok{};
  }
  std::string refuse_path;
  std::vector<std::pair<std::string, std::string>> stored;
};

/// A writer that fails, as a reporter that cannot render a part of a result.
class FailingWriter final : public reporters::ResultWriter {
 public:
  [[nodiscard]] std::string_view format() const noexcept override { return "json"; }
  [[nodiscard]] core::Result<core::Ok, reporters::WriteError> write(
      const results::RunResult& /*result*/, std::ostream& /*out*/) const override {
    return reporters::WriteError{"the signature held a part it cannot render"};
  }
};

TEST(ResultFilesTest, FormatsAreTakenInTheOrderGiven) {
  const auto formats = parse_formats("csv,json");
  ASSERT_TRUE(formats.has_value());
  EXPECT_EQ(formats.value(), (std::vector<std::string>{"csv", "json"}));
  const auto one = parse_formats("json");
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(one.value(), std::vector<std::string>{"json"});
}

TEST(ResultFilesTest, AnUnknownEmptyOrRepeatedFormatIsRefusedWithTheFormatsThatExist) {
  for (const char* bad : {"xml", "json,xml", "", "json,", ",json", "json,,csv"}) {
    const auto formats = parse_formats(bad);
    ASSERT_FALSE(formats.has_value()) << "\"" << bad << "\"";
    EXPECT_NE(formats.error().find("unknown output format"), std::string::npos) << formats.error();
    EXPECT_NE(formats.error().find("json, csv"), std::string::npos) << formats.error();
  }
  const auto twice = parse_formats("json,csv,json");
  ASSERT_FALSE(twice.has_value());
  EXPECT_NE(twice.error().find("named twice"), std::string::npos);
}

TEST(ResultFilesTest, EachFormatHasItsWriterAndAnythingElseHasNone) {
  ASSERT_NE(make_writer("json"), nullptr);
  EXPECT_EQ(make_writer("json")->format(), "json");
  ASSERT_NE(make_writer("csv"), nullptr);
  EXPECT_EQ(make_writer("csv")->format(), "csv");
  EXPECT_EQ(make_writer("yaml"), nullptr);
  EXPECT_EQ(make_writer(""), nullptr);
}

TEST(ResultFilesTest, EveryFormatAskedForIsWrittenToItsOwnFile) {
  RecordingSink sink;
  const results::RunResult result;
  const auto written = write_results(result, {"json", "csv"}, "out", sink);
  ASSERT_TRUE(written.has_value()) << written.error();
  EXPECT_EQ(written.value(), (std::vector<std::string>{"out/run.json", "out/run.csv"}));
  ASSERT_EQ(sink.stored.size(), 2U);
  EXPECT_EQ(sink.stored[0].first, "out/run.json");
  EXPECT_NE(sink.stored[0].second.find("\"schema_version\""), std::string::npos);
  EXPECT_EQ(sink.stored[1].first, "out/run.csv");
  EXPECT_NE(sink.stored[1].second.find("schema_version"), std::string::npos);
}

TEST(ResultFilesTest, NoFormatWritesNothing) {
  RecordingSink sink;
  const auto written = write_results(results::RunResult{}, {}, "out", sink);
  ASSERT_TRUE(written.has_value());
  EXPECT_TRUE(written.value().empty());
  EXPECT_TRUE(sink.stored.empty());
}

TEST(ResultFilesTest, AFormatNoWriterExistsForStopsTheWriting) {
  RecordingSink sink;
  const auto written = write_results(results::RunResult{}, {"json", "yaml"}, "out", sink);
  ASSERT_FALSE(written.has_value());
  EXPECT_NE(written.error().find("\"yaml\""), std::string::npos) << written.error();
  EXPECT_EQ(sink.stored.size(), 1U) << "the file before it stays";
}

TEST(ResultFilesTest, AWriterThatFailsIsReportedWithItsFormatAndItsReason) {
  RecordingSink sink;
  const auto written =
      write_results(results::RunResult{}, {"json"}, "out", sink,
                    [](std::string_view) -> std::unique_ptr<reporters::ResultWriter> {
                      return std::make_unique<FailingWriter>();
                    });
  ASSERT_FALSE(written.has_value());
  EXPECT_NE(written.error().find("the json rendering failed"), std::string::npos);
  EXPECT_NE(written.error().find("cannot render"), std::string::npos);
  EXPECT_TRUE(sink.stored.empty()) << "nothing half-rendered reaches the sink";
}

TEST(ResultFilesTest, ASinkThatRefusesStopsTheWritingAtThatFile) {
  RecordingSink sink;
  sink.refuse_path = "out/run.csv";
  const auto written = write_results(results::RunResult{}, {"json", "csv"}, "out", sink);
  ASSERT_FALSE(written.has_value());
  EXPECT_EQ(written.error(), "refused out/run.csv");
  EXPECT_EQ(sink.stored.size(), 1U);
}

TEST(FileResultSinkTest, TheBytesArriveWholeAndNoPartialFileIsLeftBehind) {
  const ScratchDirectory directory;
  FileResultSink sink;
  const std::string path = (directory.path() / "run.json").string();
  const std::string bytes(100'000, 'x');
  ASSERT_TRUE(sink.store(path, bytes).has_value());
  EXPECT_EQ(slurp(path), bytes);
  EXPECT_FALSE(fs::exists(path + ".partial"));
  // A second result replaces the first, whole.
  ASSERT_TRUE(sink.store(path, "second").has_value());
  EXPECT_EQ(slurp(path), "second");
}

TEST(FileResultSinkTest, ADirectoryThatDoesNotExistIsAnErrorNamingTheFile) {
  const ScratchDirectory directory;
  FileResultSink sink;
  const std::string path = (directory.path() / "missing" / "run.json").string();
  const auto stored = sink.store(path, "bytes");
  ASSERT_FALSE(stored.has_value());
  EXPECT_NE(stored.error().find("cannot create " + path + ".partial"), std::string::npos)
      << stored.error();
}

TEST(FileResultSinkTest, ADeviceThatIsFullIsAnErrorAndNotAShortFile) {
  // /dev/full accepts the open and refuses every write with ENOSPC: the one
  // way to make write(2) fail that needs no privilege and no luck.
  const ScratchDirectory directory;
  const fs::path path = directory.path() / "run.json";
  fs::create_symlink("/dev/full", path.string() + ".partial");
  FileResultSink sink;
  const auto stored = sink.store(path.string(), std::string(100'000, 'x'));
  ASSERT_FALSE(stored.has_value());
  EXPECT_NE(stored.error().find("cannot write"), std::string::npos) << stored.error();
  EXPECT_FALSE(fs::exists(path));
}

TEST(FileResultSinkTest, ADirectoryWhereTheResultBelongsIsAnErrorAndNothingIsReplaced) {
  const ScratchDirectory directory;
  const fs::path path = directory.path() / "run.json";
  fs::create_directories(path / "inside");
  FileResultSink sink;
  const auto stored = sink.store(path.string(), "bytes");
  ASSERT_FALSE(stored.has_value());
  EXPECT_NE(stored.error().find("cannot rename"), std::string::npos) << stored.error();
  EXPECT_TRUE(fs::is_directory(path)) << "what was there is untouched";
}

}  // namespace
}  // namespace loadforge::cli
