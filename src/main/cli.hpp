// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_MAIN_CLI_HPP
#define LOADFORGE_MAIN_CLI_HPP

#include <iosfwd>
#include <span>
#include <string_view>
#include <vector>

namespace loadforge::cli {

inline constexpr std::string_view kVersion = "0.0.0-m0";

/// Convert a raw argv span into arguments, dropping argv[0].
///
/// Lives here rather than in main() so the argc == 0 case is testable. POSIX
/// permits execve with an empty argv, and an unguarded subspan(1) on an empty
/// span is undefined behaviour -- which this project refuses to rely on
/// anywhere. A defensive branch that no test can reach is a branch nobody has
/// shown to work.
[[nodiscard]] std::vector<std::string_view> collect_args(std::span<char* const> argv);

/// Run the command line. Separated from main() so every path is testable
/// in-process; main() is a shim covered by a smoke test that runs the binary.
///
/// `args` excludes argv[0]. Returns the process exit code.
[[nodiscard]] int run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

}  // namespace loadforge::cli

#endif
