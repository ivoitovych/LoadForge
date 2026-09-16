// SPDX-License-Identifier: GPL-3.0-or-later
#include "topology/cache.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "topology/cpu_list.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::topology {
namespace {

/// Refuses a size past this. A cache measured in exabytes is not a cache; it is
/// a file that is not what we think it is.
constexpr std::uint64_t kMaxCacheBytes = 1ULL << 40U;  // 1 TiB

/// Deepest level accepted. Real machines stop at 3, occasionally 4; anything
/// past this says the file is not a cache level.
constexpr std::int64_t kMaxCacheLevel = 8;

std::string index_directory(std::string_view sysfs_root, std::uint32_t cpu, std::uint32_t index) {
  return std::string{sysfs_root} + "/devices/system/cpu/cpu" + std::to_string(cpu) +
         "/cache/index" + std::to_string(index);
}

DiscoveryFailure source_failure(const Unavailable& unavailable) {
  std::string subject = unavailable.path;
  std::string detail = unavailable.detail;
  return DiscoveryFailure{DiscoveryFailure::Kind::kSource, unavailable.kind, std::move(subject),
                          std::move(detail)};
}

DiscoveryFailure disagreement(std::string subject, std::string detail) {
  return DiscoveryFailure{DiscoveryFailure::Kind::kContradiction, Availability::kPresent,
                          std::move(subject), std::move(detail)};
}

core::Result<CacheType, std::string_view> parse_type(std::string_view text) {
  // Exactly the three strings the kernel writes, matched exactly. A prefix or
  // case-insensitive match would turn an unexpected value into a plausible
  // wrong one, and a cache whose type we guessed is worse than one we refused.
  if (text == "Data") {
    return CacheType::kData;
  }
  if (text == "Instruction") {
    return CacheType::kInstruction;
  }
  if (text == "Unified") {
    return CacheType::kUnified;
  }
  return std::string_view{"not one of Data, Instruction or Unified"};
}

/// Reads a small non-negative integer attribute, refusing a negative value.
core::Result<std::uint32_t, DiscoveryFailure> read_count(platform::FileSystem& filesystem,
                                                         const std::string& path) {
  Source source(filesystem, path);
  auto value = source.integer();
  if (!value.has_value()) {
    return source_failure(value.error());
  }
  if (value.value() < 0) {
    std::string detail = "the kernel reports " + std::to_string(value.value()) +
                         ", and a cache cannot have a negative one";
    return disagreement(path, std::move(detail));
  }
  if (value.value() > CpuList::kMaxCpuId) {
    std::string detail = "the kernel reports " + std::to_string(value.value()) +
                         ", which is past anything a cache attribute holds";
    return disagreement(path, std::move(detail));
  }
  return static_cast<std::uint32_t>(value.value());
}

std::string name_of(std::uint32_t cpu) { return "cpu" + std::to_string(cpu); }

/// Renders a cache the way a message should name it: "cpu0's L1 Data cache".
std::string name_of(const CacheId& identity) {
  return "the L" + std::to_string(identity.level) + " " + std::string{describe(identity.type)} +
         " cache (id " + std::to_string(identity.id) + ")";
}

/// One cache as one CPU described it, kept with the CPU it came from.
///
/// Every reading is retained until the cross-checks have run. Deduplicating
/// while reading would discard the second opinion that makes checking possible
/// at all.
struct Reading {
  std::uint32_t cpu = 0;
  Cache cache;
};

/// Reads `cache/indexN` for one CPU.
///
/// An empty optional means the index does not exist, which ends the scan: sysfs
/// numbers these contiguously from zero. Absence is the ONLY terminator. A
/// denied or unreadable index is a failure rather than the end of the list, or
/// a machine whose L3 we were not allowed to read would silently report as a
/// machine with no L3.
core::Result<std::optional<Cache>, DiscoveryFailure> read_index(platform::FileSystem& filesystem,
                                                                const std::string& directory,
                                                                std::uint32_t cpu) {
  // `level` is read through Source::integer, NOT through parse_cache_size.
  // They look interchangeable and are not: a size parser accepts a `K` suffix,
  // so "1K" would read as level 1024 -- a plausible wrong answer, which is the
  // one outcome this module exists to avoid.
  const std::string level_path = directory + "/level";
  Source level_source(filesystem, level_path);
  auto level_value = level_source.integer();
  if (!level_value.has_value()) {
    if (level_value.error().kind == Availability::kAbsent) {
      return std::optional<Cache>{};
    }
    return source_failure(level_value.error());
  }
  if (level_value.value() < 1 || level_value.value() > kMaxCacheLevel) {
    std::string detail = "the kernel reports level " + std::to_string(level_value.value()) +
                         ", which is not a cache level";
    return disagreement(level_path, std::move(detail));
  }
  const auto level = static_cast<std::uint32_t>(level_value.value());

  Source type_source(filesystem, directory + "/type");
  auto type_text = type_source.text();
  if (!type_text.has_value()) {
    return source_failure(type_text.error());
  }
  auto type = parse_type(type_text.value());
  if (!type.has_value()) {
    std::string detail = "\"" + type_text.value() + "\" is " + std::string{type.error()};
    return disagreement(directory + "/type", std::move(detail));
  }

  auto id = read_count(filesystem, directory + "/id");
  if (!id.has_value()) {
    return id.error();
  }

  Source size_source(filesystem, directory + "/size");
  auto size_text = size_source.text();
  if (!size_text.has_value()) {
    return source_failure(size_text.error());
  }
  auto size = parse_cache_size(size_text.value());
  if (!size.has_value()) {
    std::string detail = "\"" + size_text.value() + "\": " + std::string{size.error()};
    return disagreement(directory + "/size", std::move(detail));
  }
  if (size.value() == 0) {
    std::string detail = "a cache of zero bytes is not a cache";
    return disagreement(directory + "/size", std::move(detail));
  }

  auto line = read_count(filesystem, directory + "/coherency_line_size");
  if (!line.has_value()) {
    return line.error();
  }

  Source shared_source(filesystem, directory + "/shared_cpu_list");
  auto shared = shared_source.cpu_list();
  if (!shared.has_value()) {
    return source_failure(shared.error());
  }

  // A cache is shared with at least the CPU it was read through. The cheapest
  // sign that this directory is not describing what we think.
  if (!shared.value().contains(cpu)) {
    std::string detail = name_of(cpu) + " is not among the CPUs sharing its own cache";
    return disagreement(directory + "/shared_cpu_list", std::move(detail));
  }

  const CacheId identity{level, type.value(), id.value()};
  return std::optional<Cache>{Cache{identity, size.value(), line.value(), shared.value()}};
}

/// Every CPU a cache claims to serve must be one the CPU topology found. A
/// cache naming a CPU that is not online means the cache masks and the hotplug
/// machinery disagree, and there is no way to tell which is right.
std::optional<DiscoveryFailure> check_served_cpus_online(const std::vector<Reading>& readings,
                                                         const CpuTopology& cpus,
                                                         std::string_view sysfs_root) {
  for (const Reading& reading : readings) {
    for (const std::uint32_t served : reading.cache.shared_with.ids()) {
      const bool online = std::any_of(cpus.cpus().begin(), cpus.cpus().end(),
                                      [served](const LogicalCpu& c) { return c.id == served; });
      if (!online) {
        std::string detail = name_of(reading.cache.identity) + " on " + name_of(reading.cpu) +
                             " claims to serve " + name_of(served) + ", which is not online";
        return disagreement(index_directory(sysfs_root, reading.cpu, 0) + "/../shared_cpu_list",
                            std::move(detail));
      }
    }
  }
  return std::nullopt;
}

/// Sharing an L1 is nested inside sharing an L2, and so on up. This is the
/// defining structural property of a cache hierarchy, and it is checked per CPU
/// rather than globally because that is where it is meaningful: the CPUs that
/// see MY L1 are among those that see MY L2.
std::optional<DiscoveryFailure> check_nesting(const std::vector<Reading>& readings,
                                              std::string_view sysfs_root) {
  for (const Reading& inner : readings) {
    for (const Reading& outer : readings) {
      if (outer.cpu != inner.cpu || outer.cache.identity.level <= inner.cache.identity.level) {
        continue;
      }
      for (const std::uint32_t served : inner.cache.shared_with.ids()) {
        if (!outer.cache.shared_with.contains(served)) {
          std::string detail = name_of(inner.cpu) + " shares " + name_of(inner.cache.identity) +
                               " with " + name_of(served) + " but does not share " +
                               name_of(outer.cache.identity) + " with it; caches nest";
          return disagreement(index_directory(sysfs_root, inner.cpu, 0) + "/..", std::move(detail));
        }
      }
    }
  }
  return std::nullopt;
}

/// Collapses readings to distinct caches, checking as it goes that every CPU
/// describing a given cache describes the SAME one. Sharing a cache is an
/// equivalence relation exactly as sharing a core is, so two readings of one
/// identity must be identical -- not merely compatible.
core::Result<std::vector<Cache>, DiscoveryFailure> deduplicate(const std::vector<Reading>& readings,
                                                               std::string_view sysfs_root) {
  std::vector<Cache> distinct;
  for (const Reading& reading : readings) {
    const auto existing =
        std::find_if(distinct.begin(), distinct.end(),
                     [&reading](const Cache& c) { return c.identity == reading.cache.identity; });
    if (existing == distinct.end()) {
      distinct.push_back(reading.cache);
      continue;
    }
    if (*existing != reading.cache) {
      std::string detail =
          name_of(reading.cache.identity) +
          " is described differently by two of the CPUs that share it, including " +
          name_of(reading.cpu);
      return disagreement(index_directory(sysfs_root, reading.cpu, 0) + "/..", std::move(detail));
    }
  }
  return distinct;
}

/// Level, then id, then type -- so L1 Data and L1 Instruction with the same id
/// sit together and in a fixed order.
bool ordered_before(const Cache& lhs, const Cache& rhs) {
  if (lhs.identity.level != rhs.identity.level) {
    return lhs.identity.level < rhs.identity.level;
  }
  if (lhs.identity.id != rhs.identity.id) {
    return lhs.identity.id < rhs.identity.id;
  }
  return static_cast<std::uint8_t>(lhs.identity.type) <
         static_cast<std::uint8_t>(rhs.identity.type);
}

}  // namespace

std::string_view describe(CacheType type) noexcept {
  switch (type) {
    case CacheType::kData:
      return "Data";
    case CacheType::kInstruction:
      return "Instruction";
    case CacheType::kUnified:
      return "Unified";
  }
  // Reached only for a value outside the enumeration, which an enum's value
  // range permits. Saying so beats a blank a report would render as nothing.
  return "unrecognised cache type";
}

core::Result<std::uint64_t, std::string_view> parse_cache_size(std::string_view text) {
  if (text.empty()) {
    return std::string_view{"a cache size is never written blank"};
  }

  // Split the digits from the suffix by hand rather than reaching for
  // core::split_number_and_suffix, which would also accept the leading
  // whitespace and '+' that its configuration callers want and the kernel never
  // writes.
  std::size_t digits = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    ++digits;
  }
  if (digits == 0) {
    return std::string_view{"a cache size must begin with a digit"};
  }

  std::uint64_t scale = 1;
  const std::string_view suffix = text.substr(digits);
  if (suffix == "K") {
    scale = 1024;
  } else if (!suffix.empty() && suffix != "B") {
    return std::string_view{"a cache size ends in K, B or nothing"};
  }

  std::uint64_t value = 0;
  const std::uint64_t ceiling = kMaxCacheBytes / scale;
  for (std::size_t i = 0; i < digits; ++i) {
    // Bounded before the multiply, not after: the check has to keep the value
    // in range rather than notice it left. See journal §4.6 -- the same guard
    // written the other way round was undefined behaviour that a test missed.
    const auto digit = static_cast<std::uint64_t>(text[i] - '0');
    if (value > (ceiling - digit) / 10) {
      return std::string_view{"a cache size larger than any cache"};
    }
    value = value * 10 + digit;
  }
  return value * scale;
}

core::Result<CacheHierarchy, DiscoveryFailure> CacheHierarchy::discover(
    platform::FileSystem& filesystem, const CpuTopology& cpus, std::string_view sysfs_root) {
  // Read everything first, check afterwards. Each check below needs every
  // reading to exist before it can be made: a pair is not answerable until both
  // halves are in, and half-checked pairs are how an inconsistency slips
  // through. The same shape as CpuTopology::discover, for the same reason.
  std::vector<Reading> readings;
  for (const LogicalCpu& cpu : cpus.cpus()) {
    for (std::uint32_t index = 0; index < kMaxCacheIndex; ++index) {
      auto reading = read_index(filesystem, index_directory(sysfs_root, cpu.id, index), cpu.id);
      if (!reading.has_value()) {
        return reading.error();
      }
      // Bound once and checked once. Written as `*reading.value()` after a
      // check on `reading.value().has_value()`, clang-tidy reports an
      // unchecked access: it cannot see that two calls to value() return the
      // same optional. A named reference is both clearer and provably checked.
      const std::optional<Cache>& found = reading.value();
      if (!found.has_value()) {
        break;  // The first absent index is the end of this CPU's list.
      }
      readings.push_back(Reading{cpu.id, *found});
    }
  }

  if (auto failure = check_served_cpus_online(readings, cpus, sysfs_root)) {
    return *failure;
  }
  if (auto failure = check_nesting(readings, sysfs_root)) {
    return *failure;
  }

  auto distinct = deduplicate(readings, sysfs_root);
  if (!distinct.has_value()) {
    return distinct.error();
  }
  std::vector<Cache> caches = distinct.value();
  std::sort(caches.begin(), caches.end(), ordered_before);
  return CacheHierarchy{std::move(caches)};
}

std::size_t CacheHierarchy::count_at_level(std::uint32_t level) const noexcept {
  return static_cast<std::size_t>(
      std::count_if(caches_.begin(), caches_.end(),
                    [level](const Cache& cache) { return cache.identity.level == level; }));
}

std::uint32_t CacheHierarchy::deepest_level() const noexcept {
  std::uint32_t deepest = 0;
  for (const Cache& cache : caches_) {
    deepest = std::max(deepest, cache.identity.level);
  }
  return deepest;
}

std::uint64_t CacheHierarchy::total_bytes_at_level(std::uint32_t level) const noexcept {
  std::uint64_t total = 0;
  for (const Cache& cache : caches_) {
    if (cache.identity.level == level) {
      total += cache.size_bytes;
    }
  }
  return total;
}

}  // namespace loadforge::topology
