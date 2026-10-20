// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8: run.json written to a real file and read back, and a real stream that
// cannot take it. /dev/full accepts the open and fails every write with ENOSPC,
// which is what a full disk does, and no fake of a stream models better.

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include "reporters/json/json_writer.hpp"
#include "unit/controller/results/rich_run.hpp"
#include "unit/reporters/json/mini_json.hpp"

namespace loadforge::reporters {
namespace {

using results::testing::rich_run;
using testing::Json;
using testing::JsonReader;

class RunFile {
 public:
  RunFile()
      : path_(std::filesystem::temp_directory_path() /
              ("loadforge-run-" + std::to_string(::getpid()) + ".json")) {}
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
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(JsonFileTest, ARealFileReadsBackAsTheSameDocumentTheStreamHeld) {
  const RunFile file;
  const results::RunResult run = rich_run();
  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    const auto wrote = JsonWriter{}.write(run, out);
    ASSERT_TRUE(wrote.has_value()) << wrote.error().detail;
  }
  std::ostringstream memory;
  ASSERT_TRUE(JsonWriter{}.write(run, memory).has_value());
  const std::string text = read_back(file.path());
  EXPECT_EQ(text, memory.str()) << "a file and a string stream receive the same bytes";
  const Json doc = JsonReader::parse(text);
  EXPECT_EQ(doc.at("schema_version").text, "1");
  EXPECT_EQ(doc.at("workloads").at(0).at("descriptor").at("name").text, "compression");
  EXPECT_EQ(doc.at("workloads").at(0).at("findings").items.size(), 6U);
  EXPECT_EQ(doc.at("fingerprint").at("kernel").text, "6.8.0");
}

TEST(JsonFileTest, AFullDiskIsAFailureNotAShortFile) {
  std::ofstream full("/dev/full", std::ios::binary);
  ASSERT_TRUE(full.is_open()) << "/dev/full is a Linux device";
  const auto wrote = JsonWriter{}.write(rich_run(), full);
  ASSERT_FALSE(wrote.has_value());
  EXPECT_FALSE(wrote.error().detail.empty());
}

}  // namespace
}  // namespace loadforge::reporters
