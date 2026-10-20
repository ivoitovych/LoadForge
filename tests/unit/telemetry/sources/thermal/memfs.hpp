// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_TESTS_UNIT_TELEMETRY_SOURCES_THERMAL_MEMFS_HPP
#define LOADFORGE_TESTS_UNIT_TELEMETRY_SOURCES_THERMAL_MEMFS_HPP

// The telemetry sources' own reading of the platform seam: a machine's /sys
// and /proc held in memory, one entry per file, each of which can be put in
// any state a file can be in (ADR 0003: this is not shared with any other
// component; the sources' tests build it for themselves).
//
// WHY NOT A TREE ON DISK. Four of the six states cannot be committed: a mode-000
// file arrives from a clone readable, and as root it stays readable; "vanishes
// mid-run" is an event, not a state; a directory where a file belongs is
// cheap to build here and awkward to keep in git; and an errno the kernel
// returns only from a driver (EIO, ENODEV) cannot be arranged on disk at all.
// So the seam is substituted (platform::Syscalls, P2), and what the files
// CONTAIN comes from trees captured on a real machine (load()), edited at
// run time by the test that needs a different state and saying how.
//
// The model is as strict as the kernel for what the sources use. open() of a
// path that is not there is ENOENT; open() of a directory succeeds and the
// first read() is EISDIR (journal 1.1); a descriptor that is not open is EBADF;
// a read delivers the file from where the last one stopped, and zero bytes at
// its end.

#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/syscall_error.hpp"
#include "platform/syscalls.hpp"

namespace loadforge::telemetry::fixtures {

class MemFs final : public platform::Syscalls {
 public:
  /// Puts a regular file in place, replacing whatever was there.
  void set(const std::string& path, std::string content) {
    Entry& entry = entries_[path];
    entry = Entry{};
    entry.content = std::move(content);
  }

  /// A directory where a reader expects a file.
  void make_directory(const std::string& path) {
    Entry& entry = entries_[path];
    entry = Entry{};
    entry.directory = true;
  }

  /// The file is gone: the next open() is ENOENT.
  void remove(const std::string& path) { entries_.erase(path); }

  [[nodiscard]] bool has(const std::string& path) const { return entries_.contains(path); }

  /// Every open() of the file fails with this errno (EACCES for a denied one).
  void fail_open(const std::string& path, int error) { entries_[path].open_error = error; }

  /// open() succeeds and every read() fails with this errno (EIO, EINTR, ...).
  void fail_read(const std::string& path, int error) { entries_[path].read_error = error; }

  /// Puts back what fail_open and fail_read took away.
  void heal(const std::string& path) {
    entries_[path].open_error = 0;
    entries_[path].read_error = 0;
  }

  /// How many times the file was opened.
  [[nodiscard]] int opens_of(const std::string& path) const {
    const auto found = opens_.find(path);
    return found == opens_.end() ? 0 : found->second;
  }

  /// Every descriptor opened was closed.
  [[nodiscard]] bool all_closed() const { return open_files_.empty(); }

  /// Loads every regular file below `directory` as `mount` + its path below
  /// `directory`, except the CAPTURED note that says how the tree was made.
  void load(const std::filesystem::path& directory, const std::string& mount = {}) {
    for (const auto& item : std::filesystem::recursive_directory_iterator(directory)) {
      if (!item.is_regular_file() || item.path().filename() == "CAPTURED") {
        continue;
      }
      std::ifstream in(item.path(), std::ios::binary);
      std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      set(mount + "/" + std::filesystem::relative(item.path(), directory).generic_string(),
          std::move(content));
    }
  }

  /// The content of a loaded or set file, for a test that edits it.
  [[nodiscard]] const std::string& content_of(const std::string& path) const {
    return entries_.at(path).content;
  }

  // --- the three calls the sources make ------------------------------------------

  core::Result<int, platform::SyscallError> open_read(const std::string& path) override {
    ++opens_[path];
    const auto found = entries_.find(path);
    if (found == entries_.end()) {
      return platform::SyscallError{ENOENT, "open", path};
    }
    if (found->second.open_error != 0) {
      return platform::SyscallError{found->second.open_error, "open", path};
    }
    const int fd = next_fd_++;
    open_files_[fd] = OpenFile{path, 0};
    return fd;
  }

  core::Result<std::size_t, platform::SyscallError> read(int fd, char* buffer,
                                                         std::size_t size) override {
    const auto open = open_files_.find(fd);
    if (open == open_files_.end()) {
      return platform::SyscallError{EBADF, "read", "descriptor " + std::to_string(fd)};
    }
    const auto found = entries_.find(open->second.path);
    if (found == entries_.end()) {
      return std::size_t{0};  // removed while open: the descriptor still reads end of file
    }
    const Entry& entry = found->second;
    if (entry.directory) {
      return platform::SyscallError{EISDIR, "read", open->second.path};
    }
    if (entry.read_error != 0) {
      return platform::SyscallError{entry.read_error, "read", open->second.path};
    }
    const std::size_t left =
        entry.content.size() - std::min(open->second.offset, entry.content.size());
    const std::size_t count = std::min(left, size);
    entry.content.copy(buffer, count, open->second.offset);
    open->second.offset += count;
    return count;
  }

  core::Result<core::Ok, platform::SyscallError> close(int fd) override {
    if (open_files_.erase(fd) == 0) {
      return platform::SyscallError{EBADF, "close", "descriptor " + std::to_string(fd)};
    }
    return core::Ok{};
  }

  // --- everything else: not a file operation, so not available here ---------------

  core::Result<std::size_t, platform::SyscallError> read_link(const std::string& path,
                                                              char* /*buffer*/,
                                                              std::size_t /*size*/) override {
    return unsupported("readlink", path);
  }
  core::Result<pid_t, platform::SyscallError> fork_process() override {
    return unsupported("fork", {});
  }
  platform::SyscallError exec(const std::string& path,
                              const std::vector<std::string>& /*argv*/) override {
    return unsupported("exec", path);
  }
  core::Result<core::Ok, platform::SyscallError> set_parent_death_signal(int /*signal*/) override {
    return unsupported("prctl", {});
  }
  pid_t parent_pid() override { return 0; }
  void start_session() override {}
  core::Result<platform::Reaped, platform::SyscallError> wait_any() override {
    return unsupported("wait", {});
  }
  core::Result<bool, platform::SyscallError> has_exited(pid_t /*pid*/) override {
    return unsupported("waitid", {});
  }
  core::Result<core::Ok, platform::SyscallError> send_signal(pid_t /*pid*/,
                                                             int /*signal*/) override {
    return unsupported("kill", {});
  }
  core::Result<platform::TimeSpec, platform::SyscallError> read_clock(int /*clock_id*/) override {
    return unsupported("clock_gettime", {});
  }
  core::Result<platform::CpuMask, platform::SyscallError> get_affinity(pid_t /*pid*/) override {
    return unsupported("sched_getaffinity", {});
  }
  core::Result<core::Ok, platform::SyscallError> set_affinity(
      pid_t /*pid*/, const platform::CpuMask& /*mask*/) override {
    return unsupported("sched_setaffinity", {});
  }
  core::Result<void*, platform::SyscallError> map_anonymous(std::size_t /*length*/,
                                                            int /*extra_flags*/) override {
    return unsupported("mmap", {});
  }
  core::Result<core::Ok, platform::SyscallError> unmap(void* /*address*/,
                                                       std::size_t /*length*/) override {
    return unsupported("munmap", {});
  }
  core::Result<core::Ok, platform::SyscallError> lock_memory(void* /*address*/,
                                                             std::size_t /*length*/) override {
    return unsupported("mlock", {});
  }
  core::Result<core::Ok, platform::SyscallError> advise_memory(void* /*address*/,
                                                               std::size_t /*length*/,
                                                               int /*advice*/) override {
    return unsupported("madvise", {});
  }
  core::Result<core::Ok, platform::SyscallError> bind_memory(
      void* /*address*/, std::size_t /*length*/, const platform::NodeMask& /*nodes*/) override {
    return unsupported("mbind", {});
  }
  core::Result<int, platform::SyscallError> create_memory_file(const std::string& name,
                                                               std::size_t /*length*/) override {
    return unsupported("memfd_create", name);
  }
  core::Result<void*, platform::SyscallError> map_shared(int /*fd*/,
                                                         std::size_t /*length*/) override {
    return unsupported("mmap", {});
  }
  core::Result<core::Ok, platform::SyscallError> inherit_across_exec(int /*fd*/) override {
    return unsupported("fcntl", {});
  }
  core::Result<platform::PipeEnds, platform::SyscallError> create_pipe() override {
    return unsupported("pipe2", {});
  }
  core::Result<std::size_t, platform::SyscallError> write(
      int /*fd*/, std::span<const std::byte> /*bytes*/) override {
    return unsupported("write", {});
  }
  core::Result<std::vector<platform::Readiness>, platform::SyscallError> poll_readable(
      const std::vector<int>& /*fds*/, int /*timeout_ms*/) override {
    return unsupported("poll", {});
  }
  core::Result<core::Ok, platform::SyscallError> ignore_signal(int /*signal*/) override {
    return unsupported("sigaction", {});
  }
  pid_t process_id() override { return 1; }
  core::Result<std::optional<platform::Reaped>, platform::SyscallError> reap_any_nonblocking()
      override {
    return unsupported("waitpid", {});
  }

 private:
  struct Entry {
    std::string content;
    bool directory = false;
    int open_error = 0;
    int read_error = 0;
  };
  struct OpenFile {
    std::string path;
    std::size_t offset = 0;
  };

  static platform::SyscallError unsupported(const char* call, const std::string& what) {
    return platform::SyscallError{ENOSYS, call, what};
  }

  std::map<std::string, Entry> entries_;
  std::map<std::string, int> opens_;
  std::map<int, OpenFile> open_files_;
  int next_fd_ = 3;
};

}  // namespace loadforge::telemetry::fixtures

#endif
