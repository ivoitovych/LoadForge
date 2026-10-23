// SPDX-License-Identifier: GPL-3.0-or-later
//
// `--output json,csv`: the formats named, the files written, and every way
// writing can fail.

#include "main/result_files.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <ostream>
#include <span>
#include <string>
#include <vector>

#include "controller/journal/file_ops.hpp"
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
  core::Result<core::Ok, std::string> prepare(const std::string& directory) override {
    prepared.push_back(directory);
    if (refuse_directory) {
      return std::string{"refused " + directory};
    }
    return core::Ok{};
  }
  core::Result<core::Ok, std::string> store(const std::string& path,
                                            const std::string& bytes) override {
    if (!refuse_path.empty() && path == refuse_path) {
      return std::string{"refused " + path};
    }
    stored.emplace_back(path, bytes);
    return core::Ok{};
  }
  std::string refuse_path;
  bool refuse_directory = false;
  std::vector<std::string> prepared;
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
  EXPECT_NE(written.error().find("written before it: out/run.json"), std::string::npos)
      << written.error();
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
  EXPECT_EQ(written.error(), "refused out/run.csv; written before it: out/run.json")
      << "review C3: the user is told which of the two files is there";
  EXPECT_EQ(sink.stored.size(), 1U);
}

TEST(ResultFilesTest, AFirstFileThatFailsHasNoWrittenFileToName) {
  RecordingSink sink;
  sink.refuse_path = "out/run.json";
  const auto written = write_results(results::RunResult{}, {"json", "csv"}, "out", sink);
  ASSERT_FALSE(written.has_value());
  EXPECT_EQ(written.error(), "refused out/run.json");
}

TEST(ResultFilesTest, ARenderingThatFailsAfterTheFirstFileNamesTheFileThatWasWritten) {
  RecordingSink sink;
  // The factory is a plain function pointer, so it counts in a static.
  static int made = 0;
  made = 0;
  const auto written =
      write_results(results::RunResult{}, {"json", "csv"}, "out", sink,
                    [](std::string_view format) -> std::unique_ptr<reporters::ResultWriter> {
                      return ++made == 1 ? make_writer(format) : std::make_unique<FailingWriter>();
                    });
  ASSERT_FALSE(written.has_value());
  EXPECT_NE(written.error().find("the csv rendering failed"), std::string::npos) << written.error();
  EXPECT_NE(written.error().find("written before it: out/run.json"), std::string::npos)
      << written.error();
}

TEST(FileResultSinkTest, ADirectoryThatCanBeWrittenToIsReadyAndLeavesNothingBehind) {
  const ScratchDirectory directory;
  FileResultSink sink;
  ASSERT_TRUE(sink.prepare(directory.path().string()).has_value());
  EXPECT_TRUE(fs::is_empty(directory.path())) << "the probe file is removed";
}

TEST(FileResultSinkTest, ADirectoryThatDoesNotExistIsRefusedBeforeTheRun) {
  const ScratchDirectory directory;
  FileResultSink sink;
  const std::string missing = (directory.path() / "missing").string();
  const auto prepared = sink.prepare(missing);
  ASSERT_FALSE(prepared.has_value());
  EXPECT_EQ(prepared.error(), "\"" + missing + "\" is not a directory");
  // A file is not a directory either.
  const std::string file = (directory.path() / "file").string();
  std::ofstream{file} << "x";
  EXPECT_FALSE(sink.prepare(file).has_value());
}

TEST(FileResultSinkTest, ADirectoryThatCannotBeWrittenToIsRefusedWithTheKernelsReason) {
  // /proc is a directory no file can be created in, for any user.
  FileResultSink sink;
  const auto prepared = sink.prepare("/proc");
  ASSERT_FALSE(prepared.has_value());
  EXPECT_NE(prepared.error().find("cannot create a file in /proc: "), std::string::npos)
      << prepared.error();
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
  EXPECT_NE(stored.error().find("cannot create " + path + ".partial: "), std::string::npos)
      << stored.error();
  EXPECT_NE(stored.error().find("No such file or directory"), std::string::npos) << stored.error();
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
  EXPECT_FALSE(fs::exists(path.string() + ".partial")) << "review C3: no leftover";
}

TEST(FileResultSinkTest, ALeftoverPartialOfAnEarlierRunIsNotThisResultsBeginning) {
  const ScratchDirectory directory;
  const std::string path = (directory.path() / "run.json").string();
  std::ofstream{path + ".partial"} << "a result that was cut off long ago";
  FileResultSink sink;
  ASSERT_TRUE(sink.store(path, "new").has_value());
  EXPECT_EQ(slurp(path), "new");
  EXPECT_FALSE(fs::exists(path + ".partial"));
}

// --- review C3: the file is durable before the name is, and each step is checked ----------------

/// Real files, with a failure forced at one call: `call` of the `nth` descriptor of its kind
/// (the result file, or the directory) fails with `error`. A failing close still closes.
class FaultyOps final : public journal::FileOps {
 public:
  enum class Call : unsigned char { kOpen, kWrite, kFsync, kClose };
  void fail(Call call, bool directory, int nth, int error) {
    call_ = call;
    directory_ = directory;
    nth_ = nth;
    error_ = error;
  }
  [[nodiscard]] int closed_fds() const { return closes_; }

  core::Result<int, platform::SyscallError> open(const std::string& path, Mode mode) override {
    const bool directory = mode == Mode::kDirectory;
    if (strikes(Call::kOpen, directory)) {
      return platform::SyscallError{error_, "open", path};
    }
    auto opened = real_.open(path, mode);
    if (opened.has_value()) {
      directory_fds_[opened.value()] = directory;
    }
    return opened;
  }
  core::Result<std::size_t, platform::SyscallError> write(
      int fd, std::span<const std::byte> bytes) override {
    if (strikes(Call::kWrite, false)) {
      return platform::SyscallError{error_, "write", std::to_string(fd)};
    }
    return real_.write(fd, bytes);
  }
  core::Result<std::size_t, platform::SyscallError> read(int fd,
                                                         std::span<std::byte> into) override {
    return real_.read(fd, into);
  }
  core::Result<core::Ok, platform::SyscallError> fsync(int fd) override {
    if (strikes(Call::kFsync, directory_fds_.at(fd))) {
      return platform::SyscallError{error_, "fsync", std::to_string(fd)};
    }
    return real_.fsync(fd);
  }
  core::Result<core::Ok, platform::SyscallError> close(int fd) override {
    ++closes_;
    const bool directory = directory_fds_.at(fd);
    auto closed = real_.close(fd);
    if (strikes(Call::kClose, directory)) {
      return platform::SyscallError{error_, "close", std::to_string(fd)};
    }
    return closed;
  }
  core::Result<core::Ok, platform::SyscallError> lock(int fd) override { return real_.lock(fd); }

 private:
  bool strikes(Call call, bool directory) {
    if (call != call_ || directory != directory_ || error_ == 0) {
      return false;
    }
    return --nth_ == 0;
  }
  journal::RealFileOps real_;
  std::map<int, bool> directory_fds_;
  Call call_ = Call::kOpen;
  bool directory_ = false;
  int nth_ = 0;
  int error_ = 0;
  int closes_ = 0;
};

using Call = FaultyOps::Call;

/// The result already there, which a failed write must leave as it was.
class StoreRig : public ::testing::TestWithParam<int> {
 protected:
  StoreRig() : path_((directory_.path() / "run.json").string()), sink_(ops_) {
    std::ofstream{path_} << "the old result";
  }
  /// Stores "new result" with the failure forced, answering the sink's refusal.
  std::string refusal(Call call, bool directory, int nth) {
    ops_.fail(call, directory, nth, GetParam());
    const auto stored = sink_.store(path_, "new result");
    EXPECT_FALSE(stored.has_value());
    return stored.has_value() ? std::string{} : stored.error();
  }
  void expect_nothing_left_and_the_old_result_kept() {
    EXPECT_FALSE(fs::exists(path_ + ".partial")) << "the leftover is removed";
    EXPECT_EQ(slurp(path_), "the old result");
  }

  ScratchDirectory directory_;
  std::string path_;
  FaultyOps ops_;
  FileResultSink sink_;
};

// The errnos each call documents that a test process cannot provoke on demand.
const std::vector<int> kWriteErrors{ENOSPC, EIO, EDQUOT, EFBIG, EBADF, EPERM, ENOMEM};
const std::vector<int> kFsyncErrors{EIO, ENOSPC, EDQUOT, EBADF, EROFS, EINVAL};
const std::vector<int> kCloseErrors{EIO, EBADF, ENOSPC, EDQUOT};
const std::vector<int> kOpenErrors{EACCES, ENOENT, EMFILE, ENFILE, ENOSPC, EROFS, ENOMEM, EDQUOT};

class WriteFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, WriteFails, ::testing::ValuesIn(kWriteErrors));
TEST_P(WriteFails, IsReportedAndLeavesNoPartial) {
  const std::string text = refusal(Call::kWrite, false, 1);
  EXPECT_NE(text.find("cannot write " + path_ + ".partial: "), std::string::npos) << text;
  EXPECT_NE(text.find("(errno " + std::to_string(GetParam()) + ")"), std::string::npos) << text;
  expect_nothing_left_and_the_old_result_kept();
}

class FileFsyncFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, FileFsyncFails, ::testing::ValuesIn(kFsyncErrors));
TEST_P(FileFsyncFails, BeforeTheRenameIsReportedAndLeavesNoPartial) {
  // The directory is synced once inside open (the partial's name), which is the first
  // directory fsync; the result file's fsync is the only fsync of a file.
  const std::string text = refusal(Call::kFsync, false, 1);
  EXPECT_NE(text.find("cannot sync " + path_ + ".partial: "), std::string::npos) << text;
  EXPECT_NE(text.find("(errno " + std::to_string(GetParam()) + ")"), std::string::npos) << text;
  expect_nothing_left_and_the_old_result_kept();
}

class FileCloseFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, FileCloseFails, ::testing::ValuesIn(kCloseErrors));
TEST_P(FileCloseFails, IsReportedAndLeavesNoPartial) {
  const std::string text = refusal(Call::kClose, false, 1);
  EXPECT_NE(text.find("cannot close " + path_ + ".partial: "), std::string::npos) << text;
  EXPECT_NE(text.find("(errno " + std::to_string(GetParam()) + ")"), std::string::npos) << text;
  expect_nothing_left_and_the_old_result_kept();
  EXPECT_EQ(ops_.closed_fds(), 2) << "the partial's directory, and the file once";
}

class PartialDirectoryOpenFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, PartialDirectoryOpenFails, ::testing::ValuesIn(kOpenErrors));
TEST_P(PartialDirectoryOpenFails, IsReportedAsNotCreatedAndLeavesNoPartial) {
  const std::string text = refusal(Call::kOpen, true, 1);
  EXPECT_NE(text.find("cannot create " + path_ + ".partial: "), std::string::npos) << text;
  expect_nothing_left_and_the_old_result_kept();
}

class PartialDirectoryFsyncFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, PartialDirectoryFsyncFails, ::testing::ValuesIn(kFsyncErrors));
TEST_P(PartialDirectoryFsyncFails, IsReportedAsNotCreatedAndLeavesNoPartial) {
  const std::string text = refusal(Call::kFsync, true, 1);
  EXPECT_NE(text.find("cannot create " + path_ + ".partial: "), std::string::npos) << text;
  expect_nothing_left_and_the_old_result_kept();
}

class PartialDirectoryCloseFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, PartialDirectoryCloseFails, ::testing::ValuesIn(kCloseErrors));
TEST_P(PartialDirectoryCloseFails, IsReportedAsNotCreatedAndLeavesNoPartial) {
  const std::string text = refusal(Call::kClose, true, 1);
  EXPECT_NE(text.find("cannot create " + path_ + ".partial: "), std::string::npos) << text;
  expect_nothing_left_and_the_old_result_kept();
}

// After the rename the new result is in place; what failed is only its durability, and the
// error says so, naming the file.
class FinalDirectoryOpenFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, FinalDirectoryOpenFails, ::testing::ValuesIn(kOpenErrors));
TEST_P(FinalDirectoryOpenFails, IsReportedAfterTheRename) {
  const std::string text = refusal(Call::kOpen, true, 2);
  EXPECT_NE(text.find("cannot make " + path_ + " durable: "), std::string::npos) << text;
  EXPECT_EQ(slurp(path_), "new result");
  EXPECT_FALSE(fs::exists(path_ + ".partial"));
}

class FinalDirectoryFsyncFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, FinalDirectoryFsyncFails, ::testing::ValuesIn(kFsyncErrors));
TEST_P(FinalDirectoryFsyncFails, IsReportedAfterTheRename) {
  const std::string text = refusal(Call::kFsync, true, 2);
  EXPECT_NE(text.find("cannot make " + path_ + " durable: "), std::string::npos) << text;
  EXPECT_EQ(slurp(path_), "new result");
}

class FinalDirectoryCloseFails : public StoreRig {};
INSTANTIATE_TEST_SUITE_P(Errno, FinalDirectoryCloseFails, ::testing::ValuesIn(kCloseErrors));
TEST_P(FinalDirectoryCloseFails, IsReportedAfterTheRename) {
  const std::string text = refusal(Call::kClose, true, 2);
  EXPECT_NE(text.find("cannot make " + path_ + " durable: "), std::string::npos) << text;
  EXPECT_EQ(slurp(path_), "new result");
}

TEST(FileResultSinkTest, AnInterruptedCloseIsNotAFailureOfTheResult) {
  const ScratchDirectory directory;
  const std::string path = (directory.path() / "run.json").string();
  FaultyOps ops;
  FileResultSink sink(ops);
  ops.fail(Call::kClose, false, 1, EINTR);
  ASSERT_TRUE(sink.store(path, "bytes").has_value());
  EXPECT_EQ(slurp(path), "bytes");
}

TEST(FileResultSinkTest, EveryStepIsMadeInTheOrderThatMakesTheResultDurable) {
  // Pinned by what was asked of the kernel: the file is synced before it is closed and renamed,
  // and the directory after the rename (a second directory sync, after the file's).
  const ScratchDirectory directory;
  const std::string path = (directory.path() / "run.json").string();
  FaultyOps ops;
  FileResultSink sink(ops);
  ASSERT_TRUE(sink.store(path, "bytes").has_value());
  EXPECT_EQ(ops.closed_fds(), 3) << "the directory twice, the file once, each closed";
}

}  // namespace
}  // namespace loadforge::cli
