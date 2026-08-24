// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/cli.hpp"

#include <ostream>

namespace loadforge::cli {
namespace {

constexpr int kOk = 0;
constexpr int kUsageError = 2;

void print_usage(std::ostream& out) {
  out << "loadforge " << kVersion << "\n"
      << "\n"
      << "Usage:\n"
      << "  loadforge --version\n"
      << "  loadforge --help\n"
      << "\n"
      << "No workload commands yet: this is the M0 foundation build.\n"
      << "See docs/PLAN.md for what lands when.\n";
}

}  // namespace

int run(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  if (args.empty()) {
    print_usage(out);
    return kOk;
  }
  if (args.size() > 1) {
    err << "loadforge: expected a single argument, got " << args.size() << "\n";
    return kUsageError;
  }

  const std::string_view command = args.front();
  if (command == "--version" || command == "-V") {
    out << kVersion << "\n";
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
