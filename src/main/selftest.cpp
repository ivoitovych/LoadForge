// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/selftest.hpp"

#include <array>
#include <string_view>

#include "core/byte_size.hpp"
#include "core/duration.hpp"

namespace loadforge::selftest {
namespace {

// FNV-1a. Chosen because it is exactly specified, has no tuning parameters and
// no endianness dependence at this width, so any difference between two runs is
// a difference in the inputs rather than in the hash.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

constexpr std::uint64_t fold(std::uint64_t hash, std::string_view text) noexcept {
  for (const char c : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= kFnvPrime;
  }
  return hash;
}

constexpr std::array<std::string_view, 16> kCorpus{
    "500ms", "30s", "20m",  "5h",     "0s", "  20m  ", "2562047788015h", "1h30m",
    "8GiB",  "70%", "1024", "512MiB", "0%", "8GB",     "-1GiB",          "",
};

}  // namespace

std::uint64_t core_digest() noexcept {
  std::uint64_t hash = kFnvOffset;
  for (const std::string_view input : kCorpus) {
    hash = fold(hash, input);

    const auto duration = core::parse_duration(input);
    hash = duration ? fold(hash, core::to_string(duration.value()))
                    : fold(hash, core::describe(duration.error()));

    const auto size = core::parse_byte_size(input);
    hash =
        size ? fold(hash, core::to_string(size.value())) : fold(hash, core::describe(size.error()));
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
