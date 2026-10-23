// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/result_files.hpp"

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

#include "controller/journal/file_storage.hpp"
#include "platform/syscall_error.hpp"
#include "reporters/csv/csv_writer.hpp"
#include "reporters/json/json_writer.hpp"

namespace loadforge::cli {

namespace {
constexpr std::string_view kKnownFormats = "json, csv";

/// errno in words, thread-safely: strerror is not.
std::string os_error() { return std::error_code(errno, std::generic_category()).message(); }
}  // namespace

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
      return "cannot create a file in " + directory + ": " + os_error();
    }
  }
  std::filesystem::remove(probe, ignored);
  return core::Ok{};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (where, what) is the shape of a write.
core::Result<core::Ok, std::string> FileResultSink::store(const std::string& path,
                                                          const std::string& bytes) {
  const std::string partial = path + ".partial";
  // A leftover of an earlier run that died is not this result's beginning: the
  // storage appends, so it is removed first.
  std::error_code ignored;
  std::filesystem::remove(partial, ignored);
  // A failure from here on leaves nothing of ours behind: the ".partial" goes,
  // and `path` is as it was.
  const auto abandon = [&partial](const std::string& what, const std::string& detail) {
    std::error_code gone;
    std::filesystem::remove(partial, gone);
    return what + " " + partial + ": " + detail;
  };
  auto storage = journal::FileStorage::open(*ops_, partial);
  if (!storage.has_value()) {
    return abandon("cannot create", storage.error().detail);
  }
  if (auto written = storage.value()->append(std::as_bytes(std::span{bytes}));
      !written.has_value()) {
    return abandon("cannot write", written.error().detail);
  }
  // The bytes are on the disk before the name is: a rename that outlives a
  // power cut over a file whose contents did not is a result that reads as
  // written and is not.
  if (auto synced = storage.value()->sync(); !synced.has_value()) {
    return abandon("cannot sync", synced.error().detail);
  }
  // Closed here, and the close read: a file system may report a deferred write
  // error only there.
  if (auto closed = storage.value()->close(); !closed.has_value()) {
    return abandon("cannot close", closed.error().detail);
  }
  std::error_code renamed;
  std::filesystem::rename(partial, path, renamed);
  if (renamed) {
    std::string detail = "cannot rename " + partial + " to " + path + ": " + renamed.message();
    std::filesystem::remove(partial, ignored);
    return detail;
  }
  if (auto durable = journal::sync_directory(*ops_, path); !durable.has_value()) {
    return "cannot make " + path + " durable: " + platform::describe(durable.error());
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
  // A failure after the first format says which files were written: the user
  // would otherwise find one of two result files and not know which.
  const auto refused = [&paths](std::string why) {
    if (!paths.empty()) {
      why += "; written before it:";
      for (const std::string& written : paths) {
        why += ' ';
        why += written;
      }
    }
    return why;
  };
  for (const std::string& format : formats) {
    const auto writer = writer_for(format);
    if (writer == nullptr) {
      return refused("no writer for output format \"" + format + "\"");
    }
    std::ostringstream bytes;
    if (auto written = writer->write(result, bytes); !written.has_value()) {
      std::string refusal = "the ";
      refusal += format;
      refusal += " rendering failed: ";
      refusal += written.error().detail;
      return refused(std::move(refusal));
    }
    std::string path = directory;
    path += "/run.";
    path += format;
    if (auto stored = sink.store(path, bytes.str()); !stored.has_value()) {
      return refused(stored.error());
    }
    paths.push_back(path);
  }
  return paths;
}

}  // namespace loadforge::cli
