// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporters/csv/csv_text.hpp"

#include <array>
#include <charconv>
#include <cstddef>

namespace loadforge::reporters {

namespace {

constexpr std::string_view kRecordEnd = "\r\n";
constexpr std::string_view kHeader =
    "schema_version,field,value,status,reason,source_path,detail,remediation";

struct Row {
  std::string_view version;
  std::string_view field;
  std::string_view value;
  std::string_view status;
  std::string_view reason;
  std::string_view source_path;
  std::string_view detail;
  std::string_view remediation;
};

void append_row(std::string& out, const Row& row) {
  out += csv_field(row.version);
  for (const std::string_view cell : {row.field, row.value, row.status, row.reason, row.source_path,
                                      row.detail, row.remediation}) {
    out += ',';
    out += csv_field(cell);
  }
  out += kRecordEnd;
}

template <typename Integer>
std::string decimal(Integer value) {
  std::array<char, 24> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return std::string(buffer.data(), written.ptr);
}

void flatten(std::string& out, std::string_view version, const std::string& field,
             const results::Node& node) {
  Row row{version, field, "", "present", "", "", "", ""};
  std::string value;
  switch (node.kind()) {
    case results::NodeKind::kNull:
      row.status = "absent";
      row.reason = "not recorded";
      append_row(out, row);
      return;
    case results::NodeKind::kBool:
      value = node.as_bool() ? "true" : "false";
      break;
    case results::NodeKind::kInt:
      value = decimal(node.as_integer());
      break;
    case results::NodeKind::kUnsigned:
      value = decimal(node.as_unsigned());
      break;
    case results::NodeKind::kReal:
      value = results::format_real(node.as_real());
      break;
    case results::NodeKind::kText:
      value = node.as_text();
      break;
    case results::NodeKind::kArray: {
      const results::Array& array = node.as_array();
      if (array.items.empty()) {
        row.status = "empty";
        row.reason = "no items";
        append_row(out, row);
        return;
      }
      for (std::size_t index = 0; index < array.items.size(); ++index) {
        flatten(out, version, field + "[" + decimal(index) + "]", array.items[index]);
      }
      return;
    }
    case results::NodeKind::kObject: {
      const results::Object& object = node.as_object();
      if (object.keys.empty()) {
        row.status = "empty";
        row.reason = "no members";
        append_row(out, row);
        return;
      }
      for (std::size_t index = 0; index < object.keys.size(); ++index) {
        flatten(out, version, field.empty() ? object.keys[index] : field + "." + object.keys[index],
                object.values[index]);
      }
      return;
    }
    case results::NodeKind::kAbsence: {
      const results::Absence& absence = node.as_absence();
      row.status = absence.kind;
      row.reason = absence.words;
      row.source_path = absence.path;
      row.detail = absence.detail;
      row.remediation = absence.remediation;
      append_row(out, row);
      return;
    }
  }
  row.value = value;
  append_row(out, row);
}

}  // namespace

std::string csv_field(std::string_view text) {
  if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string(text);
  }
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') {
      out += '"';
    }
    out += c;
  }
  out += '"';
  return out;
}

std::string to_csv(const results::Node& document) {
  const std::string version = decimal(document.find("schema_version")->as_unsigned());
  std::string out{kHeader};
  out += kRecordEnd;
  flatten(out, version, "", document);
  return out;
}

}  // namespace loadforge::reporters
