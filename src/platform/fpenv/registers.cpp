// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/fpenv/registers.hpp"

#include <cstdint>

namespace loadforge::fpenv {
namespace {

constexpr std::uint32_t kRoundingMask = 3U;

// Four two-bit entries packed into one byte, entry i in bits 2i..2i+1: a lookup
// by shift, with no array to index. Written out, each reads "input -> output".
//
// Rounding's values are nearest 0, toward zero 1, upward 2, downward 3.
//   MXCSR.RC  nearest 0, down 1, up 2, zero 3:   enum -> field   0->0 1->3 2->2 3->1
//                                                field -> enum   0->0 1->3 2->2 3->1
//   FPCR.RMode nearest 0, up 1, down 2, zero 3:  enum -> field   0->0 1->3 2->1 3->2
//                                                field -> enum   0->0 1->2 2->3 3->1
constexpr std::uint32_t kMxcsrEncode = 0b01'10'11'00;
constexpr std::uint32_t kMxcsrDecode = 0b01'10'11'00;
constexpr std::uint32_t kFpcrEncode = 0b10'01'11'00;
constexpr std::uint32_t kFpcrDecode = 0b01'11'10'00;

// Entry `index & 3` of a packed table. A Rounding outside the enumeration is
// masked into range: a caller's bug lands on a defined field, not past a table.
[[nodiscard]] constexpr std::uint32_t entry(std::uint32_t table, std::uint64_t index) noexcept {
  return (table >> (2U * static_cast<std::uint32_t>(index & kRoundingMask))) & kRoundingMask;
}

[[nodiscard]] constexpr Rounding rounding_of(std::uint32_t value) noexcept {
  return static_cast<Rounding>(value);
}

}  // namespace

FpState decode_mxcsr(std::uint32_t mxcsr) noexcept {
  FpState state;
  state.rounding = rounding_of(entry(kMxcsrDecode, mxcsr >> kMxcsrRoundingShift));
  state.flush_to_zero = (mxcsr & kMxcsrFtz) != 0U;
  state.denormals_are_zero = (mxcsr & kMxcsrDaz) != 0U;
  return state;
}

std::uint32_t encode_mxcsr(std::uint32_t mxcsr, const FpState& state) noexcept {
  mxcsr &= ~(kRoundingMask << kMxcsrRoundingShift);
  mxcsr |= entry(kMxcsrEncode, static_cast<std::uint8_t>(state.rounding)) << kMxcsrRoundingShift;
  mxcsr = state.flush_to_zero ? (mxcsr | kMxcsrFtz) : (mxcsr & ~kMxcsrFtz);
  if (state.denormals_are_zero.has_value()) {
    mxcsr = *state.denormals_are_zero ? (mxcsr | kMxcsrDaz) : (mxcsr & ~kMxcsrDaz);
  }
  return mxcsr;
}

FpState decode_fpcr(std::uint64_t fpcr) noexcept {
  FpState state;
  state.rounding = rounding_of(entry(kFpcrDecode, fpcr >> kFpcrRoundingShift));
  state.flush_to_zero = (fpcr & kFpcrFz) != 0U;
  state.default_nan = (fpcr & kFpcrDn) != 0U;
  return state;
}

std::uint64_t encode_fpcr(std::uint64_t fpcr, const FpState& state) noexcept {
  fpcr &= ~(std::uint64_t{kRoundingMask} << kFpcrRoundingShift);
  fpcr |= std::uint64_t{entry(kFpcrEncode, static_cast<std::uint8_t>(state.rounding))}
          << kFpcrRoundingShift;
  fpcr = state.flush_to_zero ? (fpcr | kFpcrFz) : (fpcr & ~kFpcrFz);
  if (state.default_nan.has_value()) {
    fpcr = *state.default_nan ? (fpcr | kFpcrDn) : (fpcr & ~kFpcrDn);
  }
  return fpcr;
}

}  // namespace loadforge::fpenv
