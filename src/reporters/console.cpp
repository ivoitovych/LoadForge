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
#include "verification/isolation.hpp"
#include "verification/record.hpp"
#include "verification/record/render.hpp"

namespace loadforge::reporters {
namespace {

constexpr double kNanosecondsPerSecond = 1e9;

/// A finding's lines, each indented under its worker.
void print_record(std::ostream& out, const verification::ErrorRecord& record) {
  std::istringstream lines(verification::render(record));
  for (std::string line; std::getline(lines, line);) {
    out << "      " << line << "\n";
  }
}

/// The ladder's answers in words, then what they are consistent with: never
/// a verdict on the hardware (verification.md §7).
void print_isolation(std::ostream& out, const verification::IsolationReport& isolation) {
  out << "      isolation: same core " << verification::describe(isolation.answers.same_core)
      << ", another core " << verification::describe(isolation.answers.other_core)
      << ", single thread " << verification::describe(isolation.answers.single_thread) << "\n"
      << "      " << verification::describe(isolation.finding) << " (confidence "
      << verification::describe(isolation.confidence) << ")\n";
}

/// What a verifying worker checked, and every error record it sent. The tally
/// is told in the words of the contracts: units "checked" over "sampled"
/// blocks, never "correct" -- an oracle may compare a sample of a unit
/// (oracle.hpp), and a run that sampled has not shown the rest right.
void print_verification(std::ostream& out, const controller::WorkerReport& worker) {
  const verification::Tally& tally = worker.tally;
  if (!worker.tally_seen) {
    out << "    verification: this worker sent no tally; nothing it checked is known\n";
  } else {
    out << "    verification: " << tally.units_computed << " unit(s) computed; the oracle checked "
        << tally.oracle_checked << " of them over " << tally.oracle_blocks
        << " sampled block(s) and found " << tally.oracle_failed << " wrong, "
        << tally.oracle_not_performed << " not checked; the input bracket covered "
        << tally.integrity_checked << " unit(s) and found " << tally.integrity_failed << " wrong\n";
  }
  out << "    findings: " << worker.wrong_results << " wrong result(s), " << worker.not_performed
      << " check(s) not performed";
  if (worker.findings_dropped != 0) {
    out << " (" << worker.findings_dropped << " more not kept)";
  }
  out << "\n";
  for (const controller::FindingReport& finding : worker.findings) {
    print_record(out, finding.record);
  }
}

std::string seconds(std::int64_t nanoseconds) {
  std::ostringstream text;
  text << std::fixed << std::setprecision(3)
       << static_cast<double>(nanoseconds) / kNanosecondsPerSecond << " s";
  return text.str();
}

}  // namespace

void ConsoleReporter::run_starting(const controller::RunPlan& plan) {
  *out_ << "loadforge " << core::kVersion << ": " << config::describe(plan.mode) << " for "
        << core::to_string(plan.duration) << " with " << plan.workers << " worker(s)"
        << (plan.workloads.empty() ? ", null workload" : "") << "\n"
        << "  executable: " << plan.executable << "\n"
        << "  topology: " << plan.topology << "\n";
  for (const std::string& workload : plan.workloads) {
    *out_ << "  workload " << workload << "\n";
  }
  if (!plan.environment.empty()) {
    *out_ << "  " << plan.environment << "\n";
  }
  if (!plan.capabilities.empty()) {
    *out_ << "  capabilities:\n" << plan.capabilities;
    if (plan.capabilities.back() != '\n') {
      *out_ << "\n";
    }
  }
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
    if (worker.work.has_value()) {
      print_verification(*out_, worker);
    }
  }
  out_->flush();
}

void ConsoleReporter::isolation_finished(const controller::RunReport& report) {
  for (const controller::WorkerReport& worker : report.workers) {
    for (const controller::FindingReport& finding : worker.findings) {
      if (finding.isolation.has_value()) {
        *out_ << "  worker " << worker.slot << " isolation of "
              << verification::kind_name(finding.record) << ":\n";
        print_isolation(*out_, *finding.isolation);
      } else if (verification::is_wrong_result(finding.record)) {
        *out_ << "  worker " << worker.slot << " " << verification::kind_name(finding.record)
              << " not isolated: " << finding.not_isolated << "\n";
      }
    }
  }
  out_->flush();
}

}  // namespace loadforge::reporters
