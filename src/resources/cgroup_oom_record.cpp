// SPDX-License-Identifier: GPL-3.0-or-later
#include "resources/cgroup_oom_record.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "resources/cgroup.hpp"
#include "resources/decimal.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/source.hpp"

namespace loadforge::resources {
namespace {

constexpr std::string_view kCountKey = "oom_kill ";

}  // namespace

std::string_view oom_count_file(CgroupVersion version) noexcept {
  return version == CgroupVersion::kV2 ? "memory.events" : "memory.oom_control";
}

core::Result<std::optional<std::uint64_t>, std::string> parse_oom_kill_count(
    const std::vector<std::string>& lines) {
  std::optional<std::uint64_t> count;
  for (const std::string& line : lines) {
    // The whole key and its one space: "oom_kill_disable 0" in memory.oom_control
    // begins with the same eight letters and is a different line.
    if (!std::string_view{line}.starts_with(kCountKey)) {
      continue;
    }
    if (count.has_value()) {
      return std::string{"oom_kill appears twice"};
    }
    count = parse_decimal(std::string_view{line}.substr(kCountKey.size()));
    if (!count.has_value()) {
      return "\"" + line + "\" does not end in a count";
    }
  }
  return count;
}

CgroupOomKillRecord::CgroupOomKillRecord(platform::FileSystem& filesystem, std::string_view root) {
  auto located = locate_memory_cgroup(filesystem, root);
  if (!located.has_value()) {
    unlocated_ = located.error();
    return;
  }
  const std::optional<MemoryCgroup>& found = located.value();
  if (!found.has_value()) {
    const std::string subject = std::string{root} + "/proc/self/cgroup";
    std::string detail =
        "this process is in no memory cgroup, so the kernel keeps no OOM-kill count for it";
    unlocated_ = source_failure(topology::Availability::kAbsent, subject, std::move(detail));
    return;
  }
  const MemoryCgroup& cgroup = *found;
  std::string path = directory_of(cgroup) + "/" + std::string{oom_count_file(cgroup.version)};
  count_.emplace(filesystem, std::move(path));
}

std::string CgroupOomKillRecord::scope() const {
  if (count_.has_value()) {
    return count_->path();
  }
  return "the memory cgroup of this process";
}

core::Result<std::uint64_t, topology::DiscoveryFailure> CgroupOomKillRecord::read() {
  if (!count_.has_value()) {
    return unlocated_;
  }
  auto lines = count_->lines();
  if (!lines.has_value()) {
    return topology::from_source(lines.error());
  }
  auto count = parse_oom_kill_count(lines.value());
  if (!count.has_value()) {
    std::string detail = count.error();
    return source_failure(topology::Availability::kMalformed, count_->path(), std::move(detail));
  }
  const std::optional<std::uint64_t>& value = count.value();
  if (!value.has_value()) {
    std::string detail = "no oom_kill line: the count arrived in Linux 4.13";
    return source_failure(topology::Availability::kAbsent, count_->path(), std::move(detail));
  }
  return *value;
}

core::Result<std::uint64_t, std::string> CgroupOomKillRecord::kills() {
  auto count = read();
  if (!count.has_value()) {
    return topology::describe(count.error());
  }
  return count.value();
}

}  // namespace loadforge::resources
