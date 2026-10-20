// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_CONTROLLER_MACHINE_TREE_HPP
#define LOADFORGE_TESTS_UNIT_CONTROLLER_MACHINE_TREE_HPP

// A /sys the controller's tests describe machines with, built on disk and read
// by the real snapshot code through the real seam (the topology module's tests
// say why a fake would agree with whatever the reader believes): N online CPUs,
// each on a core of its own in package 0, optionally one level-1 data cache per
// CPU with a line size and optionally the one NUMA node holding them all.

#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/snapshot.hpp"

namespace loadforge::controller_testing {

struct MachineShape {
  std::size_t cpus = 2;
  std::optional<std::uint32_t> line_bytes = 64;  ///< None: no cache directories at all.
  bool numa = true;
};

class MachineTree {
 public:
  explicit MachineTree(const MachineShape& shape)
      : root_(std::filesystem::temp_directory_path() /
              ("loadforge-machine-" + std::to_string(::getpid()) + "-" +
               std::to_string(++serial()))) {
    const std::filesystem::path cpu = root_ / "devices" / "system" / "cpu";
    std::filesystem::create_directories(cpu);
    write(cpu / "online", "0-" + std::to_string(shape.cpus - 1));
    for (std::size_t id = 0; id < shape.cpus; ++id) {
      const std::filesystem::path mine = cpu / ("cpu" + std::to_string(id));
      std::filesystem::create_directories(mine / "topology");
      write(mine / "topology" / "physical_package_id", "0");
      write(mine / "topology" / "core_id", std::to_string(id));
      write(mine / "topology" / "thread_siblings_list", std::to_string(id));
      if (shape.line_bytes.has_value()) {
        const std::filesystem::path cache = mine / "cache" / "index0";
        std::filesystem::create_directories(cache);
        write(cache / "level", "1");
        write(cache / "type", "Data");
        write(cache / "size", "32K");
        write(cache / "coherency_line_size", std::to_string(*shape.line_bytes));
        write(cache / "shared_cpu_list", std::to_string(id));
      }
    }
    if (shape.numa) {
      const std::filesystem::path node = root_ / "devices" / "system" / "node";
      std::filesystem::create_directories(node / "node0");
      write(node / "online", "0");
      write(node / "node0" / "cpulist", "0-" + std::to_string(shape.cpus - 1));
      write(node / "node0" / "meminfo", "Node 0 MemTotal:        1048576 kB");
      write(node / "node0" / "distance", "10");
    }
  }
  ~MachineTree() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  MachineTree(const MachineTree&) = delete;
  MachineTree& operator=(const MachineTree&) = delete;
  MachineTree(MachineTree&&) = delete;
  MachineTree& operator=(MachineTree&&) = delete;

  [[nodiscard]] core::Result<topology::Snapshot, topology::DiscoveryFailure> snapshot() const {
    platform::RealSyscalls syscalls;
    platform::FileSystem filesystem(syscalls);
    return topology::take_snapshot(filesystem, root_.string());
  }

  /// The snapshot, or a failed test: for a tree the test built to be accepted.
  [[nodiscard]] topology::Snapshot accepted() const { return snapshot().value(); }

 private:
  static int& serial() {
    static int counter = 0;
    return counter;
  }
  static void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path);
    out << text << "\n";
  }
  std::filesystem::path root_;
};

}  // namespace loadforge::controller_testing

#endif
