// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/result_files.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "reporters/csv/csv_writer.hpp"
#include "reporters/json/json_writer.hpp"

namespace loadforge::cli {

namespace {
constexpr std::string_view kKnownFormats = "json, csv";
}

core::Result<core::Ok, std::string> FileResultSink::prepare(const std::string& directory) {
  std::error_code ignored;
  if (!std::filesystem::is_directory(directory, ignored)) {
    return "\"" + directory + "\" is not a directory";
  }
  // The only test that a directory can be written to is to write to it.
  const std::string probe = directory + "/.loadforge-write-test";
  {
    std::ofstream file(probe, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      return "cannot create a file in " + directory + ": " + std::strerror(errno);
    }
  }
  std::filesystem::remove(probe, ignored);
  return core::Ok{};
}

core::Result<core::Ok, std::string> FileResultSink::store(const std::string& path,
                                                          const std::string& bytes) {
  const std::string partial = path + ".partial";
  {
    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
      return "cannot create " + partial + ": " + std::strerror(errno);
    }
    file << bytes;
    file.flush();
    if (!file.good()) {
      return "cannot write " + partial + ": " + std::strerror(errno);
    }
  }
  std::error_code renamed;
  std::filesystem::rename(partial, path, renamed);
  if (renamed) {
    return "cannot rename " + partial + " to " + path + ": " + renamed.message();
  }
  return core::Ok{};
}

core::Result<std::vector<std::string>, std::string> parse_formats(std::string_view text) {
  std::vector<std::string> formats;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = text.find(',', begin);
    const std::string_view item =
        text.substr(begin, comma == std::string_view::npos ? comma : comma - begin);
    if (make_writer(item) == nullptr) {
      return "unknown output format \"" + std::string{item} + "\"; the formats are " +
             std::string{kKnownFormats};
    }
    for (const std::string& seen : formats) {
      if (seen == item) {
        return "output format \"" + std::string{item} + "\" is named twice";
      }
    }
    formats.emplace_back(item);
    if (comma == std::string_view::npos) {
      return formats;
    }
    begin = comma + 1;
  }
}

std::unique_ptr<reporters::ResultWriter> make_writer(std::string_view format) {
  if (format == "json") {
    return std::make_unique<reporters::JsonWriter>();
  }
  if (format == "csv") {
    return std::make_unique<reporters::CsvWriter>();
  }
  return nullptr;
}

core::Result<std::vector<std::string>, std::string> write_results(
    const results::RunResult& result, const std::vector<std::string>& formats,
    const std::string& directory, ResultSink& sink, WriterFactory writer_for) {
  std::vector<std::string> paths;
  for (const std::string& format : formats) {
    const auto writer = writer_for(format);
    if (writer == nullptr) {
      return "no writer for output format \"" + format + "\"";
    }
    std::ostringstream bytes;
    if (auto written = writer->write(result, bytes); !written.has_value()) {
      return "the " + format + " rendering failed: " + written.error().detail;
    }
    const std::string path = directory + "/run." + format;
    if (auto stored = sink.store(path, bytes.str()); !stored.has_value()) {
      return stored.error();
    }
    paths.push_back(path);
  }
  return paths;
}

}  // namespace loadforge::cli
