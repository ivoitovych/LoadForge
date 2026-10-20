// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/fpenv/real_control.hpp"

#include <cstdint>

#include "platform/fpenv/registers.hpp"

namespace loadforge::fpenv {
namespace {

#if defined(__x86_64__)

std::uint32_t read_register() noexcept {
  std::uint32_t value = 0;  // NOLINT(misc-const-correctness): written by the instruction
  asm volatile("stmxcsr %0" : "=m"(value));
  return value;
}

void write_register(std::uint32_t value) noexcept {
  asm volatile("ldmxcsr %0" : : "m"(value) : "memory");
}

FpState decode(std::uint32_t value) noexcept { return decode_mxcsr(value); }
std::uint32_t encode(std::uint32_t value, const FpState& state) noexcept {
  return encode_mxcsr(value, state);
}

#elif defined(__aarch64__)

std::uint64_t read_register() noexcept {
  std::uint64_t value = 0;
  asm volatile("mrs %0, fpcr" : "=r"(value));
  return value;
}

void write_register(std::uint64_t value) noexcept {
  asm volatile("msr fpcr, %0\n\tisb" : : "r"(value) : "memory");
}

FpState decode(std::uint64_t value) noexcept { return decode_fpcr(value); }
std::uint64_t encode(std::uint64_t value, const FpState& state) noexcept {
  return encode_fpcr(value, state);
}

#else
#error "platform/fpenv: no floating-point control register access for this architecture"
#endif

}  // namespace

FpState RealFpControl::read() const { return decode(read_register()); }

void RealFpControl::write(const FpState& state) { write_register(encode(read_register(), state)); }

}  // namespace loadforge::fpenv
