// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_CLI_HPP
#define LOADFORGE_MAIN_CLI_HPP

#include <iosfwd>
#include <span>
#include <string_view>

namespace loadforge::cli {

inline constexpr std::string_view kVersion = "0.0.0-m0";

/// Run the command line. Separated from main() so every path is testable
/// in-process; main() is a shim covered by a smoke test that runs the binary.
///
/// `args` excludes argv[0]. Returns the process exit code.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace loadforge::cli

#endif
