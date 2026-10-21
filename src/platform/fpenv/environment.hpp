// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_PLATFORM_FPENV_ENVIRONMENT_HPP
#define LOADFORGE_PLATFORM_FPENV_ENVIRONMENT_HPP

// FpEnvironment (determinism.md §3): establish the required state at startup,
// assert it again after any point where a third party could have changed it.
// Both report a failure as a configuration error -- a machine whose flush-to-zero
// bit a library set is not a faulty machine -- naming every departure.

#include <string>

#include "core/result.hpp"
#include "platform/fpenv/fpenv.hpp"

namespace loadforge::fpenv {

/// Reads the calling thread's state through `control` and checks it.
[[nodiscard]] core::Result<core::Ok, FpError> assert_environment(const FpControl& control);

/// Writes the required state for the architecture `control` describes (the
/// flags its read() reports are the flags it has), then asserts it: a register
/// that did not take the write is reported as the departure it is.
[[nodiscard]] core::Result<core::Ok, FpError> establish_environment(FpControl& control);

/// The startup message for a failed assertion: every violation, once, and what
/// the required state is.
[[nodiscard]] std::string describe(const FpError& error);

}  // namespace loadforge::fpenv

#endif
