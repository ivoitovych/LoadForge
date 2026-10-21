// SPDX-License-Identifier: GPL-3.0-or-later
#include "verification/record/format.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace loadforge::verification {

namespace {

// Long enough for any double in shortest form ("-1.7976931348623157e+308" is
// 24 characters) and any 64-bit value in hexadecimal (16), so to_chars cannot
// run out of room and its error code has nothing to report.
constexpr std::size_t kNumberRoom = 32;

}  // namespace

std::string shortest(double value) {
  std::array<char, kNumberRoom> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return {buffer.data(), written.ptr};
}

std::string hex(std::uint64_t value) {
  std::array<char, kNumberRoom> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, 16);
  return "0x" + std::string(buffer.data(), written.ptr);
}

std::string escaped(std::string_view text) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(text.size());
  for (const char character : text) {
    const auto byte = static_cast<unsigned char>(character);
    if (character == '\\') {
      out += "\\\\";
    } else if (byte >= 0x20U && byte < 0x7FU) {
      out += character;
    } else {
      out += "\\x";
      out += kDigits[byte >> 4U];
      out += kDigits[byte & 0x0FU];
    }
  }
  return out;
}

}  // namespace loadforge::verification
