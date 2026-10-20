// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_REPORTERS_CONSOLE_HPP
#define LOADFORGE_REPORTERS_CONSOLE_HPP

#include <cstdint>
#include <iosfwd>
#include <string_view>

#include "controller/report.hpp"

namespace loadforge::reporters {

/// The controller's story for a person at a terminal: a banner saying what
/// will run and which limits are actually enforced, one line per event with
/// the time into the run, and a summary naming each worker's fate.
class ConsoleReporter final : public controller::Reporter {
 public:
  explicit ConsoleReporter(std::ostream& out) noexcept : out_(&out) {}

  void run_starting(const controller::RunPlan& plan) override;
  void event(std::int64_t elapsed_ns, std::string_view line) override;
  void run_finished(const controller::RunReport& report) override;

 private:
  std::ostream* out_;
};

}  // namespace loadforge::reporters

#endif
