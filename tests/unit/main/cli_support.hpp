// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the command line's tests share: a machine to describe, and a run to give it.

#ifndef LOADFORGE_TESTS_UNIT_MAIN_CLI_SUPPORT_HPP
#define LOADFORGE_TESTS_UNIT_MAIN_CLI_SUPPORT_HPP

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "config/config.hpp"
#include "core/duration.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/snapshot.hpp"

namespace loadforge::cli {

/// One online CPU on its own core, no caches, no NUMA layout: the smallest
/// /sys a snapshot accepts, built once on disk for the runs over the fake.
inline const topology::Snapshot& one_cpu_machine() {
  static const topology::Snapshot machine = [] {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("loadforge-cli-sysfs-" + std::to_string(::getpid()));
    const std::filesystem::path cpu = root / "devices" / "system" / "cpu";
    std::filesystem::create_directories(cpu / "cpu0" / "topology");
    for (const auto& [file, text] :
         {std::pair{cpu / "online", "0"}, std::pair{cpu / "cpu0" / "topology" / "core_id", "0"},
          std::pair{cpu / "cpu0" / "topology" / "physical_package_id", "0"},
          std::pair{cpu / "cpu0" / "topology" / "thread_siblings_list", "0"}}) {
      std::ofstream(file) << text << "\n";
    }
    platform::RealSyscalls syscalls;
    platform::FileSystem filesystem(syscalls);
    auto taken = topology::take_snapshot(filesystem, root.string());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return std::move(taken).value();
  }();
  return machine;
}

/// Two null workloads for a short while, for driving run_stress over a fake.
inline config::Config two_workloads() {
  config::Config config;
  config.run.mode = config::Mode::kStress;
  config.run.duration = core::Duration{50};
  config.workloads.push_back(config::Workload{"first", config::Threads::all(), true, {}});
  config.workloads.push_back(config::Workload{"second", config::Threads::all(), true, {}});
  return config;
}

}  // namespace loadforge::cli

#endif
