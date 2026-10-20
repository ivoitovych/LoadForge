// SPDX-License-Identifier: GPL-3.0-or-later
#include "reporters/console.hpp"

#include <cstdint>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include "config/config.hpp"
#include "controller/report.hpp"
#include "controller/supervision.hpp"
#include "core/duration.hpp"
#include "core/version.hpp"

namespace loadforge::reporters {
namespace {

constexpr double kNanosecondsPerSecond = 1e9;

std::string seconds(std::int64_t nanoseconds) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(3)
       << static_cast<double>(nanoseconds) / kNanosecondsPerSecond << " s";
  return text.str();
}

}  // namespace

void ConsoleReporter::run_starting(const controller::RunPlan& plan) {
  *out_ << "loadforge " << core::kVersion << ": " << config::describe(plan.mode) << " for "
        << core::to_string(plan.duration) << " with " << plan.workers
        << " worker(s), null workload\n"
        << "  executable: " << plan.executable << "\n";
  for (const controller::LimitStatus& limit : plan.limits) {
    *out_ << "  limit " << limit.name << ": " << (limit.enforced ? "enforced" : "NOT ENFORCED")
          << " (" << limit.note << ")\n";
  }
  out_->flush();
}

void ConsoleReporter::event(std::int64_t elapsed_ns, std::string_view line) {
  *out_ << "[" << seconds(elapsed_ns) << "] " << line << "\n";
  out_->flush();
}

void ConsoleReporter::run_finished(const controller::RunReport& report) {
  *out_ << "run ended after " << seconds(report.elapsed_ns) << ": "
        << controller::clean_count(report) << " of " << report.workers.size()
        << " worker(s) completed\n";
  for (const controller::WorkerReport& worker : report.workers) {
    *out_ << "  worker " << worker.slot << " (pid " << worker.pid
          << "): " << controller::describe(worker.fate) << "; " << worker.termination
          << "; progress " << worker.progress << "; " << worker.stop << "\n";
    for (const std::string& record : worker.records) {
      *out_ << "    " << record << "\n";
    }
  }
  out_->flush();
}

}  // namespace loadforge::reporters
