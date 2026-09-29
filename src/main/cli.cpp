// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <ostream>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "core/duration.hpp"
#include "main/selftest.hpp"

namespace loadforge::cli {
namespace {

constexpr int kOk = 0;
constexpr int kConfigRefused = 1;
constexpr int kUsageError = 2;

void print_usage(std::ostream& out) {
  out << "loadforge " << kVersion << "\n"
      << "\n"
      << "Usage:\n"
      << "  loadforge --version\n"
      << "  loadforge --help\n"
      << "  loadforge --check-config FILE   parse and validate a configuration\n"
      << "\n"
      << "No workload commands yet: the controller lands with M1.\n"
      << "See docs/PLAN.md for what lands when.\n";
}

/// Reads a configuration and says whether it would be accepted, and why not
/// if not. Exit 1 on a refusal so a script can rely on it, and the message on
/// stderr where a script expects a diagnostic.
int check_config(std::string_view path, std::ostream& out, std::ostream& err) {
  const auto loaded = config::load(std::string{path});
  if (!loaded.has_value()) {
    err << "loadforge: " << config::describe(loaded.error()) << "\n";
    return kConfigRefused;
  }
  const config::Config& config = loaded.value();
  out << "ok: " << path << ": " << config::describe(config.run.mode) << " for "
      << core::to_string(config.run.duration) << ", " << config.workloads.size()
      << " workload(s)\n";
  return kOk;
}

}  // namespace

std::vector<std::string_view> collect_args(std::span<char* const> argv) {
  if (argv.empty()) {
    return {};
  }
  const std::span<char* const> without_program = argv.subspan(1);
  std::vector<std::string_view> args;
  args.reserve(without_program.size());
  for (const char* arg : without_program) {
    args.emplace_back(arg);
  }
  return args;
}

// bugprone-easily-swappable-parameters fires on the adjacent ostream
// references. (out, err) is the conventional shape for a testable CLI entry
// point, and contorting it to satisfy the check would make the code worse. This
// is suppressed here rather than project-wide so the check can still catch a
// genuinely dangerous pair elsewhere -- particularly in the platform and
// control code still to come.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    print_usage(out);
    return kOk;
  }
  const std::string_view command = args.front();
  if (command == "--check-config") {
    if (args.size() != 2) {
      err << "loadforge: --check-config takes exactly one FILE\n";
      return kUsageError;
    }
    return check_config(args[1], out, err);
  }
  if (args.size() > 1) {
    err << "loadforge: expected a single argument, got " << args.size() << "\n";
    return kUsageError;
  }

  if (command == "--version" || command == "-V") {
    out << kVersion << "\n";
    return kOk;
  }
  if (command == "--selftest-digest") {
    // Cross-architecture determinism check: the scalar build must produce an
    // identical digest on x86-64 and ARM64. See docs/determinism.md.
    out << selftest::core_digest_hex() << "\n";
    return kOk;
  }
  if (command == "--help" || command == "-h") {
    print_usage(out);
    return kOk;
  }

  err << "loadforge: unknown argument '" << command << "'\n"
      << "Try 'loadforge --help'.\n";
  return kUsageError;
}

}  // namespace loadforge::cli
