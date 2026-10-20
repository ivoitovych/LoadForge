// SPDX-License-Identifier: GPL-3.0-or-later
#include "telemetry/sources/power/power.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
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

Unavailable make_failure(Availability kind, std::string path, std::string detail,
                         std::string remediation) {
  return Unavailable{kind, std::move(path), std::move(detail), std::move(remediation)};
}

/// A failed read of a sysfs attribute, in the report's form.
Unavailable from_file(topology::Unavailable failure) {
  std::string remediation = powercap_remediation(failure.kind, failure.path);
  return make_failure(failure.kind, std::move(failure.path), std::move(failure.detail),
                      std::move(remediation));
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
  switch (kind) {
    case Availability::kDenied:
      return 5;
    case Availability::kMalformed:
      return 4;
    case Availability::kUnreadable:
      return 3;
    case Availability::kVanished:
      return 2;
    case Availability::kAbsent:
    case Availability::kPresent:
      break;
  }
  return 1;
}

/// Several providers failed for one package; say it once. The kind and path are
/// those of the failure that can be acted on most; the detail names every
/// provider's; the remedy is each ACTIONABLE provider's own, labelled, and a
/// provider that merely lacks the interface contributes none -- advice for it
/// could not work. When none is actionable, the first one's remedy stands alone.
Unavailable combine(const std::vector<Labelled>& failures) {
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
    return best->failure;
  }
  std::string detail;
  std::string remediation;
  for (const Labelled& labelled : failures) {
    if (!detail.empty()) {
      detail += "; ";
    }
    detail += labelled.provider;
    detail += ": ";
    detail += labelled.failure.detail;
    if (labelled.failure.kind != Availability::kAbsent) {
      if (!remediation.empty()) {
        remediation += "; ";
      }
      if (actionable > 1) {
        remediation += labelled.provider;
        remediation += ": ";
      }
      remediation += labelled.failure.remediation;
    }
  }
  if (actionable == 0) {
    remediation = best->failure.remediation;
  }
  return make_failure(best->failure.kind, best->failure.path, std::move(detail),
                      std::move(remediation));
}

Metric make_metric(std::uint32_t index, std::optional<std::uint64_t> modulus,
                   std::string_view provider) {
  Metric metric;
  metric.name = std::string{kPackageMetric};
  metric.unit = Unit::kJoules;
  metric.kind = Kind::kCounter;
  metric.scope = Scope::kPackage;
  metric.scope_index = index;
  metric.modulus = modulus;
  metric.provider = std::string{provider};
  return metric;
}

/// The power PMU as sysfs describes it: its type number, the energy-pkg event's
/// config, the factor from a count to microjoules, and the CPU that stands for
/// each package. Or why none of that could be had.
struct PmuInfo {
  std::uint32_t type = 0;
  std::uint64_t config = 0;
  double factor = 0.0;
  std::vector<std::uint32_t> cpus;
  std::optional<Unavailable> failure;
};

PmuInfo read_pmu(platform::FileSystem& filesystem, const std::string& root) {
  PmuInfo info;
  const std::string directory = root + "/bus/event_source/devices/power";

  topology::Source type_file{filesystem, directory + "/type"};
  auto type = read_count(type_file);
  if (!type.has_value()) {
    info.failure = from_file(type.error());
    return info;
  }
  info.type = static_cast<std::uint32_t>(type.value());

  topology::Source event_file{filesystem, directory + "/events/energy-pkg"};
  auto event = event_file.text();
  if (!event.has_value()) {
    info.failure = from_file(event.error());
    return info;
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
    info.failure = from_file(topology::Unavailable{Availability::kMalformed,
                                                   directory + "/events/energy-pkg", detail});
    return info;
  }
  info.config = config;

  topology::Source scale_file{filesystem, directory + "/events/energy-pkg.scale"};
  auto scale = scale_file.text();
  if (!scale.has_value()) {
    info.failure = from_file(scale.error());
    return info;
  }
  double joules = 0.0;
  const std::string_view scale_text = scale.value();
  const auto [scale_stop, scale_ec] =
      std::from_chars(scale_text.data(), scale_text.data() + scale_text.size(), joules);
  const double factor = joules * kMicrojoulesPerJoule;
  if (scale_ec != std::errc{} || scale_stop != scale_text.data() + scale_text.size() ||
      !(factor > 0.0 && factor <= kLargestFactor)) {
    std::string detail = "not a plausible joules-per-count scale: '";
    detail += scale_text;
    detail += "'";
    info.failure = from_file(topology::Unavailable{
        Availability::kMalformed, directory + "/events/energy-pkg.scale", detail});
    return info;
  }
  info.factor = factor;

  topology::Source mask_file{filesystem, directory + "/cpumask"};
  auto mask = mask_file.cpu_list();
  if (!mask.has_value()) {
    info.failure = from_file(mask.error());
    return info;
  }
  info.cpus = mask.value().ids();
  if (info.cpus.empty()) {
    info.failure = make_failure(Availability::kAbsent, directory + "/cpumask",
                                "the power PMU names no CPU to read it from",
                                powercap_remediation(Availability::kAbsent, directory));
  }
  return info;
}

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
      text += " is refused: energy_uj is root-only since CVE-2020-8694; run LoadForge "
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
  for (const Channel& channel : channels_) {
    if (channel.fd >= 0) {
      static_cast<void>(syscalls_->close(channel.fd));
    }
  }
  channels_.clear();
}

std::vector<MetricStatus> PowerSource::probe() {
  close_all();
  std::vector<MetricStatus> statuses;

  const PmuInfo pmu = read_pmu(*filesystem_, root_);
  std::optional<Unavailable> pmu_failure = pmu.failure;

  // Powercap's side of package `index`, if the machine has a zone for it: the
  // channel, or why the zone could not be read.
  struct Attempt {
    std::optional<Channel> channel;
    std::optional<Unavailable> failure;
  };

  const auto try_powercap = [&](const std::string& zone) {
    Attempt attempt;
    const std::string directory = root_ + "/class/powercap/" + zone;
    topology::Source range_file{*filesystem_, directory + "/max_energy_range_uj"};
    auto range = read_count(range_file);
    if (!range.has_value()) {
      attempt.failure = from_file(range.error());
      return attempt;
    }
    if (range.value() == 0) {
      attempt.failure = make_failure(Availability::kMalformed, range_file.path(),
                                     "a range of zero cannot hold a counter",
                                     powercap_remediation(Availability::kMalformed,
                                                          range_file.path()));
      return attempt;
    }
    topology::Source energy{*filesystem_, directory + "/energy_uj"};
    auto first = read_count(energy);
    if (!first.has_value()) {
      attempt.failure = from_file(first.error());
      return attempt;
    }
    if (first.value() >= range.value()) {
      std::string detail = "energy ";
      detail += std::to_string(first.value());
      detail += " is not below the range ";
      detail += std::to_string(range.value());
      attempt.failure = make_failure(Availability::kMalformed, energy.path(), std::move(detail),
                                     powercap_remediation(Availability::kMalformed,
                                                          energy.path()));
      return attempt;
    }
    attempt.channel = Channel{std::move(energy), range.value(), -1, 0.0, std::string{}};
    return attempt;
  };

  const auto try_perf = [&](std::uint32_t cpu) {
    Attempt attempt;
    platform::PerfEventSpec spec;
    spec.type = pmu.type;
    spec.config = pmu.config;
    spec.pid = -1;
    spec.cpu = static_cast<int>(cpu);
    auto opened = syscalls_->open_perf_event(spec);
    if (!opened.has_value()) {
      const platform::SyscallError& error = opened.error();
      const Availability kind = classify_perf_open(error.number);
      attempt.failure = make_failure(kind, error.subject, platform::describe(error),
                                     perf_remediation(kind, std::nullopt, error.subject));
      return attempt;
    }
    const int fd = opened.value();
    for (const platform::PerfAction action :
         {platform::PerfAction::kReset, platform::PerfAction::kEnable}) {
      auto done = syscalls_->control_perf_event(fd, action);
      if (!done.has_value()) {
        static_cast<void>(syscalls_->close(fd));
        const platform::SyscallError& error = done.error();
        attempt.failure = make_failure(Availability::kUnreadable, error.subject,
                                       platform::describe(error),
                                       perf_remediation(Availability::kUnreadable, std::nullopt,
                                                        error.subject));
        return attempt;
      }
    }
    std::string subject = "perf event type ";
    subject += std::to_string(pmu.type);
    subject += " config ";
    subject += std::to_string(pmu.config);
    attempt.channel = Channel{topology::Source{*filesystem_, std::string{}}, 0, fd, pmu.factor,
                              std::move(subject)};
    return attempt;
  };

  std::uint32_t package = 0;
  for (std::uint32_t zone = 0;; ++zone) {
    const std::string zone_name = "intel-rapl:" + std::to_string(zone);
    topology::Source name_file{*filesystem_,
                               root_ + "/class/powercap/" + zone_name + "/name"};
    auto name = name_file.text();
    const bool has_pmu_cpu = !pmu.failure.has_value() && package < pmu.cpus.size();
    if (!name.has_value() && name.error().kind == Availability::kAbsent) {
      break;
    }
    if (name.has_value() && !name.value().starts_with("package")) {
      continue;  // A zone that is not a package (psys): not ours to report.
    }
    Attempt best = name.has_value() ? try_powercap(zone_name) : Attempt{};
    std::vector<Labelled> failures;
    if (!name.has_value()) {
      failures.push_back(Labelled{"powercap", from_file(name.error())});
    } else if (best.failure.has_value()) {
      failures.push_back(Labelled{"powercap", *best.failure});
    }
    if (!best.channel.has_value() && has_pmu_cpu) {
      Attempt perf = try_perf(pmu.cpus[package]);
      if (perf.channel.has_value()) {
        best.channel = std::move(perf.channel);
      } else {
        failures.push_back(Labelled{"perf", *perf.failure});
      }
    } else if (!best.channel.has_value() && pmu_failure.has_value()) {
      failures.push_back(Labelled{"perf", *pmu_failure});
    }

    if (best.channel.has_value()) {
      const bool via_perf = best.channel->fd >= 0;
      std::optional<std::uint64_t> modulus;
      if (!via_perf) {
        modulus = best.channel->modulus;
      }
      statuses.push_back(MetricStatus{
          make_metric(package, modulus, via_perf ? kPerfProvider : kPowercapProvider),
          std::nullopt});
      channels_.push_back(std::move(*best.channel));
    } else {
      statuses.push_back(MetricStatus{make_metric(package, std::nullopt, kPowercapProvider),
                                      combine(failures)});
    }
    ++package;
  }

  // No powercap zone at all: the packages are the power PMU's CPUs, or, when it
  // has none to offer, one metric that says why there is no package power.
  if (package == 0) {
    for (std::uint32_t index = 0; !pmu.failure.has_value() && index < pmu.cpus.size(); ++index) {
      Attempt perf = try_perf(pmu.cpus[index]);
      if (perf.channel.has_value()) {
        statuses.push_back(
            MetricStatus{make_metric(index, std::nullopt, kPerfProvider), std::nullopt});
        channels_.push_back(std::move(*perf.channel));
      } else {
        statuses.push_back(MetricStatus{make_metric(index, std::nullopt, kPerfProvider),
                                        *perf.failure});
      }
    }
    if (statuses.empty()) {
      statuses.push_back(MetricStatus{make_metric(0, std::nullopt, kPowercapProvider),
                                      *pmu_failure});
    }
  }
  return statuses;
}

void PowerSource::sample(std::vector<Reading>& out) {
  for (Channel& channel : channels_) {
    if (channel.fd >= 0) {
      auto count = syscalls_->read_perf_count(channel.fd);
      if (!count.has_value()) {
        const platform::SyscallError& error = count.error();
        const Availability kind = topology::classify(error, true);
        out.emplace_back(make_failure(kind, channel.subject, platform::describe(error),
                                      perf_remediation(kind, std::nullopt, channel.subject)));
        continue;
      }
      out.emplace_back(Counter{static_cast<std::uint64_t>(
          static_cast<double>(count.value().value) * channel.microjoules_per_count)});
      continue;
    }
    auto energy = read_count(channel.energy);
    if (energy.has_value() && energy.value() >= channel.modulus) {
      std::string detail = "energy ";
      detail += std::to_string(energy.value());
      detail += " is not below the range ";
      detail += std::to_string(channel.modulus);
      out.emplace_back(make_failure(Availability::kMalformed, channel.energy.path(),
                                    std::move(detail),
                                    powercap_remediation(Availability::kMalformed,
                                                         channel.energy.path())));
    } else if (energy.has_value()) {
      out.emplace_back(Counter{energy.value()});
    } else {
      out.emplace_back(from_file(energy.error()));
    }
  }
}

}  // namespace loadforge::telemetry
