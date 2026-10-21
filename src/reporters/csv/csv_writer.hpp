// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_REPORTERS_CSV_CSV_WRITER_HPP
#define LOADFORGE_REPORTERS_CSV_CSV_WRITER_HPP

// The result as a CSV table (IMPLEMENTATION M3). The columns are in csv_text.hpp
// and the shape of what they hold is docs/result-schema.md.

#include <ostream>
#include <string_view>

#include "reporters/writer.hpp"

namespace loadforge::reporters {

class CsvWriter final : public ResultWriter {
 public:
  [[nodiscard]] std::string_view format() const noexcept override;
  [[nodiscard]] core::Result<core::Ok, WriteError> write(const results::RunResult& result,
                                                         std::ostream& out) const override;
};

}  // namespace loadforge::reporters

#endif
