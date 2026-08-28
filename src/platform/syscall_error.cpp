// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/syscall_error.hpp"

#include <array>
#include <cstring>
#include <string>

namespace loadforge::platform {

std::string describe(const SyscallError& error) {
  std::array<char, 256> buffer{};

  // The GNU strerror_r returns a pointer that may or may not be `buffer`: it is
  // not required to copy into it. Reading the return value rather than the
  // buffer is the only correct way to get the result, and getting that backwards
  // is a classic source of empty error messages.
  //
  // There is deliberately no null or empty check on what comes back. GNU
  // strerror_r always returns a valid string -- an unrecognised number renders
  // as "Unknown error 999999" -- so a check would guard a condition that cannot
  // occur, and docs/IMPLEMENTATION.md §2.6 is explicit that such a guard is not
  // defence but untested code awaiting a chance to be wrong. This was not a
  // theoretical call: the branch existed, the coverage gate proved no test could
  // reach it, and deleting it was the third rung of that ladder rather than the
  // fourth.
  const char* description = ::strerror_r(error.number, buffer.data(), buffer.size());

  // The number is always included, not only when the description is missing.
  // It survives translation and truncation, it is what a user searches for, and
  // it is what makes a bug report from an unfamiliar machine actionable.
  const std::string number = std::to_string(error.number);

  std::string message;
  message.reserve(error.call.size() + error.subject.size() + buffer.size() + number.size());
  message.append(error.call);
  if (!error.subject.empty()) {
    message.append("(").append(error.subject).append(")");
  }
  message.append(": ").append(description).append(" (errno ").append(number).append(")");
  return message;
}

}  // namespace loadforge::platform
