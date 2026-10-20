// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporters/json/json_text.hpp"

#include <array>
#include <charconv>
#include <cstdint>

namespace loadforge::reporters {

namespace {

constexpr std::string_view kIndent = "  ";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::string_view kReplacement = "\\ufffd";

bool continuation(std::string_view text, std::size_t index, unsigned char low, unsigned char high) {
  const auto byte = static_cast<unsigned char>(text[index]);
  return byte >= low && byte <= high;
}

void indent(std::string& out, std::size_t depth) {
  for (std::size_t level = 0; level < depth; ++level) {
    out += kIndent;
  }
}

template <typename Integer>
void append_integer(std::string& out, Integer value) {
  std::array<char, 24> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  out.append(buffer.data(), written.ptr);
}

void write_node(std::string& out, const results::Node& node, std::size_t depth);

void write_absence(std::string& out, const results::Absence& absence, std::size_t depth) {
  out += "{\n";
  indent(out, depth + 1);
  out += "\"kind\": " + json_string(absence.kind) + ",\n";
  indent(out, depth + 1);
  out += "\"words\": " + json_string(absence.words) + ",\n";
  indent(out, depth + 1);
  out += "\"path\": " + json_string(absence.path) + ",\n";
  indent(out, depth + 1);
  out += "\"detail\": " + json_string(absence.detail) + ",\n";
  indent(out, depth + 1);
  out += "\"remediation\": " + json_string(absence.remediation) + "\n";
  indent(out, depth);
  out += "}";
}

void write_array(std::string& out, const results::Array& array, std::size_t depth) {
  if (array.items.empty()) {
    out += "[]";
    return;
  }
  out += "[\n";
  for (std::size_t index = 0; index < array.items.size(); ++index) {
    indent(out, depth + 1);
    write_node(out, array.items[index], depth + 1);
    out += index + 1 < array.items.size() ? ",\n" : "\n";
  }
  indent(out, depth);
  out += "]";
}

void write_object(std::string& out, const results::Object& object, std::size_t depth) {
  if (object.keys.empty()) {
    out += "{}";
    return;
  }
  out += "{\n";
  for (std::size_t index = 0; index < object.keys.size(); ++index) {
    const results::Node& value = object.values[index];
    const bool absent = value.kind() == results::NodeKind::kAbsence;
    indent(out, depth + 1);
    out += json_string(object.keys[index]) + ": ";
    if (absent) {
      out += "null,\n";
      indent(out, depth + 1);
      out += json_string(object.keys[index] + "_unavailable") + ": ";
      write_absence(out, value.as_absence(), depth + 1);
    } else {
      write_node(out, value, depth + 1);
    }
    out += index + 1 < object.keys.size() ? ",\n" : "\n";
  }
  indent(out, depth);
  out += "}";
}

void write_node(std::string& out, const results::Node& node, std::size_t depth) {
  switch (node.kind()) {
    case results::NodeKind::kNull:
      out += "null";
      return;
    case results::NodeKind::kBool:
      out += node.as_bool() ? "true" : "false";
      return;
    case results::NodeKind::kInt:
      append_integer(out, node.as_integer());
      return;
    case results::NodeKind::kUnsigned:
      append_integer(out, node.as_unsigned());
      return;
    case results::NodeKind::kReal:
      out += results::format_real(node.as_real());
      return;
    case results::NodeKind::kText:
      out += json_string(node.as_text());
      return;
    case results::NodeKind::kArray:
      write_array(out, node.as_array(), depth);
      return;
    case results::NodeKind::kObject:
      write_object(out, node.as_object(), depth);
      return;
    case results::NodeKind::kAbsence:
      break;
  }
  write_absence(out, node.as_absence(), depth);
}

}  // namespace

std::size_t utf8_sequence_length(std::string_view text) noexcept {
  const auto lead = static_cast<unsigned char>(text.front());
  if (lead < 0x80U) {
    return 1;
  }
  // Each row: the lead bytes it covers, how many bytes follow, and the range of
  // the first of them (the others are always 0x80..0xBF).
  struct Form {
    unsigned char first;
    unsigned char last;
    std::size_t length;
    unsigned char second_low;
    unsigned char second_high;
  };
  constexpr std::array<Form, 7> kForms = {{{0xC2, 0xDF, 2, 0x80, 0xBF},
                                           {0xE0, 0xE0, 3, 0xA0, 0xBF},
                                           {0xE1, 0xEC, 3, 0x80, 0xBF},
                                           {0xED, 0xED, 3, 0x80, 0x9F},
                                           {0xEE, 0xEF, 3, 0x80, 0xBF},
                                           {0xF0, 0xF0, 4, 0x90, 0xBF},
                                           {0xF1, 0xF4, 4, 0x80, 0xBF}}};
  for (const Form& form : kForms) {
    if (lead < form.first || lead > form.last) {
      continue;
    }
    // F4 stops at U+10FFFF: its second byte is at most 0x8F.
    const unsigned char second_high = lead == 0xF4U ? 0x8FU : form.second_high;
    if (text.size() < form.length || !continuation(text, 1, form.second_low, second_high)) {
      return 0;
    }
    for (std::size_t index = 2; index < form.length; ++index) {
      if (!continuation(text, index, 0x80, 0xBF)) {
        return 0;
      }
    }
    return form.length;
  }
  return 0;
}

std::string json_string(std::string_view text) {
  std::string out = "\"";
  std::size_t index = 0;
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte == '"') {
      out += "\\\"";
    } else if (byte == '\\') {
      out += "\\\\";
    } else if (byte == '\n') {
      out += "\\n";
    } else if (byte == '\r') {
      out += "\\r";
    } else if (byte == '\t') {
      out += "\\t";
    } else if (byte < 0x20U || byte == 0x7FU) {
      out += "\\u00";
      out += kHexDigits[byte >> 4U];
      out += kHexDigits[byte & 0xFU];
    } else {
      const std::size_t length = utf8_sequence_length(text.substr(index));
      if (length == 0) {
        out += kReplacement;
        ++index;
        continue;
      }
      out.append(text.substr(index, length));
      index += length;
      continue;
    }
    ++index;
  }
  out += '"';
  return out;
}

std::string to_json(const results::Node& document) {
  std::string out;
  write_node(out, document, 0);
  out += '\n';
  return out;
}

}  // namespace loadforge::reporters
