// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_REPORTERS_CSV_MINI_CSV_HPP
#define LOADFORGE_TESTS_UNIT_REPORTERS_CSV_MINI_CSV_HPP

// A strict RFC 4180 reader, written independently of the writer it checks:
// records end in CRLF, a quoted field holds commas, CRs, LFs and doubled
// quotes, a quote inside an unquoted field or text after a closing quote is
// refused, and every record has as many fields as the header.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace loadforge::reporters::testing {

using CsvRecord = std::vector<std::string>;

inline std::vector<CsvRecord> parse_csv(std::string_view text) {
  std::vector<CsvRecord> records;
  CsvRecord record;
  std::string field;
  std::size_t at = 0;
  bool quoted_field = false;
  const auto end_field = [&]() {
    record.push_back(std::move(field));
    field.clear();
    quoted_field = false;
  };
  while (at < text.size()) {
    const char c = text[at];
    if (c == '"' && field.empty() && !quoted_field) {
      quoted_field = true;
      ++at;
      for (;;) {
        if (at >= text.size()) {
          throw std::runtime_error("unterminated quoted field");
        }
        if (text[at] == '"') {
          if (at + 1 < text.size() && text[at + 1] == '"') {
            field += '"';
            at += 2;
            continue;
          }
          ++at;
          break;
        }
        field += text[at++];
      }
      if (at < text.size() && text[at] != ',' && text[at] != '\r') {
        throw std::runtime_error("text after a closing quote");
      }
      continue;
    }
    if (c == '"') {
      throw std::runtime_error("a quote inside an unquoted field");
    }
    if (c == ',') {
      end_field();
      ++at;
      continue;
    }
    if (c == '\r') {
      if (at + 1 >= text.size() || text[at + 1] != '\n') {
        throw std::runtime_error("a bare CR");
      }
      end_field();
      records.push_back(std::move(record));
      record.clear();
      at += 2;
      continue;
    }
    if (c == '\n') {
      throw std::runtime_error("a bare LF");
    }
    field += c;
    ++at;
  }
  if (!field.empty() || !record.empty() || quoted_field) {
    throw std::runtime_error("the last record is not ended by CRLF");
  }
  if (records.empty()) {
    throw std::runtime_error("no header");
  }
  for (const CsvRecord& each : records) {
    if (each.size() != records.front().size()) {
      throw std::runtime_error("a record with " + std::to_string(each.size()) + " fields");
    }
  }
  return records;
}

}  // namespace loadforge::reporters::testing

#endif
