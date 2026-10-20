// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_REPORTERS_WRITER_HPP
#define LOADFORGE_REPORTERS_WRITER_HPP

// CONTRACT (ADR 0004). How a finished run is written down (IMPLEMENTATION
// M3: CSV and JSON, schema-versioned). Frozen: changed only by a contract
// change, never inside a stream.
//
//   Providers: one reporter stream per format (src/reporters/json/,
//     src/reporters/csv/); the console reporter may provide it too.
//   Consumer: the controller, which writes the run's result in every format
//     the run asked for.
//
// The live narration of a run -- the banner, the event lines -- is the older
// controller::Reporter (controller/report.hpp) and stays as it is. This is the
// record that outlives the run.

#include <ostream>
#include <string>
#include <string_view>

#include "controller/results/result.hpp"
#include "core/result.hpp"

namespace loadforge::reporters {

struct WriteError {
  std::string detail;  ///< What failed: the stream, or which part of the result.
};

/// Writes a RunResult in one format.
///
/// write(result, out):
///   * is deterministic: equal results give byte-identical output;
///   * states result.schema_version, in decimal;
///   * renders every part of the result -- the fingerprint, the capability
///     report, the signature, every workload with its tally, duty cycle and
///     findings, every worker, the observers, the verdict -- with nothing
///     summarised away;
///   * renders every absent value as the format's explicit absence, and every
///     Unavailable with its kind's words (topology::describe), its detail and
///     its remediation verbatim (escaped as the format requires);
///   * names every finding by its record's kind_name(), and gives a
///     NumericalVerificationFailure no bit fields and an ExactBitCorruption
///     without a difference no bit classification -- a field that does not
///     exist is not printed as zero;
///   * returns WriteError if the stream is not good() before writing or
///     after it: output that did not reach its destination is a failure,
///     never a success with a short file.
///
/// format() is the format's name, lower case, as configuration names it:
/// "json", "csv".
///
/// Thread-safety: const; one writer may write several results at once to
/// different streams.
class ResultWriter {
 public:
  ResultWriter() = default;
  ResultWriter(const ResultWriter&) = delete;
  ResultWriter(ResultWriter&&) = delete;
  ResultWriter& operator=(const ResultWriter&) = delete;
  ResultWriter& operator=(ResultWriter&&) = delete;
  virtual ~ResultWriter() = default;

  [[nodiscard]] virtual std::string_view format() const noexcept = 0;
  [[nodiscard]] virtual core::Result<core::Ok, WriteError> write(const results::RunResult& result,
                                                                 std::ostream& out) const = 0;
};

}  // namespace loadforge::reporters

#endif
