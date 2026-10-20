// SPDX-License-Identifier: GPL-3.0-or-later
//
// THE FAKE WORKER
// ---------------
// A test-only executable that speaks the worker protocol and misbehaves on
// demand (docs/IMPLEMENTATION.md §4, "Testing a multi-process supervisor"). The
// controller under test runs it as its worker executable (RunOptions::
// executable) exactly as it runs the real binary: the same argv after
// --worker, the same inherited heartbeat region, record pipe and control pipe,
// the same exit codes. It is never installed, and a release gate
// (tools/check-no-test-hooks.sh) refuses a shipped binary that carries any
// symbol from this namespace.
//
// AN INDEPENDENT READING OF THE CONTRACT (ADR 0003)
// -------------------------------------------------
// The contract is worker/entry.hpp (the argv and the exit codes) and
// worker/ipc (the heartbeat slot and the record frame). This file reads the
// argv itself rather than calling worker::parse_args, and runs its own loop
// rather than worker::NullWorkload: two readings of one contract are two
// chances to notice it is ambiguous, and the supervision tier compares them by
// running the controller against this one. What it does share is the IPC
// library, because that is the protocol's provider, not the worker's
// behaviour; it links loadforge_ipc and not loadforge_worker, so reaching for
// the real loop is a link error rather than a review comment.
//
// HOW A MISBEHAVIOUR IS SELECTED
// ------------------------------
// By the environment variable LOADFORGE_FAKE_WORKER, because the controller
// builds the argv and a test cannot add to it, while the environment passes
// through fork and exec untouched. The value is a comma-separated list of
// SLOT=NAME entries, where SLOT is a worker index or '*' for every slot not
// named; an unset or empty variable makes every worker 'complete'. So
// "*=outlast,1=segv" is: worker 1 dies by SIGSEGV, every other worker outlasts
// it. A selector this file cannot read is refused with the bad-arguments code,
// so a typo in a test is a startup failure, never a silently different test.
//
// THE CATALOGUE (one name per row of IMPLEMENTATION §4 that has a fake)
//
//   complete          The protocol, plainly: started record, a beat with
//                     progress every interval, finished record and exit 0 at
//                     the duration or on a stop byte, exit 124 on hang-up.
//   outlast           complete, and also finishes once every other worker it
//                     has seen beat has been reaped. The companion that lets a
//                     test end without a deadline racing the misbehaviour.
//   slow              outlast, advancing its progress only once per quarter of
//                     the staleness allowance: working slowly, not stuck.
//   exit0             Exit 0 with its finished record, at once.
//   exit-error        An error record, then exit 1: the worker's own failure.
//   segv, bus, abrt   Killed by SIGSEGV, SIGBUS or SIGABRT.
//   hang              Alive, silent, descriptors held open: no beat, no read.
//   spin              Beats with a fresh timestamp and never advances progress.
//   close-records     Closes its record pipe, works on, exits 0.
//   truncated-record  Writes a finished frame short by one byte, exits 0.
//   bad-checksum      Writes a finished frame with one payload bit flipped.
//   ignore-term       Ignores SIGTERM and its control pipe, beats forever.
//   orphan            Forks a grandchild that spins on a core, then exits 1.
//   deaf              Never reads its control pipe and never stops on its own:
//                     only the parent-death signal can end it.
//   allocate          Maps and touches memory until the OOM killer ends it.
//                     For the OOM tier, under a memory limit; never run bare.
//
// Every misbehaviour but complete, ignore-term and deaf starts with the same
// prelude: the started record, one beat with progress 1, and a wait until the
// controller has beaten twice more. The controller publishes its own beat and
// then judges every worker's within the same tick, so two of its beats after
// this worker's are a full tick in which the controller read it. A test can
// then assert the timeline a fault must preserve without hoping the controller
// looked in time (docs/JOURNAL.md §4.11).

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/result.hpp"
#include "platform/syscalls.hpp"
#include "worker/ipc/heartbeat.hpp"
#include "worker/ipc/record.hpp"

namespace loadforge::fake_worker {

/// The selector's name, and the one string from this executable the release
/// gate also refuses to find in a shipped binary.
constexpr std::string_view kSelector = "LOADFORGE_FAKE_WORKER";

/// The exit codes, as worker/entry.hpp documents them. Spelled out rather than
/// taken from that header: they are the contract this file reads, and a
/// reading that copied the provider's constants could not disagree with it.
constexpr int kExitWorkerError = 1;
constexpr int kExitSetupFailed = 119;
constexpr int kExitControllerLost = 124;
constexpr int kExitBadArguments = 125;

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr std::int64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::uint32_t kPhase = 1;

enum class Behaviour : std::uint8_t {
  kComplete,
  kOutlast,
  kSlow,
  kExitZero,
  kExitError,
  kSegv,
  kBus,
  kAbort,
  kHang,
  kSpin,
  kCloseRecords,
  kTruncatedRecord,
  kBadChecksum,
  kIgnoreTerm,
  kOrphan,
  kDeaf,
  kAllocate,
};

struct Named {
  std::string_view name;
  Behaviour behaviour;
};

constexpr std::array<Named, 17> kCatalogue{{
    {"complete", Behaviour::kComplete},
    {"outlast", Behaviour::kOutlast},
    {"slow", Behaviour::kSlow},
    {"exit0", Behaviour::kExitZero},
    {"exit-error", Behaviour::kExitError},
    {"segv", Behaviour::kSegv},
    {"bus", Behaviour::kBus},
    {"abrt", Behaviour::kAbort},
    {"hang", Behaviour::kHang},
    {"spin", Behaviour::kSpin},
    {"close-records", Behaviour::kCloseRecords},
    {"truncated-record", Behaviour::kTruncatedRecord},
    {"bad-checksum", Behaviour::kBadChecksum},
    {"ignore-term", Behaviour::kIgnoreTerm},
    {"orphan", Behaviour::kOrphan},
    {"deaf", Behaviour::kDeaf},
    {"allocate", Behaviour::kAllocate},
}};

std::optional<Behaviour> behaviour_named(std::string_view name) {
  for (const Named& entry : kCatalogue) {
    if (entry.name == name) {
      return entry.behaviour;
    }
  }
  return std::nullopt;
}

std::string_view name_of(Behaviour behaviour) {
  for (const Named& entry : kCatalogue) {
    if (entry.behaviour == behaviour) {
      return entry.name;
    }
  }
  return "unnamed";
}

/// What the controller told this worker, read from the argv as entry.hpp
/// documents it.
struct Args {
  std::size_t slot = 0;
  std::size_t workers = 0;
  int heartbeat_fd = -1;
  int record_fd = -1;
  int control_fd = -1;
  std::int64_t duration_ns = 0;
  std::int64_t interval_ns = 0;
  std::int64_t stale_after_ns = 0;
};

/// A whole non-negative decimal number and nothing else.
std::optional<std::uint64_t> number_of(std::string_view text) {
  std::uint64_t value = 0;
  const char* const end = text.data() + text.size();  // NOLINT(*-pointer-arithmetic)
  const auto [last, error] = std::from_chars(text.data(), end, value);
  if (text.empty() || error != std::errc{} || last != end) {
    return std::nullopt;
  }
  return value;
}

core::Result<Args, std::string> parse(std::span<const std::string_view> argv) {
  // The switch, then key/value pairs. Every key entry.hpp names is required
  // once except --cpus, which is accepted and not acted on: placement is not
  // a misbehaviour.
  constexpr std::array<std::string_view, 9> kRequired{
      "--slot",           "--workers",     "--heartbeat-fd", "--record-fd",     "--control-fd",
      "--supervisor-pid", "--duration-ms", "--interval-ms",  "--stale-after-ms"};
  if (argv.size() < 2 || argv[1] != "--worker") {
    return std::string{"expected --worker as the first argument"};
  }
  std::map<std::string_view, std::uint64_t> values;
  for (std::size_t index = 2; index < argv.size(); index += 2) {
    const std::string_view key = argv[index];
    bool known = key == "--cpus";
    for (const std::string_view required : kRequired) {
      known = known || key == required;
    }
    if (!known || index + 1 >= argv.size() || values.contains(key)) {
      return std::string{key} + ": unknown, repeated or missing its value";
    }
    if (key == "--cpus") {
      values[key] = 0;
      continue;
    }
    const auto value = number_of(argv[index + 1]);
    if (!value.has_value()) {
      return std::string{key} + ": not a whole number: " + std::string{argv[index + 1]};
    }
    values[key] = *value;
  }
  for (const std::string_view required : kRequired) {
    if (!values.contains(required)) {
      return std::string{required} + ": required";
    }
  }
  const auto descriptor = [&values](std::string_view key) -> std::optional<int> {
    const std::uint64_t value = values[key];
    return value <= static_cast<std::uint64_t>(INT_MAX)
               ? std::optional<int>{static_cast<int>(value)}
               : std::nullopt;
  };
  const auto nanoseconds = [&values](std::string_view key) -> std::optional<std::int64_t> {
    const std::uint64_t value = values[key];
    constexpr auto kLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() /
                                                       kNanosecondsPerMillisecond);
    return value >= 1 && value <= kLimit
               ? std::optional<std::int64_t>{static_cast<std::int64_t>(value) *
                                             kNanosecondsPerMillisecond}
               : std::nullopt;
  };
  Args args;
  args.workers = values["--workers"];
  args.slot = values["--slot"];
  if (args.workers == 0 || args.workers > ipc::kMaxWorkers || args.slot >= args.workers) {
    return std::string{"--slot/--workers: slot "} + std::to_string(args.slot) + " of " +
           std::to_string(args.workers);
  }
  const auto heartbeat = descriptor("--heartbeat-fd");
  const auto record = descriptor("--record-fd");
  const auto control = descriptor("--control-fd");
  const auto duration = nanoseconds("--duration-ms");
  const auto interval = nanoseconds("--interval-ms");
  const auto stale = nanoseconds("--stale-after-ms");
  if (!heartbeat || !record || !control || !duration || !interval || !stale) {
    return std::string{"a descriptor or a time is out of range"};
  }
  args.heartbeat_fd = *heartbeat;
  args.record_fd = *record;
  args.control_fd = *control;
  args.duration_ns = *duration;
  args.interval_ns = *interval;
  args.stale_after_ns = *stale;
  return args;
}

/// This slot's behaviour under `selector` (null: the variable is unset).
core::Result<Behaviour, std::string> behaviour_for(std::size_t slot, const char* selector) {
  if (selector == nullptr || *selector == '\0') {
    return Behaviour::kComplete;
  }
  std::optional<Behaviour> for_every;
  std::optional<Behaviour> for_this;
  std::string_view rest{selector};
  while (!rest.empty()) {
    const std::size_t comma = rest.find(',');
    const std::string_view entry = rest.substr(0, comma);
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    const std::size_t equals = entry.find('=');
    if (equals == std::string_view::npos) {
      return std::string{"selector entry without '=': "} + std::string{entry};
    }
    const std::string_view who = entry.substr(0, equals);
    const auto behaviour = behaviour_named(entry.substr(equals + 1));
    if (!behaviour.has_value()) {
      return std::string{"no misbehaviour named '"} + std::string{entry.substr(equals + 1)} + "'";
    }
    if (who == "*") {
      for_every = behaviour;
      continue;
    }
    const auto index = number_of(who);
    if (!index.has_value()) {
      return std::string{"selector slot is not a number or '*': "} + std::string{who};
    }
    if (*index == slot) {
      for_this = behaviour;
    }
  }
  if (for_this.has_value()) {
    return *for_this;
  }
  return for_every.value_or(Behaviour::kComplete);
}

std::int64_t monotonic_ns() {
  timespec now{};
  ::clock_gettime(CLOCK_MONOTONIC, &now);
  return (std::int64_t{now.tv_sec} * kNanosecondsPerSecond) + std::int64_t{now.tv_nsec};
}

void sleep_for_ns(std::int64_t nanoseconds) {
  timespec span{};
  span.tv_sec = nanoseconds / kNanosecondsPerSecond;
  span.tv_nsec = nanoseconds % kNanosecondsPerSecond;
  while (::nanosleep(&span, &span) != 0 && errno == EINTR) {
  }
}

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

/// Writes all of `bytes` to `fd`, or as much as the pipe takes before it
/// refuses. Used only for the frames the IPC library would never send.
void write_raw(int fd, std::span<const std::byte> bytes) {
  while (!bytes.empty()) {
    const ssize_t written = ::write(fd, bytes.data(), bytes.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    bytes = bytes.subspan(static_cast<std::size_t>(written));
  }
}

/// Dies by `signal` for certain, whatever handler a sanitizer runtime put in
/// place: docs/JOURNAL.md §1.7 -- ASan and TSan catch SIGSEGV and SIGBUS and
/// turn them into an exit code, which would make the fault rows test the
/// sanitizer instead of the controller. The disposition is reset with the raw
/// system call, below every interceptor, and the signal is sent to this thread
/// the same way. No core is written: a test that faults on purpose has no use
/// for one, and a core dump changes the status word the test reads.
[[noreturn]] void die_by(int signal) {
  const rlimit none{0, 0};
  ::setrlimit(RLIMIT_CORE, &none);
  ::prctl(PR_SET_DUMPABLE, 0);  // NOLINT(cppcoreguidelines-pro-type-vararg)
  // The kernel's struct sigaction: handler, flags, restorer, mask. All zero is
  // SIG_DFL with no flags and nothing blocked, on x86-64 and AArch64 alike.
  std::array<std::uint64_t, 4> action{};
  // The kernel's sigset is 64 bits, whatever glibc's sigset_t is.
  constexpr std::size_t kKernelSigsetBytes = 8;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  ::syscall(SYS_rt_sigaction, signal, action.data(), nullptr, kKernelSigsetBytes);
  sigset_t unblock{};
  ::sigemptyset(&unblock);
  ::sigaddset(&unblock, signal);
  ::pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  ::syscall(SYS_tgkill, ::getpid(), ::gettid(), signal);
  while (true) {
    ::pause();
  }
}

/// What one look at the control pipe found.
enum class Control : std::uint8_t { kNothing, kStop, kHungUp };

class Worker {
 public:
  Worker(platform::Syscalls& syscalls, const Args& args, ipc::HeartbeatRegion& region,
         Behaviour behaviour)
      : args_(args),
        region_(&region),
        records_(syscalls, args.record_fd),
        behaviour_(behaviour),
        pid_(::getpid()),
        peers_(args.workers, 0) {}

  int run();

 private:
  void beat(std::uint64_t progress) {
    (void)region_->publish(
        ipc::worker_slot(args_.slot),
        ipc::Heartbeat{monotonic_ns(), progress, kPhase, static_cast<std::uint32_t>(pid_)});
  }

  bool send(ipc::RecordType type, const std::string& text) {
    return records_.send(type, bytes_of(text)).has_value();
  }

  /// The controller's beat counter: it numbers its beats 1, 2, 3...
  [[nodiscard]] std::uint64_t controller_beats() const {
    const auto observed = region_->observe(ipc::kControllerSlot);
    if (!observed.has_value()) {
      return 0;
    }
    const std::optional<ipc::Heartbeat> beat = observed.value();
    return beat.has_value() ? beat->progress : 0;
  }

  [[nodiscard]] int interval_ms() const {
    return static_cast<int>(args_.interval_ns / kNanosecondsPerMillisecond);
  }

  /// One interval spent on the control pipe.
  [[nodiscard]] Control wait_on_control() const {
    pollfd entry{args_.control_fd, POLLIN, 0};
    const int ready = ::poll(&entry, 1, interval_ms());
    if (ready <= 0) {
      return Control::kNothing;
    }
    if ((entry.revents & POLLIN) != 0) {
      std::array<char, 1> byte{};
      const ssize_t count = ::read(args_.control_fd, byte.data(), byte.size());
      return count == 1 ? Control::kStop : Control::kHungUp;
    }
    return Control::kHungUp;
  }

  /// Every other worker has been seen to beat and is gone. A worker the
  /// controller has not reaped yet still answers kill(pid, 0), so "gone" here
  /// means the controller has finished with it.
  bool peers_gone() {
    bool gone = true;
    for (std::size_t other = 0; other < args_.workers; ++other) {
      if (other == args_.slot) {
        continue;
      }
      if (peers_[other] == 0) {
        const auto observed = region_->observe(ipc::worker_slot(other));
        const std::optional<ipc::Heartbeat> beat =
            observed.has_value() ? observed.value() : std::nullopt;
        if (beat.has_value()) {
          peers_[other] = static_cast<pid_t>(beat->pid);
        }
      }
      gone = gone && peers_[other] != 0 && ::kill(peers_[other], 0) != 0 && errno == ESRCH;
    }
    return gone;
  }

  [[nodiscard]] std::string started_text() const {
    return "slot " + std::to_string(args_.slot) + " pid " + std::to_string(pid_) +
           " (fake worker: " + std::string{name_of(behaviour_)} + ")";
  }

  [[nodiscard]] std::string finished_text(std::uint64_t progress) const {
    return "fake worker " + std::string{name_of(behaviour_)} + " after " +
           std::to_string(progress) + " beat(s)";
  }

  /// Started, one beat, and two controller beats after it. Returns false if
  /// the controller stopped beating first: a lost controller, as a real
  /// worker would judge it.
  bool prelude() {
    (void)send(ipc::RecordType::kStarted, started_text());
    beat(1);
    const std::uint64_t seen = controller_beats();
    const std::int64_t give_up_at = monotonic_ns() + args_.stale_after_ns;
    while (controller_beats() < seen + 2) {
      if (monotonic_ns() > give_up_at) {
        (void)send(ipc::RecordType::kError, "controller lost during the prelude");
        return false;
      }
      sleep_for_ns(args_.interval_ns / 4);
      beat(1);
    }
    return true;
  }

  int protocol(bool outlast, bool slow);
  [[noreturn]] void beat_forever(bool spin_without_progress);
  int orphan();
  int allocate();
  int corrupt_frame(bool truncate);

  Args args_;
  ipc::HeartbeatRegion* region_;
  ipc::RecordWriter records_;
  Behaviour behaviour_;
  pid_t pid_;
  std::vector<pid_t> peers_;
};

int Worker::protocol(bool outlast, bool slow) {
  (void)send(ipc::RecordType::kStarted, started_text());
  const std::int64_t started = monotonic_ns();
  std::int64_t last_step = started;
  // Progress 1 from the first beat: the controller's clock for "not
  // progressing" runs from its own start, so the first change it sees must
  // come as soon as the worker is seen, however long its exec took (a
  // sanitizer runtime under load takes hundreds of milliseconds).
  std::uint64_t progress = 1;
  while (true) {
    const std::int64_t now = monotonic_ns();
    beat(progress);
    if (now - started >= args_.duration_ns || (outlast && peers_gone())) {
      break;
    }
    const Control control = wait_on_control();
    if (control == Control::kStop) {
      break;
    }
    if (control == Control::kHungUp) {
      (void)send(ipc::RecordType::kError, "controller lost: the control pipe hung up");
      return kExitControllerLost;
    }
    if (!slow || monotonic_ns() - last_step >= args_.stale_after_ns / 4) {
      ++progress;
      last_step = monotonic_ns();
    }
  }
  if (!send(ipc::RecordType::kFinished, finished_text(progress))) {
    return kExitSetupFailed;
  }
  return 0;
}

void Worker::beat_forever(bool spin_without_progress) {
  std::uint64_t progress = 1;
  while (true) {
    beat(progress);
    if (spin_without_progress) {
      // Busy, on a core, and going nowhere: the timestamp is fresh every
      // interval and the progress never moves.
      const std::int64_t until = monotonic_ns() + args_.interval_ns;
      while (monotonic_ns() < until) {
      }
    } else {
      ++progress;
      sleep_for_ns(args_.interval_ns);
    }
  }
}

int Worker::orphan() {
  const pid_t child = ::fork();
  if (child == 0) {
    // The grandchild: holds every descriptor it inherited, the record pipe
    // among them, and a core at full load. It has no death signal -- that is
    // cleared across fork -- so nothing of the worker's own ends it.
    volatile std::uint64_t spins = 0;
    while (true) {
      spins = spins + 1;
    }
  }
  if (child < 0) {
    (void)send(ipc::RecordType::kError, "fork failed: " + std::to_string(errno));
    return kExitWorkerError;
  }
  (void)send(ipc::RecordType::kError, "forked grandchild " + std::to_string(child) + ", then died");
  return kExitWorkerError;
}

int Worker::allocate() {
  constexpr std::size_t kChunkBytes = std::size_t{64} << 20U;
  const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  std::uint64_t progress = 1;
  while (true) {
    void* chunk =
        ::mmap(nullptr, kChunkBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (chunk == MAP_FAILED) {
      (void)send(ipc::RecordType::kError, "mmap refused before the OOM killer came");
      return kExitWorkerError;
    }
    const std::span<std::byte> bytes{static_cast<std::byte*>(chunk), kChunkBytes};
    for (std::size_t offset = 0; offset < bytes.size(); offset += page) {
      bytes[offset] = std::byte{1};
    }
    beat(++progress);
  }
}

int Worker::corrupt_frame(bool truncate) {
  auto frame = ipc::encode(static_cast<std::uint16_t>(ipc::RecordType::kFinished),
                           bytes_of(finished_text(1)));
  if (!frame.has_value()) {
    return kExitWorkerError;
  }
  std::vector<std::byte> bytes = std::move(frame).value();
  if (truncate) {
    bytes.pop_back();
  } else {
    bytes.back() ^= std::byte{0x01};
  }
  write_raw(args_.record_fd, bytes);
  return 0;
}

int Worker::run() {
  switch (behaviour_) {
    case Behaviour::kComplete:
      return protocol(false, false);
    case Behaviour::kOutlast:
      return protocol(true, false);
    case Behaviour::kSlow:
      return protocol(true, true);
    case Behaviour::kIgnoreTerm: {
      std::signal(SIGTERM, SIG_IGN);
      (void)send(ipc::RecordType::kStarted, started_text());
      beat_forever(false);
    }
    case Behaviour::kDeaf:
      (void)send(ipc::RecordType::kStarted, started_text());
      beat_forever(false);
    default:
      break;
  }
  if (!prelude()) {
    return kExitControllerLost;
  }
  switch (behaviour_) {
    case Behaviour::kExitZero:
      return send(ipc::RecordType::kFinished, finished_text(1)) ? 0 : kExitSetupFailed;
    case Behaviour::kExitError:
      (void)send(ipc::RecordType::kError, "fake worker: a deliberate failure");
      return kExitWorkerError;
    case Behaviour::kSegv:
      die_by(SIGSEGV);
    case Behaviour::kBus:
      die_by(SIGBUS);
    case Behaviour::kAbort:
      die_by(SIGABRT);
    case Behaviour::kHang:
      // Alive and silent, every descriptor still open: no end of file will
      // come, so only the heartbeat can tell.
      while (true) {
        ::pause();
      }
    case Behaviour::kSpin:
      beat_forever(true);
    case Behaviour::kCloseRecords: {
      ::close(args_.record_fd);
      for (std::uint64_t progress = 2; progress < 5; ++progress) {
        sleep_for_ns(args_.interval_ns);
        beat(progress);
      }
      return 0;
    }
    case Behaviour::kTruncatedRecord:
      return corrupt_frame(true);
    case Behaviour::kBadChecksum:
      return corrupt_frame(false);
    case Behaviour::kOrphan:
      return orphan();
    case Behaviour::kAllocate:
      return allocate();
    default:
      return kExitBadArguments;  // Handled above; here only to be total.
  }
}

int run(std::span<const std::string_view> argv) {
  auto parsed = parse(argv);
  if (!parsed.has_value()) {
    std::cerr << "fake_worker: " << parsed.error() << "\n";
    return kExitBadArguments;
  }
  const Args& args = parsed.value();
  // Read once, before anything else could change the environment.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  auto behaviour = behaviour_for(args.slot, std::getenv(std::string{kSelector}.c_str()));
  if (!behaviour.has_value()) {
    std::cerr << "fake_worker: " << kSelector << ": " << behaviour.error() << "\n";
    return kExitBadArguments;
  }
  // A write to a controller that has gone is EPIPE, as for a real worker.
  std::signal(SIGPIPE, SIG_IGN);
  platform::RealSyscalls syscalls;
  auto region = ipc::HeartbeatRegion::attach(syscalls, args.heartbeat_fd, args.workers);
  if (!region.has_value()) {
    std::cerr << "fake_worker: heartbeat region: " << ipc::describe(region.error()) << "\n";
    return kExitSetupFailed;
  }
  ipc::HeartbeatRegion attached = std::move(region).value();
  Worker worker(syscalls, args, attached, behaviour.value());
  return worker.run();
}

}  // namespace loadforge::fake_worker

int main(int argc, char** argv) {
  try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    std::vector<std::string_view> args;
    args.reserve(raw.size());
    for (const char* arg : raw) {
      args.emplace_back(arg);
    }
    return loadforge::fake_worker::run(args);
  } catch (...) {
    // An allocation failure, most likely: a setup failure, as for a real worker.
    return loadforge::fake_worker::kExitSetupFailed;
  }
}
