// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporters/json/json_writer.hpp"

#include <string>

#include "controller/results/document.hpp"
#include "reporters/json/json_text.hpp"

namespace loadforge::reporters {

std::string_view JsonWriter::format() const noexcept { return "json"; }

core::Result<core::Ok, WriteError> JsonWriter::write(const results::RunResult& result,
                                                     std::ostream& out) const {
  if (!out.good()) {
    std::string detail = "json: the output stream is not writable";
    return WriteError{std::move(detail)};
  }
  const std::string text = to_json(results::to_document(result));
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out.good()) {
    std::string detail =
        "json: the output stream refused " + std::to_string(text.size()) + " bytes of run.json";
    return WriteError{std::move(detail)};
  }
  return core::Ok{};
}

}  // namespace loadforge::reporters
