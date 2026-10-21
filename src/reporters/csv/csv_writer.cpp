// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporters/csv/csv_writer.hpp"

#include <string>

#include "controller/results/document.hpp"
#include "reporters/csv/csv_text.hpp"

namespace loadforge::reporters {

std::string_view CsvWriter::format() const noexcept { return "csv"; }

core::Result<core::Ok, WriteError> CsvWriter::write(const results::RunResult& result,
                                                    std::ostream& out) const {
  if (!out.good()) {
    std::string detail = "csv: the output stream is not writable";
    return WriteError{std::move(detail)};
  }
  const std::string text = to_csv(results::to_document(result));
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out.good()) {
    std::string detail =
        "csv: the output stream refused " + std::to_string(text.size()) + " bytes of the CSV table";
    return WriteError{std::move(detail)};
  }
  return core::Ok{};
}

}  // namespace loadforge::reporters
