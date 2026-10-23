// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_RESULT_FILES_HPP
#define LOADFORGE_MAIN_RESULT_FILES_HPP

// `--output json,csv`: a run's result, written in each format asked for.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "controller/journal/file_ops.hpp"
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

  /// Whether results can be written into `directory`, found out before the run
  /// and not after it: a run of hours that finds its output directory missing
  /// only when it ends has lost its result.
  [[nodiscard]] virtual core::Result<core::Ok, std::string> prepare(
      const std::string& directory) = 0;

  /// All of `bytes` at `path`, or nothing there: a result that is half written
  /// is worse than none, so the real sink writes a sibling and renames it.
  [[nodiscard]] virtual core::Result<core::Ok, std::string> store(const std::string& path,
                                                                  const std::string& bytes) = 0;
};

/// The result files on disk. store() makes `path` durable before it says so
/// (review C3): the bytes go to "<path>.partial", which is fsynced and closed
/// with the close checked, then renamed over `path`, then the directory is
/// fsynced so that the rename survives a power cut. A failure at any step
/// removes the ".partial" and leaves what was at `path` as it was.
class FileResultSink final : public ResultSink {
 public:
  FileResultSink() = default;
  /// The file operations are `ops`, so that a test can make each fail; it must
  /// outlive the sink.
  explicit FileResultSink(journal::FileOps& ops) noexcept : ops_(&ops) {}

  [[nodiscard]] core::Result<core::Ok, std::string> prepare(const std::string& directory) override;
  [[nodiscard]] core::Result<core::Ok, std::string> store(const std::string& path,
                                                          const std::string& bytes) override;

 private:
  journal::RealFileOps real_;
  journal::FileOps* ops_ = &real_;
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
