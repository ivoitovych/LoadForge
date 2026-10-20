// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_FPENV_FPENV_HPP
#define LOADFORGE_PLATFORM_FPENV_FPENV_HPP

// CONTRACT (ADR 0004). The floating-point environment every verified result
// depends on (docs/determinism.md §3, PLAN F15). Frozen: changed only by a
// contract change, never inside a stream.
//
//   Provider of FpControl (the real MXCSR / FPCR access) and of check():
//     the numerics stream, in src/platform/fpenv/.
//   Consumers: the worker and controller at startup and after any library
//     load (establish and assert), the run fingerprint (record the state).
//
// The rule this exists for: rounding to nearest-even, no flush-to-zero, no
// denormals-are-zero, no default-NaN -- established at startup, re-asserted
// after any point where a third party could have changed it, and recorded. An
// assertion failure is a CONFIGURATION error reported at startup, never a
// verification failure: a machine whose FTZ bit was set by a library is not a
// faulty machine.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.hpp"

namespace loadforge::fpenv {

/// The IEEE 754 rounding direction in force.
enum class Rounding : std::uint8_t { kToNearestEven, kTowardZero, kUpward, kDownward };

[[nodiscard]] constexpr std::string_view describe(Rounding rounding) noexcept {
  switch (rounding) {
    case Rounding::kToNearestEven:
      return "to nearest, ties to even";
    case Rounding::kTowardZero:
      return "toward zero";
    case Rounding::kUpward:
      return "upward";
    case Rounding::kDownward:
      return "downward";
  }
  return "unrecognised rounding";
}

/// The environment as the hardware holds it. A flag the running architecture
/// does not have is absent, not false: x86-64 has no default-NaN control and
/// AArch64 has no separate denormals-are-zero (FPCR.FZ covers inputs and
/// outputs), and a record that said "false" for either would be inventing a
/// fact about the machine.
///
///   flush_to_zero       x86 MXCSR.FTZ, or AArch64 FPCR.FZ.
///   denormals_are_zero  x86 MXCSR.DAZ. Absent on AArch64.
///   default_nan         AArch64 FPCR.DN. Absent on x86-64.
struct FpState {
  Rounding rounding = Rounding::kToNearestEven;
  bool flush_to_zero = false;
  std::optional<bool> denormals_are_zero;
  std::optional<bool> default_nan;

  [[nodiscard]] friend bool operator==(const FpState&, const FpState&) = default;
};

/// Reads and writes the calling thread's floating-point control state.
///
/// The control registers are per thread: a state written here holds for the
/// calling thread only, and a thread the worker starts inherits the state of
/// the thread that started it (that inheritance is a property of the platform
/// the provider must confirm against the real kernel, F21). So the worker
/// establishes the state before it starts any thread, and the executor's
/// threads assert it on start.
///
/// read() reports the state as the hardware holds it. write() sets every
/// property the state names; a property the state leaves absent is left as it
/// is. Neither can fail: they are register accesses, and a provider whose
/// platform could refuse one must report that through check(), by the read
/// that follows not matching.
///
/// Thread-safety: one instance may be used from any number of threads; each
/// call acts on the calling thread's registers.
class FpControl {
 public:
  FpControl() = default;
  FpControl(const FpControl&) = delete;
  FpControl(FpControl&&) = delete;
  FpControl& operator=(const FpControl&) = delete;
  FpControl& operator=(FpControl&&) = delete;
  virtual ~FpControl() = default;

  [[nodiscard]] virtual FpState read() const = 0;
  virtual void write(const FpState& state) = 0;
};

/// The state determinism.md requires, for an architecture that has the given
/// flags: nearest-even, and every flag that exists, off.
[[nodiscard]] constexpr FpState required_state(bool has_denormals_are_zero,
                                               bool has_default_nan) noexcept {
  FpState state;
  if (has_denormals_are_zero) {
    state.denormals_are_zero = false;
  }
  if (has_default_nan) {
    state.default_nan = false;
  }
  return state;
}

/// Each way a state departs from the required one, in a fixed order:
/// rounding, flush_to_zero, denormals_are_zero, default_nan. Each entry names
/// the property and the value found, for the startup message.
struct FpViolation {
  std::string property;  ///< "rounding", "flush_to_zero", "denormals_are_zero", "default_nan".
  std::string found;     ///< "upward", "on".

  [[nodiscard]] friend bool operator==(const FpViolation&, const FpViolation&) = default;
};

struct FpError {
  std::vector<FpViolation> violations;  ///< Never empty.

  [[nodiscard]] friend bool operator==(const FpError&, const FpError&) = default;
};

/// Whether `state` is the required one. Provided by the numerics stream.
///
/// Ok when rounding is nearest-even and no flag that is present is on. An
/// absent flag is never a violation: it does not exist on this architecture.
/// Otherwise every violation, in the order FpViolation names, none omitted --
/// a message that reported only the first would send the user round twice.
[[nodiscard]] core::Result<core::Ok, FpError> check(const FpState& state);

}  // namespace loadforge::fpenv

#endif
