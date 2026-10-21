// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8: the CSV table written to a real file and read back, and a real stream
// that cannot take it (/dev/full fails every write with ENOSPC).

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "reporters/csv/csv_writer.hpp"
#include "unit/controller/results/rich_run.hpp"
#include "unit/reporters/csv/mini_csv.hpp"

namespace loadforge::reporters {
namespace {

using results::testing::rich_run;
using testing::parse_csv;

class RunFile {
 public:
  RunFile()
      : path_(std::filesystem::temp_directory_path() /
              ("loadforge-run-" + std::to_string(::getpid()) + ".csv")) {}
  ~RunFile() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  RunFile(const RunFile&) = delete;
  RunFile& operator=(const RunFile&) = delete;
  RunFile(RunFile&&) = delete;
  RunFile& operator=(RunFile&&) = delete;
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string read_back(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(CsvFileTest, ARealFileReadsBackAsTheSameTableTheStreamHeld) {
  const RunFile file;
  const results::RunResult run = rich_run();
  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    const auto wrote = CsvWriter{}.write(run, out);
    ASSERT_TRUE(wrote.has_value()) << wrote.error().detail;
  }
  std::ostringstream memory;
  ASSERT_TRUE(CsvWriter{}.write(run, memory).has_value());
  const std::string text = read_back(file.path());
  EXPECT_EQ(text, memory.str()) << "a file and a string stream receive the same bytes";
  const auto records = parse_csv(text);
  ASSERT_GT(records.size(), 100U);
  EXPECT_EQ(records[0][0], "schema_version");
  EXPECT_EQ(records[1][1], "schema_version");
  EXPECT_EQ(records[1][2], "1");
}

TEST(CsvFileTest, AFullDiskIsAFailureNotAShortFile) {
  std::ofstream full("/dev/full", std::ios::binary);
  ASSERT_TRUE(full.is_open()) << "/dev/full is a Linux device";
  const auto wrote = CsvWriter{}.write(rich_run(), full);
  ASSERT_FALSE(wrote.has_value());
  EXPECT_FALSE(wrote.error().detail.empty());
}

}  // namespace
}  // namespace loadforge::reporters

namespace loadforge::contracts {
// rich_run.hpp builds on the contract suites' fixtures and so includes
// results_contract.hpp, which registers VerdictContract: this binary leaves it out.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(VerdictContract);
}  // namespace loadforge::contracts
