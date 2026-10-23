// SPDX-License-Identifier: GPL-3.0-or-later
#include "main/selftest_command.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "controller/plan.hpp"
#include "core/byte_size.hpp"
#include "core/parse_number.hpp"
#include "core/version.hpp"
#include "main/outcome.hpp"
#include "main/selftest.hpp"
#include "platform/syscalls.hpp"
#include "verification/oracle.hpp"
#include "verification/record.hpp"
#include "verification/record/render.hpp"
#include "verification/runner.hpp"
#include "worker/entry.hpp"
#include "worker/scheduler.hpp"
#include "worker/verified_run.hpp"
#include "workloads/registry/registry.hpp"

namespace loadforge::cli {
namespace {

constexpr int kOk = 0;
constexpr int kRefused = 1;
constexpr int kUsage = 2;

/// One line of the report.
///
///   name     what was checked.
///   passed   whether it held.
///   detail   what was found, on the line.
///   extra    what `--verbose` adds under it; empty adds nothing.
struct Check {
  std::string name;
  bool passed = false;
  std::string detail;
  std::string extra;
};

std::string hex16(std::uint64_t value) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string text(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    text[15 - i] = kDigits[(value >> (i * 4)) & 0xFU];
  }
  return text;
}

/// A digest compared with the one expected: the line says both when they differ.
Check digest_check(std::string name, std::string what, std::uint64_t got, std::uint64_t expected) {
  Check check;
  check.name = std::move(name);
  check.passed = got == expected;
  if (check.passed) {
    check.detail = what + " " + hex16(got) + " is the one this build expects";
  } else {
    check.detail = what + " differ: expected " + hex16(expected) + ", computed " + hex16(got);
  }
  return check;
}

/// The two halves of the golden digest. The second is computed from the EXPECTED first half, so
/// that a corpus mismatch cannot also be reported as a kernel mismatch.
std::vector<Check> golden_checks(const ExpectedDigests& expected) {
  std::vector<Check> checks;
  checks.push_back(digest_check("golden corpus", "the configuration-parsing digest",
                                selftest::corpus_digest(), expected.corpus));
  checks.push_back(digest_check("golden kernels", "the scalar workload-kernel digest",
                                selftest::workloads_digest(expected.corpus), expected.full));
  return checks;
}

/// The record's first two lines on one line: its kind, and what it checked.
std::string summary_of(const verification::ErrorRecord& record) {
  const std::string text = verification::render(record);
  const std::size_t end_of_first = text.find('\n');
  const std::size_t second = text.find_first_not_of(' ', end_of_first + 1);
  return text.substr(0, end_of_first) + "; " +
         text.substr(second, text.find('\n', second) - second);
}

std::string tally_line(const verification::Tally& tally) {
  return std::to_string(tally.units_computed) + " unit(s) computed, " +
         std::to_string(tally.oracle_checked) + " checked by the oracle (" +
         std::to_string(tally.oracle_blocks) + " block(s) compared), " +
         std::to_string(tally.integrity_checked) + " bracketed by the input-integrity check";
}

struct Outcome {
  verification::Tally tally;
  std::vector<verification::ErrorRecord> records;
};

/// Units through the runner the stress run uses, then its closing statement of what was not
/// checked; or the runner's refusal in words.
core::Result<Outcome, std::string> run_units(verification::SliceRunner& runner) {
  auto slice = runner.run_slice(verification::SliceRequest{kSelftestUnits});
  if (!slice.has_value()) {
    return slice.error().detail;
  }
  auto last = runner.finish();
  if (!last.has_value()) {
    return last.error().detail;
  }
  Outcome outcome;
  outcome.tally = runner.total();
  outcome.records = std::move(slice).value().records;
  for (auto& record : std::move(last).value().records) {
    outcome.records.push_back(std::move(record));
  }
  return outcome;
}

/// The check of one workload on one path.
Check oracle_check(platform::Syscalls& syscalls, const worker::Services& services,
                   const SelftestOptions& options, std::string_view workload,
                   simd::IsaPath path) {
  Check check;
  check.name = std::string{workload} + " [" + std::string{simd::describe(path)} + "]";
  worker::WorkloadArgs args;
  args.workload = std::string{workload};
  args.seed = options.seed;
  args.footprint_bytes = options.footprint_bytes;
  args.isa = path;
  args.lock = verification::LockPolicy::kNone;
  auto prepared = worker::prepare(syscalls, args, {}, 0, services);
  if (!prepared.has_value()) {
    check.detail = "could not be set up: " + worker::describe(prepared.error());
    return check;
  }
  auto ran = run_units(*prepared.value().runner);
  if (!ran.has_value()) {
    check.detail = "the runner refused: " + ran.error();
    return check;
  }
  const Outcome& outcome = ran.value();
  check.passed = outcome.records.empty();
  check.extra = tally_line(outcome.tally) + "\n";
  if (check.passed) {
    check.detail = tally_line(outcome.tally);
    return check;
  }
  check.detail = std::to_string(outcome.records.size()) +
                 " finding(s), the first: " + summary_of(outcome.records.front());
  for (const verification::ErrorRecord& record : outcome.records) {
    check.extra += verification::render(record);
  }
  return check;
}

std::vector<Check> oracle_checks(platform::Syscalls& syscalls, const worker::Services& services,
                                 const SelftestOptions& options) {
  std::vector<Check> checks;
  const simd::IsaSet supported = services.isa->supported();
  for (const std::string& name : services.workloads->names()) {
    const auto described = services.workloads->descriptor(name);
    if (!described.has_value()) {
      Check check;
      check.name = name;
      check.detail = "the registry would not describe it: " +
                     std::string{workloads::describe(described.error().fault)} + ": " +
                     described.error().detail;
      checks.push_back(std::move(check));
      continue;
    }
    const SelftestPaths paths = selftest_paths(options.isa, supported, simd::compiled_paths(),
                                               described.value().paths);
    for (const simd::IsaPath path : paths.run) {
      Check check = oracle_check(syscalls, services, options, name, path);
      if (!paths.note.empty()) {
        check.detail += " (" + paths.note + ")";
      }
      checks.push_back(std::move(check));
    }
  }
  return checks;
}

std::string set_of(simd::IsaSet paths) {
  std::string text;
  for (const simd::IsaPath path : simd::kPreference) {
    if (paths.contains(path)) {
      if (!text.empty()) {
      text += ' ';
    }
    text += simd::describe(path);
    }
  }
  return text;
}

void print_header(std::ostream& out, const worker::Services& services,
                  const SelftestOptions& options) {
  out << "loadforge selftest " << core::kVersion << ": " << kSelftestUnits
      << " unit(s) of each workload at a footprint of " << options.footprint_bytes
      << " bytes, seed " << options.seed << "\n";
  if (options.verbose) {
    out << "  paths this build holds: " << set_of(simd::compiled_paths()) << "\n"
        << "  paths this CPU can run: " << set_of(services.isa->supported()) << "\n";
  }
}

}  // namespace

SelftestPaths selftest_paths(std::optional<simd::IsaPath> requested, simd::IsaSet supported,
                             simd::IsaSet compiled, simd::IsaSet workload_paths) {
  SelftestPaths paths;
  auto chosen = controller::choose_isa(requested, supported, compiled, workload_paths);
  if (chosen.has_value()) {
    paths.run.push_back(chosen.value());
  } else {
    paths.note = chosen.error().detail;
  }
  if (paths.run.empty() || paths.run.front() != simd::IsaPath::kScalar) {
    paths.run.push_back(simd::IsaPath::kScalar);
  }
  return paths;
}

int run_selftest(platform::Syscalls& syscalls, const worker::Services& services,
                 const SelftestOptions& options, const ExpectedDigests& expected,
                 std::ostream& out, std::ostream& err) {
  if (options.isa.has_value()) {
    // Used or refused, never replaced (dispatch.hpp): a selftest of a path that was not asked
    // for would say the asked one was sound.
    const auto path = simd::select_path(options.isa, services.isa->supported(),
                                        simd::compiled_paths());
    if (!path.has_value()) {
      err << "loadforge selftest: --isa " << simd::describe(*options.isa)
          << ": " << path.error().detail << "\n";
      return kRefused;
    }
  }
  print_header(out, services, options);
  std::vector<Check> checks = golden_checks(expected);
  for (Check& check : oracle_checks(syscalls, services, options)) {
    checks.push_back(std::move(check));
  }
  for (const Check& check : checks) {
    out << (check.passed ? "PASS  " : "FAIL  ") << check.name << ": " << check.detail << "\n";
    if (options.verbose) {
      out << check.extra;
    }
  }
  const auto failed = static_cast<std::size_t>(
      std::ranges::count_if(checks, [](const Check& check) { return !check.passed; }));
  if (failed == 0) {
    out << "selftest passed: " << checks.size()
        << " of " << checks.size()
        << " checks. This build on this machine reproduced what it is expected to; that is a "
           "precondition of a hardware verdict, not a finding that the hardware is sound.\n";
    return kOk;
  }
  out << "selftest FAILED: " << failed << " of " << checks.size()
      << " checks failed. This environment is not trustworthy for a hardware verdict: the "
         "machine may be faulty, the build or the tool may be broken, or the platform may be "
         "unsupported. Selftest does not say which (docs/verification.md section 9).\n";
  return kExitSelftestFailed;
}


namespace {

/// The footprints below this are the user's typo and not the machine's fault: a workload
/// refuses one (sort's unit is 2.5 MiB), and a refusal would be read as a failed environment.
constexpr std::uint64_t kLeastFootprintBytes = std::uint64_t{3} << 20U;

core::Result<std::uint64_t, std::string> whole_number(std::string_view flag,
                                                      std::string_view text) {
  const auto split = core::split_number_and_suffix(text);
  if (!split.has_value() || !split.value().suffix.empty()) {
    return std::string{flag} + ": expected a whole number, got '" + std::string{text} + "'";
  }
  return split.value().value;
}

/// One flag with its value put into `options`, or what is wrong with it.
core::Result<bool, std::string> take_value(SelftestOptions& options, std::string_view flag,
                                           std::string_view value) {
  if (flag == "--isa") {
    options.isa = simd::parse_isa_path(value);
    if (!options.isa.has_value()) {
      return std::string{"--isa: '"} + std::string{value} + "' is none of scalar, avx2, avx512, neon";
    }
  } else if (flag == "--footprint") {
    const auto size = core::parse_byte_size(value);
    if (!size.has_value()) {
      return "--footprint: " + std::string{core::describe(size.error())};
    }
    if (!size.value().is_absolute()) {
      return std::string{"--footprint must be a size, not a share of memory"};
    }
    if (size.value().byte_count() < kLeastFootprintBytes) {
      return std::string{"--footprint must be at least 3MiB"};
    }
    options.footprint_bytes = size.value().byte_count();
  } else {
    const auto seed = whole_number(flag, value);
    if (!seed.has_value()) {
      return seed.error();
    }
    options.seed = seed.value();
  }
  return true;
}

}  // namespace

core::Result<SelftestOptions, std::string> parse_selftest_args(
    std::span<const std::string_view> args) {
  constexpr std::array<std::string_view, 3> kWithValue{"--isa", "--footprint", "--seed"};
  SelftestOptions options;
  std::set<std::string_view> seen;
  for (std::size_t index = 0; index < args.size(); ++index) {
    const std::string_view flag = args[index];
    const bool takes_value = std::ranges::find(kWithValue, flag) != kWithValue.end();
    if (!takes_value && flag != "--verbose") {
      return "unknown argument '" + std::string{flag} + "'";
    }
    if (!seen.insert(flag).second) {
      return std::string{flag} + " given more than once";
    }
    if (!takes_value) {
      options.verbose = true;
      continue;
    }
    if (index + 1 == args.size()) {
      return std::string{flag} + " needs a value";
    }
    ++index;
    if (auto taken = take_value(options, flag, args[index]); !taken.has_value()) {
      return taken.error();
    }
  }
  return options;
}

int selftest_command(std::span<const std::string_view> args, std::ostream& out,
                     std::ostream& err) {
  const auto options = parse_selftest_args(args);
  if (!options.has_value()) {
    err << "loadforge selftest: " << options.error() << "\n";
    return kUsage;
  }
  platform::RealSyscalls syscalls;
  const worker::ProductionServices production;
  return run_selftest(syscalls, production.services(), options.value(),
                      ExpectedDigests{selftest::kExpectedCorpusDigest, selftest::kExpectedDigest},
                      out, err);
}

}  // namespace loadforge::cli
