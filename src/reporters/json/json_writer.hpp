// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_REPORTERS_JSON_JSON_WRITER_HPP
#define LOADFORGE_REPORTERS_JSON_JSON_WRITER_HPP

// run.json (PLAN F12, IMPLEMENTATION M3). The shape is docs/result-schema.md.

#include <ostream>
#include <string_view>

#include "reporters/writer.hpp"

namespace loadforge::reporters {

class JsonWriter final : public ResultWriter {
 public:
  [[nodiscard]] std::string_view format() const noexcept override;
  [[nodiscard]] core::Result<core::Ok, WriteError> write(const results::RunResult& result,
                                                         std::ostream& out) const override;
};

}  // namespace loadforge::reporters

#endif
