// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_VERIFICATION_ISOLATION_LAYOUTS_HPP
#define LOADFORGE_TESTS_UNIT_VERIFICATION_ISOLATION_LAYOUTS_HPP

// Machine layouts for choosing the other core, written as sysfs trees and read
// by the real topology discovery, so every layout is one discovery accepts:
// its cross-checks (siblings agree, caches nest, NUMA partitions the CPUs)
// hold of each tree, or the snapshot is refused and the test fails.
//
// Where each layout comes from is said beside it. The symmetric one is this
// stream's machine, read from its /sys; the others follow the numbering and
// sharing the kernel publishes for the parts they are named after, and the
// precedence layouts exist to separate one rule of the order from the next.
// The real machine's own /sys is read as well (placement_test.cpp), so the
// trees are checked against a kernel at least once (F21).

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/fs.hpp"
#include "platform/syscalls.hpp"
#include "topology/cpu_topology.hpp"
#include "topology/snapshot.hpp"

namespace loadforge::verification::isolation::layouts {

/// A sysfs tree under a directory of its own, removed when it goes.
class Tree {
 public:
  explicit Tree(const std::string& name)
      : root_(std::filesystem::temp_directory_path() /
              ("loadforge-isolation-" + std::to_string(::getpid()) + "-" + name)) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(cpu_root());
  }
  Tree(const Tree&) = delete;
  Tree(Tree&&) = delete;
  Tree& operator=(const Tree&) = delete;
  Tree& operator=(Tree&&) = delete;
  ~Tree() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  /// `count` CPUs, 0 to count-1, online.
  void online(std::uint32_t count) {
    write(cpu_root() / "online", count == 1 ? "0" : "0-" + std::to_string(count - 1));
  }

  void cpu(std::uint32_t id, std::uint32_t package, std::uint32_t core,
           const std::string& siblings) {
    const auto topology = cpu_dir(id) / "topology";
    write(topology / "physical_package_id", std::to_string(package));
    write(topology / "core_id", std::to_string(core));
    write(topology / "thread_siblings_list", siblings);
  }

  /// One cache, published identically through every CPU it serves -- the
  /// kernel's own way, which discovery checks.
  void cache(std::uint32_t level, const std::string& type, const std::vector<std::uint32_t>& cpus,
             const std::string& shared, const std::string& size) {
    for (const std::uint32_t id : cpus) {
      const std::uint32_t index = next_index_[id]++;
      const auto dir = cpu_dir(id) / "cache" / ("index" + std::to_string(index));
      write(dir / "level", std::to_string(level));
      write(dir / "type", type);
      write(dir / "shared_cpu_list", shared);
      write(dir / "size", size);
      write(dir / "coherency_line_size", "64");
    }
  }

  /// NUMA nodes 0 to n-1, node i local to cpulists[i], each 10 from itself
  /// and 20 from the others.
  void nodes(const std::vector<std::string>& cpulists) {
    const auto count = static_cast<std::uint32_t>(cpulists.size());
    write(node_root() / "online", count == 1 ? "0" : "0-" + std::to_string(count - 1));
    for (std::uint32_t id = 0; id < count; ++id) {
      const auto dir = node_root() / ("node" + std::to_string(id));
      std::string distances;
      for (std::uint32_t other = 0; other < count; ++other) {
        distances += (other == 0 ? "" : " ") + std::string(other == id ? "10" : "20");
      }
      write(dir / "cpulist", cpulists[id]);
      write(dir / "distance", distances);
      write(dir / "meminfo", "Node " + std::to_string(id) + " MemTotal:        8000000 kB");
    }
  }

  [[nodiscard]] core::Result<topology::Snapshot, topology::DiscoveryFailure> take() {
    return topology::take_snapshot(filesystem_, root_.string());
  }

 private:
  [[nodiscard]] std::filesystem::path cpu_root() const {
    return root_ / "devices" / "system" / "cpu";
  }
  [[nodiscard]] std::filesystem::path node_root() const {
    return root_ / "devices" / "system" / "node";
  }
  [[nodiscard]] std::filesystem::path cpu_dir(std::uint32_t id) const {
    return cpu_root() / ("cpu" + std::to_string(id));
  }
  static void write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << contents << "\n";
  }

  std::filesystem::path root_;
  std::map<std::uint32_t, std::uint32_t> next_index_;
  platform::RealSyscalls syscalls_;
  platform::FileSystem filesystem_{syscalls_};
};

/// Private L1 data, L1 instruction and L2 for one core's CPUs.
inline void private_caches(Tree& tree, const std::vector<std::uint32_t>& cpus,
                           const std::string& shared, const std::string& l1d,
                           const std::string& l1i, const std::string& l2) {
  tree.cache(1, "Data", cpus, shared, l1d);
  tree.cache(1, "Instruction", cpus, shared, l1i);
  tree.cache(2, "Unified", cpus, shared, l2);
}

/// Symmetric: the machine this stream was written on, read from its /sys
/// (x86-64, 4 CPUs; `cat /sys/devices/system/cpu/cpu*/topology/*` and
/// `cache/index*/{level,type,shared_cpu_list,size}`). Four cores, no SMT,
/// private 48K L1d, 32K L1i and 2048K L2, one 266240K L3, one node.
inline void symmetric(Tree& tree) {
  tree.online(4);
  for (std::uint32_t id = 0; id < 4; ++id) {
    tree.cpu(id, 0, id, std::to_string(id));
    private_caches(tree, {id}, std::to_string(id), "48K", "32K", "2048K");
  }
  tree.cache(3, "Unified", {0, 1, 2, 3}, "0-3", "266240K");
  tree.nodes({"0-3"});
}

/// SMT: four cores of two threads, numbered as x86 Linux numbers them -- the
/// first thread of every core, then the second (cpu N and N+4 are siblings).
/// Each core's L1 and L2 shared by its two threads, one L3, one node.
inline void smt(Tree& tree) {
  tree.online(8);
  for (std::uint32_t core = 0; core < 4; ++core) {
    const std::string siblings = std::to_string(core) + "," + std::to_string(core + 4);
    tree.cpu(core, 0, core, siblings);
    tree.cpu(core + 4, 0, core, siblings);
    private_caches(tree, {core, core + 4}, siblings, "32K", "32K", "1024K");
  }
  tree.cache(3, "Unified", {0, 1, 2, 3, 4, 5, 6, 7}, "0-7", "16384K");
  tree.nodes({"0-7"});
}

/// Heterogeneous, Arm DynamIQ big.LITTLE: four LITTLE cores (cpu 0-3; 32K L1s,
/// 128K L2) and four big ones (cpu 4-7; 64K L1s, 512K L2), LITTLE numbered
/// first as Linux numbers them on these parts, one L3 for all eight, no SMT.
inline void big_little(Tree& tree) {
  tree.online(8);
  for (std::uint32_t id = 0; id < 8; ++id) {
    tree.cpu(id, 0, id, std::to_string(id));
    const bool big = id >= 4;
    private_caches(tree, {id}, std::to_string(id), big ? "64K" : "32K", big ? "64K" : "32K",
                   big ? "512K" : "128K");
  }
  tree.cache(3, "Unified", {0, 1, 2, 3, 4, 5, 6, 7}, "0-7", "2048K");
  tree.nodes({"0-7"});
}

/// Heterogeneous, x86 hybrid: two performance cores with SMT, siblings
/// adjacent (cpu 0-1, 2-3; 48K L1d, 32K L1i, 1280K L2 per core), then eight
/// efficiency cores without SMT (cpu 4-11; 32K L1d, 64K L1i) in two clusters
/// of four, each cluster sharing a 2048K L2; one L3 for all.
inline void hybrid(Tree& tree) {
  tree.online(12);
  for (std::uint32_t core = 0; core < 2; ++core) {
    const std::uint32_t first = core * 2;
    const std::string siblings = std::to_string(first) + "-" + std::to_string(first + 1);
    tree.cpu(first, 0, first * 4, siblings);
    tree.cpu(first + 1, 0, first * 4, siblings);
    private_caches(tree, {first, first + 1}, siblings, "48K", "32K", "1280K");
  }
  for (std::uint32_t id = 4; id < 12; ++id) {
    tree.cpu(id, 0, 8 + id, std::to_string(id));
    tree.cache(1, "Data", {id}, std::to_string(id), "32K");
    tree.cache(1, "Instruction", {id}, std::to_string(id), "64K");
  }
  tree.cache(2, "Unified", {4, 5, 6, 7}, "4-7", "2048K");
  tree.cache(2, "Unified", {8, 9, 10, 11}, "8-11", "2048K");
  tree.cache(3, "Unified", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, "0-11", "25600K");
  tree.nodes({"0-11"});
}

/// One core, one thread: nothing else to move to.
inline void single_core(Tree& tree) {
  tree.online(1);
  tree.cpu(0, 0, 0, "0");
  private_caches(tree, {0}, "0", "32K", "32K", "1024K");
  tree.nodes({"0"});
}

/// One core, two threads: a second CPU, and still no other core.
inline void single_core_smt(Tree& tree) {
  tree.online(2);
  tree.cpu(0, 0, 0, "0-1");
  tree.cpu(1, 0, 0, "0-1");
  private_caches(tree, {0, 1}, "0-1", "32K", "32K", "1024K");
  tree.nodes({"0-1"});
}

/// Two packages of two cores, each package its own L3 and its own node; core
/// ids restart at 0 in each package, as they do on real multi-socket parts.
inline void two_sockets(Tree& tree) {
  tree.online(4);
  for (std::uint32_t id = 0; id < 4; ++id) {
    tree.cpu(id, id / 2, id % 2, std::to_string(id));
    private_caches(tree, {id}, std::to_string(id), "32K", "32K", "1024K");
  }
  tree.cache(3, "Unified", {0, 1}, "0-1", "32768K");
  tree.cache(3, "Unified", {2, 3}, "2-3", "32768K");
  tree.nodes({"0-1", "2-3"});
}

/// Precedence of the node over the cache: cpu 1 shares an L2 cluster with cpu
/// 0 but sits on the other node; cpu 2 shares nothing with cpu 0 but its node.
inline void node_before_cache(Tree& tree) {
  tree.online(4);
  for (std::uint32_t id = 0; id < 4; ++id) {
    tree.cpu(id, 0, id, std::to_string(id));
    tree.cache(1, "Data", {id}, std::to_string(id), "32K");
  }
  tree.cache(2, "Unified", {0, 1}, "0-1", "1024K");
  tree.cache(2, "Unified", {2, 3}, "2-3", "1024K");
  tree.nodes({"0,2", "1,3"});
}

/// Precedence of the cache over the shape, and of the shape over the id: cpu 1
/// shares an L3 with cpu 0 but has a larger L1d; cpu 2 and 3 share no cache
/// with cpu 0, and cpu 3 alone is shaped like it.
inline void cache_before_shape(Tree& tree) {
  tree.online(4);
  for (std::uint32_t id = 0; id < 4; ++id) {
    tree.cpu(id, 0, id, std::to_string(id));
    const bool like_cpu0 = id == 0 || id == 3;
    private_caches(tree, {id}, std::to_string(id), like_cpu0 ? "32K" : "64K", "32K", "512K");
  }
  tree.cache(3, "Unified", {0, 1}, "0-1", "8192K");
  tree.cache(3, "Unified", {2, 3}, "2-3", "8192K");
  tree.nodes({"0-3"});
}

/// The shape counts threads: three cores alike in every cache, core 0 (cpu 0
/// and 3) and core 2 (cpu 2 and 4) with two threads, core 1 (cpu 1) with one,
/// as a core reads once its second thread has been taken offline.
inline void uneven_smt(Tree& tree) {
  tree.online(5);
  const std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> cores = {
      {0, {0, 3}}, {1, {1}}, {2, {2, 4}}};
  for (const auto& [core, cpus] : cores) {
    std::string siblings;
    for (const std::uint32_t id : cpus) {
      siblings += (siblings.empty() ? "" : ",") + std::to_string(id);
    }
    for (const std::uint32_t id : cpus) {
      tree.cpu(id, 0, core, siblings);
    }
    private_caches(tree, cpus, siblings, "32K", "32K", "1024K");
  }
  tree.cache(3, "Unified", {0, 1, 2, 3, 4}, "0-4", "8192K");
  tree.nodes({"0-4"});
}

/// The shape is the core's own caches, not its cluster's: six cores in three
/// clusters, each cluster sharing an L2 -- 1024K, 2048K, 1024K -- and every
/// core with a 32K L1d but cpu 1 and 5, whose L1d is 64K. Cpu 2 and 3 are
/// cpu 0's kind in a cluster of another size.
inline void clusters_of_two_sizes(Tree& tree) {
  tree.online(6);
  for (std::uint32_t id = 0; id < 6; ++id) {
    tree.cpu(id, 0, id, std::to_string(id));
    tree.cache(1, "Data", {id}, std::to_string(id), id == 1 || id == 5 ? "64K" : "32K");
  }
  tree.cache(2, "Unified", {0, 1}, "0-1", "1024K");
  tree.cache(2, "Unified", {2, 3}, "2-3", "2048K");
  tree.cache(2, "Unified", {4, 5}, "4-5", "1024K");
  tree.nodes({"0-5"});
}

/// CPUs alone: no cache directory and no node directory, as a kernel that
/// exposes neither writes it.
inline void cpus_only(Tree& tree) {
  tree.online(3);
  for (std::uint32_t id = 0; id < 3; ++id) {
    tree.cpu(id, 0, id, std::to_string(id));
  }
}

}  // namespace loadforge::verification::isolation::layouts

#endif
