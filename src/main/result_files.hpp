// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_RESULT_FILES_HPP
#define LOADFORGE_MAIN_RESULT_FILES_HPP

// `--output json,csv`: a run's result, written in each format asked for.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "controller/results/result.hpp"
#include "core/result.hpp"
#include "reporters/writer.hpp"

namespace loadforge::cli {

/// Where finished bytes go. A file in the real one; a script in a test.
class ResultSink {
 public:
  ResultSink() = default;
  ResultSink(const ResultSink&) = delete;
  ResultSink(ResultSink&&) = delete;
  ResultSink& operator=(const ResultSink&) = delete;
  ResultSink& operator=(ResultSink&&) = delete;
  virtual ~ResultSink() = default;

  /// All of `bytes` at `path`, or nothing there: a result that is half written
  /// is worse than none, so the real sink writes a sibling and renames it.
  [[nodiscard]] virtual core::Result<core::Ok, std::string> store(const std::string& path,
                                                                  const std::string& bytes) = 0;
};

class FileResultSink final : public ResultSink {
 public:
  [[nodiscard]] core::Result<core::Ok, std::string> store(const std::string& path,
                                                          const std::string& bytes) override;
};

/// The formats named by a comma-separated list ("json,csv"), in the order
/// given. An empty list, an empty item, a repeat, and a format no writer
/// exists for are refused, with the formats that exist named.
[[nodiscard]] core::Result<std::vector<std::string>, std::string> parse_formats(
    std::string_view text);

/// The writer for a format, or null when there is none.
[[nodiscard]] std::unique_ptr<reporters::ResultWriter> make_writer(std::string_view format);

using WriterFactory = std::unique_ptr<reporters::ResultWriter> (*)(std::string_view format);

/// Writes `result` in each format to "<directory>/run.<format>" and answers
/// the paths written. Stops at the first failure and says which file and why;
/// the files written before it stay. `writer_for` is the factory: a test names
/// one whose writer fails, which the real writers do not do on a string stream.
[[nodiscard]] core::Result<std::vector<std::string>, std::string> write_results(
    const results::RunResult& result, const std::vector<std::string>& formats,
    const std::string& directory, ResultSink& sink, WriterFactory writer_for = make_writer);

}  // namespace loadforge::cli

#endif
