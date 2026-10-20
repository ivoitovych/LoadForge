// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_RESOURCES_MACHINE_TREE_HPP
#define LOADFORGE_TESTS_UNIT_RESOURCES_MACHINE_TREE_HPP

#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "support/fake_syscalls.hpp"

namespace loadforge::resources::testing {

/// A machine root built on disk for one test: /proc/meminfo, /proc/self/cgroup,
/// /proc/self/mountinfo and the cgroup directories they lead to, every one a
/// real file that RealSyscalls opens and reads. Removed afterwards.
///
/// The shapes are the ones the kernel writes, read off real machines: the v1
/// layout and its mountinfo lines from the container this was written in (a
/// hybrid machine, the unified hierarchy mounted beside the v1 ones), the v2
/// lines from proc(5) and cgroup-v2.rst, and the container line from a v1
/// host's Docker without a cgroup namespace.
class MachineTree {
 public:
  explicit MachineTree(const std::string& name)
      : root_(std::filesystem::temp_directory_path() /
              ("loadforge-resources-" + std::to_string(::getpid()) + "-" + name)) {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
    std::filesystem::create_directories(root_);
  }
  ~MachineTree() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  MachineTree(const MachineTree&) = delete;
  MachineTree& operator=(const MachineTree&) = delete;
  MachineTree(MachineTree&&) = delete;
  MachineTree& operator=(MachineTree&&) = delete;

  [[nodiscard]] std::string root() const { return root_.string(); }
  /// A machine path ("/proc/meminfo") as it sits in the tree.
  [[nodiscard]] std::string at(const std::string& path) const { return root_.string() + path; }

  void write(const std::string& path, const std::string& contents) const {
    const std::filesystem::path file = root_.string() + path;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file);
    out << contents;
  }
  void directory(const std::string& path) const {
    std::filesystem::create_directories(root_.string() + path);
  }
  void remove(const std::string& path) const {
    std::error_code ignored;
    std::filesystem::remove_all(root_.string() + path, ignored);
  }
  /// The path made a directory, so reading it is a real EISDIR.
  void make_directory_of(const std::string& path) const {
    remove(path);
    directory(path);
  }

  void meminfo(std::uint64_t available_kb) const {
    write("/proc/meminfo",
          "MemTotal:       16479424 kB\nMemFree:         6702632 kB\n"
          "MemAvailable:   " +
              std::to_string(available_kb) +
              " kB\nBuffers:          361524 kB\nCached:          8193156 kB\n");
  }

  /// The pure v2 layout of a current distribution's host: the unified
  /// hierarchy at /sys/fs/cgroup, its root shown, the process in `path`.
  void v2(const std::string& path) const {
    write("/proc/self/cgroup", "0::" + path + "\n");
    write("/proc/self/mountinfo",
          "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/root rw\n"
          "35 24 0:30 / /sys/fs/cgroup rw,nosuid,nodev,noexec,relatime shared:9 - cgroup2 cgroup2 "
          "rw,nsdelegate,memory_recursiveprot\n");
    write(cgroup_v2(path) + "/cgroup.controllers", "cpuset cpu io memory pids\n");
  }
  [[nodiscard]] static std::string cgroup_v2(const std::string& path) {
    return path == "/" ? "/sys/fs/cgroup" : "/sys/fs/cgroup" + path;
  }

  /// The hybrid v1 layout this was written in: every controller on its own
  /// v1 hierarchy, the memory one at /sys/fs/cgroup/memory, and the unified
  /// hierarchy mounted beside them with no controllers. The process in
  /// `path`; each directory from it up to the mount gets the unlimited
  /// sentinel, and the leaf's memory.stat says `hierarchical` is the limit.
  void v1(const std::string& path, const std::string& hierarchical) const {
    write("/proc/self/cgroup",
          "9:name=systemd:/\n8:pids:/\n7:blkio:/\n6:freezer:/\n5:devices:/\n"
          "4:memory:" +
              path + "\n3:cpuset:/\n2:cpuacct:/\n1:cpu:/\n0::/\n");
    write("/proc/self/mountinfo",
          "37 24 0:29 / /sys/fs/cgroup rw,relatime - tmpfs tmpfs rw,mode=755\n"
          "38 37 0:30 / /sys/fs/cgroup/cpu rw,relatime - cgroup cgroup rw,cpu\n"
          "41 37 0:33 / /sys/fs/cgroup/memory rw,relatime - cgroup cgroup rw,memory\n"
          "47 37 0:39 / /sys/fs/cgroup/unified rw,relatime - cgroup2 cgroup2 rw\n");
    v1_directory(path == "/" ? "" : path, hierarchical);
  }
  void v1_directory(const std::string& relative, const std::string& hierarchical) const {
    const std::string directory = "/sys/fs/cgroup/memory" + relative;
    write(directory + "/memory.limit_in_bytes", std::string{kSentinel} + "\n");
    write(directory + "/memory.stat", "cache 1048576\nrss 2097152\nhierarchical_memory_limit " +
                                          hierarchical + "\nhierarchical_memsw_limit " +
                                          std::string{kSentinel} + "\ntotal_cache 1048576\n");
  }

  /// LONG_MAX rounded down to a 4 KiB page: what v1 prints for no limit.
  static constexpr const char* kSentinel = "9223372036854771712";

 private:
  std::filesystem::path root_;
};

/// One file the FakeSyscalls read script delivers: its contents, or an errno
/// raised by its read. The fake answers every open and serves reads from one
/// queue, so a reader that opens files in a known order can be handed a
/// different answer for each -- which is how a denial is aimed at one source
/// when the tests run as root and no real file can refuse them.
struct Served {
  std::string contents;
  int error = 0;
};

inline void serve(loadforge::testing::FakeSyscalls& syscalls, const std::vector<Served>& files) {
  std::vector<loadforge::testing::FakeSyscalls::ReadStep> steps;
  for (const Served& file : files) {
    if (file.error != 0) {
      steps.push_back({"", file.error});
    } else if (file.contents.empty()) {
      steps.push_back({"", 0});  // End of file at once: one step, or the next file starts empty.
    } else {
      steps.push_back({file.contents, 0});
      steps.push_back({"", 0});
    }
  }
  syscalls.script_reads(steps);
}

/// A v2 machine as the fake serves it, in the order the readers open its
/// files: membership, mountinfo, the directory probe.
inline const std::string kServedMembership = "0::/run.scope\n";
inline const std::string kServedMountinfo = "35 24 0:30 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n";
inline const std::string kServedControllers = "memory\n";

}  // namespace loadforge::resources::testing

#endif
