// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_REPORTERS_JSON_MINI_JSON_HPP
#define LOADFORGE_TESTS_UNIT_REPORTERS_JSON_MINI_JSON_HPP

// A strict RFC 8259 reader, written independently of the writer it checks: it
// refuses trailing commas, bare control characters, bad escapes, leading
// zeros, a trailing document, and any byte sequence that is not UTF-8. A
// writer that produced text this reads is writing JSON.

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loadforge::reporters::testing {

struct Json {
  enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = Kind::kNull;
  bool boolean = false;
  std::string text;  ///< A string's characters, or a number's literal.
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> members;

  [[nodiscard]] const Json& at(const std::string& key) const {
    for (const auto& [name, value] : members) {
      if (name == key) {
        return value;
      }
    }
    throw std::out_of_range("no member " + key);
  }
  [[nodiscard]] bool has(const std::string& key) const {
    for (const auto& member : members) {
      if (member.first == key) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] const Json& at(std::size_t index) const { return items.at(index); }
};

class JsonReader {
 public:
  explicit JsonReader(std::string_view text) : text_(text) {}

  static Json parse(std::string_view text) {
    JsonReader reader(text);
    Json value = reader.value();
    reader.space();
    if (reader.at_ < text.size()) {
      throw std::runtime_error("trailing characters at " + std::to_string(reader.at_));
    }
    return value;
  }

 private:
  [[nodiscard]] char peek() const {
    if (at_ >= text_.size()) {
      throw std::runtime_error("unexpected end");
    }
    return text_[at_];
  }
  void space() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\r' || text_[at_] == '\t')) {
      ++at_;
    }
  }
  void expect(char c) {
    if (peek() != c) {
      throw std::runtime_error(std::string("expected ") + c + " at " + std::to_string(at_));
    }
    ++at_;
  }
  void word(std::string_view w) {
    if (text_.substr(at_, w.size()) != w) {
      throw std::runtime_error("bad literal at " + std::to_string(at_));
    }
    at_ += w.size();
  }

  Json value() {
    space();
    Json out;
    const char c = peek();
    if (c == '{') {
      ++at_;
      out.kind = Json::Kind::kObject;
      space();
      if (peek() == '}') {
        ++at_;
        return out;
      }
      for (;;) {
        space();
        std::string key = string();
        space();
        expect(':');
        out.members.emplace_back(std::move(key), value());
        space();
        if (peek() == ',') {
          ++at_;
          continue;
        }
        expect('}');
        return out;
      }
    }
    if (c == '[') {
      ++at_;
      out.kind = Json::Kind::kArray;
      space();
      if (peek() == ']') {
        ++at_;
        return out;
      }
      for (;;) {
        out.items.push_back(value());
        space();
        if (peek() == ',') {
          ++at_;
          continue;
        }
        expect(']');
        return out;
      }
    }
    if (c == '"') {
      out.kind = Json::Kind::kString;
      out.text = string();
      return out;
    }
    if (c == 't') {
      word("true");
      out.kind = Json::Kind::kBool;
      out.boolean = true;
      return out;
    }
    if (c == 'f') {
      word("false");
      out.kind = Json::Kind::kBool;
      return out;
    }
    if (c == 'n') {
      word("null");
      return out;
    }
    out.kind = Json::Kind::kNumber;
    out.text = number();
    return out;
  }

  std::string number() {
    const std::size_t start = at_;
    if (peek() == '-') {
      ++at_;
    }
    if (peek() == '0') {
      ++at_;
    } else if (peek() >= '1' && peek() <= '9') {
      while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
        ++at_;
      }
    } else {
      throw std::runtime_error("bad number at " + std::to_string(start));
    }
    if (at_ < text_.size() && text_[at_] == '.') {
      ++at_;
      const std::size_t digits = at_;
      while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
        ++at_;
      }
      if (at_ == digits) {
        throw std::runtime_error("no digits after the point");
      }
    }
    if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
      ++at_;
      if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) {
        ++at_;
      }
      const std::size_t digits = at_;
      while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
        ++at_;
      }
      if (at_ == digits) {
        throw std::runtime_error("no digits in the exponent");
      }
    }
    return std::string(text_.substr(start, at_ - start));
  }

  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::string string() {
    expect('"');
    std::string out;
    for (;;) {
      const auto byte = static_cast<unsigned char>(peek());
      ++at_;
      if (byte == '"') {
        return out;
      }
      if (byte < 0x20) {
        throw std::runtime_error("bare control character");
      }
      if (byte == '\\') {
        const char e = peek();
        ++at_;
        switch (e) {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case '/':
            out += '/';
            break;
          case 'b':
            out += '\b';
            break;
          case 'f':
            out += '\f';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'u': {
            std::uint32_t cp = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = peek();
              ++at_;
              cp <<= 4U;
              if (h >= '0' && h <= '9') {
                cp |= static_cast<std::uint32_t>(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                cp |= static_cast<std::uint32_t>(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                cp |= static_cast<std::uint32_t>(h - 'A' + 10);
              } else {
                throw std::runtime_error("bad hex digit");
              }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) {
              throw std::runtime_error("lone surrogate");
            }
            append_utf8(out, cp);
            break;
          }
          default:
            throw std::runtime_error("bad escape");
        }
        continue;
      }
      if (byte < 0x80) {
        out += static_cast<char>(byte);
        continue;
      }
      // A multi-byte sequence: lead, then continuation bytes, no overlong forms,
      // no surrogates, nothing above U+10FFFF.
      std::size_t length = 0;
      std::uint32_t cp = 0;
      if (byte >= 0xC2 && byte <= 0xDF) {
        length = 2;
        cp = byte & 0x1FU;
      } else if (byte >= 0xE0 && byte <= 0xEF) {
        length = 3;
        cp = byte & 0x0FU;
      } else if (byte >= 0xF0 && byte <= 0xF4) {
        length = 4;
        cp = byte & 0x07U;
      } else {
        throw std::runtime_error("invalid UTF-8 lead byte");
      }
      out += static_cast<char>(byte);
      for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(peek());
        if ((next & 0xC0U) != 0x80U) {
          throw std::runtime_error("invalid UTF-8 continuation");
        }
        ++at_;
        cp = (cp << 6U) | (next & 0x3FU);
        out += static_cast<char>(next);
      }
      const std::uint32_t minimum = length == 2 ? 0x80U : (length == 3 ? 0x800U : 0x10000U);
      if (cp < minimum || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        throw std::runtime_error("invalid UTF-8 scalar");
      }
    }
  }

  std::string_view text_;
  std::size_t at_ = 0;
};

}  // namespace loadforge::reporters::testing

#endif
