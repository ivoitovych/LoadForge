// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/selftest.hpp"

#include <array>
#include <string_view>

#include "core/byte_size.hpp"
#include "core/duration.hpp"

namespace loadforge::selftest {
namespace {

constexpr std::array<std::string_view, 16> kCorpus{
    "500ms", "30s", "20m",  "5h",     "0s", "  20m  ", "2562047788015h", "1h30m",
    "8GiB",  "70%", "1024", "512MiB", "0%", "8GB",     "-1GiB",          "",
};

}  // namespace

// FNV-1a. Chosen because it is exactly specified, has no tuning parameters and
// no endianness dependence at this width, so any difference between two runs is
// a difference in the inputs rather than in the hash.
std::uint64_t fnv1a(std::string_view text, std::uint64_t basis) noexcept {
  std::uint64_t hash = basis;
  for (const char c : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  return hash;
}

std::uint64_t core_digest() {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const std::string_view input : kCorpus) {
    hash = fnv1a(input, hash);

    // Explicit if/else rather than a ternary: the ternary form produced a
    // never-entered branch pair in the coverage data, and an unreachable branch
    // that nobody can explain is exactly what the 100% gate exists to surface.
    const auto duration = core::parse_duration(input);
    if (duration) {
      hash = fnv1a(core::to_string(duration.value()), hash);
    } else {
      hash = fnv1a(core::describe(duration.error()), hash);
    }

    const auto size = core::parse_byte_size(input);
    if (size) {
      hash = fnv1a(core::to_string(size.value()), hash);
    } else {
      hash = fnv1a(core::describe(size.error()), hash);
    }
  }
  return hash;
}

std::string core_digest_hex() {
  constexpr std::string_view kDigits = "0123456789abcdef";
  const std::uint64_t hash = core_digest();
  std::string out(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    out[15 - i] = kDigits[(hash >> (i * 4)) & 0xF];
  }
  return out;
}

}  // namespace loadforge::selftest
