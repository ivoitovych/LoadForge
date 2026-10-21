// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/power/power.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/parse_number.hpp"
#include "core/result.hpp"
#include "platform/fs.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"
#include "telemetry/sample.hpp"
#include "telemetry/sources/counters/counters.hpp"
#include "topology/source.hpp"

namespace loadforge::telemetry {
namespace {

using topology::Availability;

constexpr std::string_view kPackageMetric = "power.package";
constexpr std::string_view kPowercapProvider = "powercap:intel-rapl";
constexpr std::string_view kPerfProvider = "perf:power";

/// The scale a PMU publishes is joules per count; the metric is microjoules. A
/// factor above one half could round a count near 2^64 up to 2^64, which cannot
/// be converted back; real scales are 2^-32 J, a factor of about 2e-4.
constexpr double kMicrojoulesPerJoule = 1e6;
constexpr double kLargestFactor = 0.5;

/// A failure, with the provider's name for when two are reported as one.
struct Labelled {
  std::string_view provider;
  Unavailable failure;
};

// Every failure below is filled where it will live, a field at a time, so that
// no half-built Unavailable exists while the next string is allocated: the
// cleanup of one is a branch no test can take (journal §1.13).

/// A failure of a powercap or PMU-description read, with powercap's remedy.
void fill(Unavailable& why, Availability kind, const std::string& path, std::string_view detail) {
  why.kind = kind;
  why.path = path;
  why.detail = detail;
  why.remediation = powercap_remediation(kind, path);
}

/// A failed perf call, in the report's form: the kernel's words, the perf remedy.
void fill_perf(Unavailable& why, Availability kind, const platform::SyscallError& error,
               const std::string& path) {
  why.kind = kind;
  why.path = path;
  why.detail = platform::describe(error);
  why.remediation = perf_remediation(kind, std::nullopt, path);
}

/// A failed read of a sysfs attribute, in the report's form.
void fill_from_file(Unavailable& why, topology::Unavailable failure) {
  why.kind = failure.kind;
  why.remediation = powercap_remediation(failure.kind, failure.path);
  why.path = std::move(failure.path);
  why.detail = std::move(failure.detail);
}

/// A decimal count the kernel wrote: digits and nothing else.
core::Result<std::uint64_t, topology::Unavailable> read_count(topology::Source& file) {
  auto text = file.text();
  if (!text.has_value()) {
    return text.error();
  }
  auto parsed = core::split_number_and_suffix(text.value());
  if (!parsed.has_value() || !parsed.value().suffix.empty()) {
    std::string detail = "not a decimal count: '";
    detail += text.value();
    detail += "'";
    return topology::Unavailable{Availability::kMalformed, file.path(), std::move(detail)};
  }
  return parsed.value().value;
}

/// How much more a failure can be acted on: a refusal of permission is the
/// most, a fact about the platform the least.
int actionability(Availability kind) {
  if (kind == Availability::kDenied) {
    return 5;
  }
  if (kind == Availability::kMalformed) {
    return 4;
  }
  if (kind == Availability::kUnreadable) {
    return 3;
  }
  return 1;  // absent: a fact about the platform; vanished cannot occur at probe
}

/// Several providers failed for one package; say it once. The kind and path are
/// those of the failure that can be acted on most; the detail names every
/// provider's; the remedy is each ACTIONABLE provider's own, labelled, and a
/// provider that merely lacks the interface contributes none -- advice for it
/// could not work. When none is actionable, the first one's remedy stands alone.
void combine(Unavailable& out, const std::vector<Labelled>& failures) {
  const Labelled* best = &failures.front();
  std::size_t actionable = 0;
  for (const Labelled& labelled : failures) {
    if (labelled.failure.kind != Availability::kAbsent) {
      ++actionable;
    }
    if (actionability(labelled.failure.kind) > actionability(best->failure.kind)) {
      best = &labelled;
    }
  }
  if (failures.size() == 1) {
    out = best->failure;
    return;
  }
  out.kind = best->failure.kind;
  out.path = best->failure.path;
  for (const Labelled& labelled : failures) {
    if (!out.detail.empty()) {
      out.detail += "; ";
    }
    out.detail += labelled.provider;
    out.detail += ": ";
    out.detail += labelled.failure.detail;
    if (labelled.failure.kind != Availability::kAbsent) {
      if (!out.remediation.empty()) {
        out.remediation += "; ";
      }
      if (actionable > 1) {
        out.remediation += labelled.provider;
        out.remediation += ": ";
      }
      out.remediation += labelled.failure.remediation;
    }
  }
  if (actionable == 0) {
    out.remediation = best->failure.remediation;
  }
}

/// The power PMU as sysfs describes it: its type number, the energy-pkg event's
/// config, the factor from a count to microjoules, and the CPU that stands for
/// each package. Or, with `failed`, why none of that could be had.
struct PmuInfo {
  std::uint32_t type = 0;
  std::uint64_t config = 0;
  double factor = 0.0;
  std::vector<std::uint32_t> cpus;
  bool failed = false;
  Unavailable failure;
};

/// One provider's attempt at one package: the channel that reads it, or, with
/// `failed`, why not. Neither: the provider had nothing to try.
struct Attempt {
  std::optional<PowerChannel> channel;
  bool failed = false;
  Unavailable failure;
};

void fail_malformed(PmuInfo& info, const std::string& path, std::string_view detail) {
  info.failed = true;
  fill(info.failure, Availability::kMalformed, path, detail);
}

void fail_read(PmuInfo& info, topology::Unavailable failure) {
  info.failed = true;
  fill_from_file(info.failure, std::move(failure));
}

/// The energy-pkg event's config, from "event=0x<hex>". False, with the reason
/// in `info`, when it is anything else.
bool read_event(platform::FileSystem& filesystem, const std::string& directory, PmuInfo& info) {
  topology::Source event_file{filesystem, directory + "/events/energy-pkg"};
  auto event = event_file.text();
  if (!event.has_value()) {
    fail_read(info, event.error());
    return false;
  }
  constexpr std::string_view kPrefix = "event=0x";
  const std::string_view event_text = event.value();
  std::uint64_t config = 0;
  const bool prefixed = event_text.starts_with(kPrefix);
  const std::string_view digits = prefixed ? event_text.substr(kPrefix.size()) : event_text;
  const auto [stop, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), config, 16);
  if (!prefixed || ec != std::errc{} || stop != digits.data() + digits.size()) {
    std::string detail = "not an event=0x<hex> description: '";
    detail += event_text;
    detail += "'";
    fail_malformed(info, event_file.path(), detail);
    return false;
  }
  info.config = config;
  return true;
}

/// The factor from a count to microjoules, from the joules-per-count scale.
bool read_scale(platform::FileSystem& filesystem, const std::string& directory, PmuInfo& info) {
  topology::Source scale_file{filesystem, directory + "/events/energy-pkg.scale"};
  auto scale = scale_file.text();
  if (!scale.has_value()) {
    fail_read(info, scale.error());
    return false;
  }
  double joules = 0.0;
  const std::string_view scale_text = scale.value();
  const auto [stop, ec] =
      std::from_chars(scale_text.data(), scale_text.data() + scale_text.size(), joules);
  const double factor = joules * kMicrojoulesPerJoule;
  // Written so that NaN, which from_chars accepts, is not plausible.
  const bool plausible = factor > 0.0 && factor <= kLargestFactor;
  if (ec != std::errc{} || stop != scale_text.data() + scale_text.size() || !plausible) {
    std::string detail = "not a plausible joules-per-count scale: '";
    detail += scale_text;
    detail += "'";
    fail_malformed(info, scale_file.path(), detail);
    return false;
  }
  info.factor = factor;
  return true;
}

PmuInfo read_pmu(platform::FileSystem& filesystem, const std::string& root) {
  PmuInfo info;
  const std::string directory = root + "/bus/event_source/devices/power";

  topology::Source type_file{filesystem, directory + "/type"};
  auto type = read_count(type_file);
  if (!type.has_value()) {
    fail_read(info, type.error());
    return info;
  }
  if (type.value() > std::numeric_limits<std::uint32_t>::max()) {
    std::string detail = "not a PMU type number: ";
    detail += std::to_string(type.value());
    fail_malformed(info, type_file.path(), detail);
    return info;
  }
  info.type = static_cast<std::uint32_t>(type.value());

  if (!read_event(filesystem, directory, info) || !read_scale(filesystem, directory, info)) {
    return info;
  }

  topology::Source mask_file{filesystem, directory + "/cpumask"};
  auto mask = mask_file.cpu_list();
  if (!mask.has_value()) {
    fail_read(info, mask.error());
    return info;
  }
  info.cpus = mask.value().ids();
  if (info.cpus.empty()) {
    info.failed = true;
    fill(info.failure, Availability::kAbsent, mask_file.path(),
         "the power PMU names no CPU to read it from");
  }
  return info;
}

/// Package power from powercap: the zone's range, then its energy, each checked.
Attempt try_powercap(platform::FileSystem& filesystem, const std::string& root,
                     const std::string& zone) {
  Attempt attempt;
  const std::string directory = root + "/class/powercap/" + zone;
  topology::Source range_file{filesystem, directory + "/max_energy_range_uj"};
  auto range = read_count(range_file);
  if (!range.has_value()) {
    attempt.failed = true;
    fill_from_file(attempt.failure, range.error());
    return attempt;
  }
  if (range.value() == 0) {
    attempt.failed = true;
    fill(attempt.failure, Availability::kMalformed, range_file.path(),
         "a range of zero cannot hold a counter");
    return attempt;
  }
  topology::Source energy{filesystem, directory + "/energy_uj"};
  auto first = read_count(energy);
  if (!first.has_value()) {
    attempt.failed = true;
    fill_from_file(attempt.failure, first.error());
    return attempt;
  }
  if (first.value() >= range.value()) {
    std::string detail = "energy ";
    detail += std::to_string(first.value());
    detail += " is not below the range ";
    detail += std::to_string(range.value());
    attempt.failed = true;
    fill(attempt.failure, Availability::kMalformed, energy.path(), detail);
    return attempt;
  }
  attempt.channel.emplace(std::move(energy), range.value(), -1, 0.0, std::string{});
  return attempt;
}

/// Package power from the power PMU: the energy-pkg counter, system-wide on
/// `cpu`, reset and enabled.
Attempt try_perf(platform::Syscalls& syscalls, platform::FileSystem& filesystem, const PmuInfo& pmu,
                 std::uint32_t cpu) {
  Attempt attempt;
  platform::PerfEventSpec spec;
  spec.type = pmu.type;
  spec.config = pmu.config;
  spec.pid = -1;
  spec.cpu = static_cast<int>(cpu);
  auto opened = syscalls.open_perf_event(spec);
  if (!opened.has_value()) {
    const platform::SyscallError& error = opened.error();
    attempt.failed = true;
    fill_perf(attempt.failure, classify_perf_open(error.number), error, error.subject);
    return attempt;
  }
  const int fd = opened.value();
  for (const platform::PerfAction action :
       {platform::PerfAction::kReset, platform::PerfAction::kEnable}) {
    auto done = syscalls.control_perf_event(fd, action);
    if (!done.has_value()) {
      static_cast<void>(syscalls.close(fd));
      const platform::SyscallError& error = done.error();
      attempt.failed = true;
      fill_perf(attempt.failure, Availability::kUnreadable, error, error.subject);
      return attempt;
    }
  }
  std::string subject = "perf event type ";
  subject += std::to_string(pmu.type);
  subject += " config ";
  subject += std::to_string(pmu.config);
  attempt.channel.emplace(topology::Source{filesystem, std::string{}}, 0, fd, pmu.factor,
                          std::move(subject));
  return attempt;
}

/// The probe's working state: where the statuses and channels go, and the two
/// providers' attempts at each package.
class Prober {
 public:
  Prober(platform::Syscalls& syscalls, platform::FileSystem& filesystem, const std::string& root,
         std::vector<PowerChannel>& channels, std::vector<MetricStatus>& statuses)
      : syscalls_(&syscalls),
        filesystem_(&filesystem),
        root_(&root),
        channels_(&channels),
        statuses_(&statuses),
        pmu_(read_pmu(filesystem, root)) {}

  void run() {
    std::uint32_t package = 0;
    for (std::uint32_t zone = 0; step(zone, package); ++zone) {
    }
    // No powercap zone at all: the packages are the power PMU's CPUs, or, when
    // it has none to offer, one metric that says why there is no package power.
    if (package == 0) {
      pmu_only();
    }
  }

 private:
  /// One zone. False when the list of zones has ended; `package` advances only
  /// for a zone that is a package.
  bool step(std::uint32_t zone, std::uint32_t& package) {
    std::string zone_name = "intel-rapl:";
    zone_name += std::to_string(zone);
    topology::Source name_file{*filesystem_, *root_ + "/class/powercap/" + zone_name + "/name"};
    auto name = name_file.text();
    if (!name.has_value() && name.error().kind == Availability::kAbsent) {
      return false;
    }
    if (name.has_value() && !name.value().starts_with("package")) {
      return true;  // A zone that is not a package (psys): not ours to report.
    }
    Attempt powercap;
    if (name.has_value()) {
      powercap = try_powercap(*filesystem_, *root_, zone_name);
    } else {
      powercap.failed = true;
      fill_from_file(powercap.failure, name.error());
    }
    package_through_providers(package, powercap);
    ++package;
    return true;
  }

  void package_through_providers(std::uint32_t package, Attempt& powercap) {
    if (powercap.channel.has_value()) {
      serve(package, std::move(*powercap.channel));
      return;
    }
    std::vector<Labelled> failures;
    failures.emplace_back();
    failures.back().provider = "powercap";
    failures.back().failure = powercap.failure;
    Attempt perf = second_provider(package);
    if (perf.channel.has_value()) {
      serve(package, std::move(*perf.channel));
      return;
    }
    if (perf.failed) {
      failures.emplace_back();
      failures.back().provider = "perf";
      failures.back().failure = perf.failure;
    }
    combine(refuse(package, kPowercapProvider), failures);
  }

  /// The power PMU's try at a package powercap could not serve. Nothing to try
  /// when it has no CPU for it, or is simply not there: a power PMU that is
  /// absent was never a second provider, and saying it failed would be noise.
  /// One that is there and cannot be read is a failure of the perf provider.
  Attempt second_provider(std::uint32_t package) {
    if (!pmu_.failed && package < pmu_.cpus.size()) {
      return try_perf(*syscalls_, *filesystem_, pmu_, pmu_.cpus[package]);
    }
    Attempt attempt;
    attempt.failed = pmu_.failed && pmu_.failure.kind != Availability::kAbsent;
    if (attempt.failed) {
      attempt.failure = pmu_.failure;
    }
    return attempt;
  }

  void pmu_only() {
    for (std::uint32_t index = 0; !pmu_.failed && index < pmu_.cpus.size(); ++index) {
      Attempt perf = try_perf(*syscalls_, *filesystem_, pmu_, pmu_.cpus[index]);
      if (perf.channel.has_value()) {
        serve(index, std::move(*perf.channel));
      } else {
        refuse(index, kPerfProvider) = perf.failure;
      }
    }
    if (statuses_->empty()) {
      refuse(0, kPowercapProvider) = pmu_.failure;
    }
  }

  /// A package that was served: its status, and the channel that reads it.
  void serve(std::uint32_t package, PowerChannel&& channel) {
    const bool via_perf = channel.fd >= 0;
    MetricStatus& status = start_status(package, via_perf ? kPerfProvider : kPowercapProvider);
    if (!via_perf) {
      status.metric.modulus = channel.modulus;
    }
    channels_->push_back(std::move(channel));
  }

  /// A package that was not: the status carries the failure, filled by the caller.
  Unavailable& refuse(std::uint32_t package, std::string_view provider) {
    return start_status(package, provider).unavailable.emplace();
  }

  MetricStatus& start_status(std::uint32_t package, std::string_view provider) {
    statuses_->emplace_back();
    MetricStatus& status = statuses_->back();
    status.metric.name = kPackageMetric;
    status.metric.unit = Unit::kJoules;
    status.metric.kind = Kind::kCounter;
    status.metric.scope = Scope::kPackage;
    status.metric.scope_index = package;
    status.metric.provider = provider;
    return status;
  }

  platform::Syscalls* syscalls_;
  platform::FileSystem* filesystem_;
  const std::string* root_;
  std::vector<PowerChannel>* channels_;
  std::vector<MetricStatus>* statuses_;
  PmuInfo pmu_;
};

}  // namespace

std::string powercap_remediation(Availability kind, std::string_view path) {
  std::string text;
  switch (kind) {
    case Availability::kAbsent:
      return "this platform does not offer this power interface (no RAPL on this CPU or "
             "virtual machine, or the driver is not loaded); nothing can be changed from the "
             "software side";
    case Availability::kDenied:
      text = "reading ";
      text += path;
      text +=
          " is refused: energy_uj is root-only since CVE-2020-8694; run LoadForge "
          "privileged, or give your user read access to the file with a udev rule or "
          "group ownership";
      return text;
    case Availability::kVanished:
      return "the power interface went away during the run (driver unloaded); read the kernel "
             "log (dmesg), and treat readings before it as from a machine that has changed";
    case Availability::kMalformed:
      text = "the kernel writes a decimal count to ";
      text += path;
      text += " and it held something else; report its content and the kernel version";
      return text;
    case Availability::kUnreadable:
    case Availability::kPresent:
      break;
  }
  text = "reading ";
  text += path;
  text += " failed; read the kernel log (dmesg) for power-capping driver errors";
  return text;
}

PowerSource::PowerSource(platform::Syscalls& syscalls, platform::FileSystem& filesystem,
                         std::string_view sysfs_root)
    : syscalls_(&syscalls), filesystem_(&filesystem), root_(sysfs_root) {}

PowerSource::~PowerSource() { close_all(); }

void PowerSource::close_all() {
  for (const PowerChannel& channel : channels_) {
    if (channel.fd >= 0) {
      static_cast<void>(syscalls_->close(channel.fd));
    }
  }
  channels_.clear();
}

std::vector<MetricStatus> PowerSource::probe() {
  close_all();
  std::vector<MetricStatus> statuses;
  Prober{*syscalls_, *filesystem_, root_, channels_, statuses}.run();
  return statuses;
}

void PowerSource::sample(std::vector<Reading>& out) {
  for (PowerChannel& channel : channels_) {
    if (channel.fd >= 0) {
      auto count = syscalls_->read_perf_count(channel.fd);
      if (count.has_value()) {
        out.emplace_back(Counter{static_cast<std::uint64_t>(
            static_cast<double>(count.value().value) * channel.microjoules_per_count)});
      } else {
        const platform::SyscallError& error = count.error();
        Unavailable why;
        fill_perf(why, topology::classify(error, true), error, channel.subject);
        out.emplace_back(std::move(why));
      }
      continue;
    }
    auto energy = read_count(channel.energy);
    if (energy.has_value() && energy.value() >= channel.modulus) {
      std::string detail = "energy ";
      detail += std::to_string(energy.value());
      detail += " is not below the range ";
      detail += std::to_string(channel.modulus);
      Unavailable why;
      fill(why, Availability::kMalformed, channel.energy.path(), detail);
      out.emplace_back(std::move(why));
    } else if (energy.has_value()) {
      out.emplace_back(Counter{energy.value()});
    } else {
      Unavailable why;
      fill_from_file(why, energy.error());
      out.emplace_back(std::move(why));
    }
  }
}

}  // namespace loadforge::telemetry
