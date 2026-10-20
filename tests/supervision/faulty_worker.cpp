// SPDX-License-Identifier: GPL-3.0-or-later
//
// A worker executable that is the shipped worker in every way but one: the
// compression workload it runs corrupts its output on purpose, in the way the
// LOADFORGE_FAULT environment variable names. A test-only second program, never
// installed, never linked into the shipped binary (tools/check-no-test-hooks.sh
// refuses its symbols there), that lets the isolation ladder run against really
// re-run processes: the controller executes THIS binary as its worker and, for
// each rung, as the re-run.
//
//   LOADFORGE_FAULT=all            every fifth unit, wherever it runs
//   LOADFORGE_FAULT=core:<cpu>     every fifth unit, but only on that CPU
//   LOADFORGE_FAULT=transient      every fifth unit of the original run, never of a re-run
//   LOADFORGE_FAULT=threads        every fifth unit computed by more than one thread
//   LOADFORGE_FAULT=input          every fifth unit's input differs when generated the second time
//   LOADFORGE_FAULT=unchecked      the oracles refuse to judge anything
//   LOADFORGE_FAULT=dense          every fifth dense_linear output has its exponents flipped

#include <sched.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/syscalls.hpp"
#include "verification/oracle.hpp"
#include "worker/entry.hpp"
#include "worker/services.hpp"
#include "workloads/workload.hpp"

namespace {

using loadforge::workloads::Workload;

struct Fault {
  enum class Kind : std::uint8_t {
    kNone,
    kAll,
    kCore,
    kTransient,
    kThreads,
    kInput,
    kUnchecked,
    kDense
  } kind = Kind::kNone;
  int cpu = -1;
  bool rerun = false;
};

class Corrupting final : public Workload {
 public:
  Corrupting(std::unique_ptr<Workload> inner, Fault fault)
      : inner_(std::move(inner)), fault_(fault) {}

  [[nodiscard]] const loadforge::workloads::Descriptor& descriptor() const noexcept override {
    return inner_->descriptor();
  }
  [[nodiscard]] const loadforge::workloads::Plan& plan() const noexcept override {
    return inner_->plan();
  }
  void generate(std::uint64_t index, std::span<std::byte> input) const noexcept override {
    inner_->generate(index, input);
    if (fault_.kind == Fault::Kind::kInput && index % 5 == 0 && !input.empty()) {
      const std::lock_guard lock(mutex_);
      if (++calls_[index] % 2 == 0) {
        input[input.size() / 2] ^= std::byte{0x01};
      }
    }
  }
  void compute(std::uint64_t index, std::span<const std::byte> input, std::span<std::byte> output,
               std::span<std::byte> scratch,
               loadforge::worker::Executor& executor) const noexcept override {
    inner_->compute(index, input, output, scratch, executor);
    if (index % 5 != 0 || !applies(executor)) {
      return;
    }
    if (fault_.kind == Fault::Kind::kDense) {
      for (std::size_t at = 7; at < output.size(); at += 8) {
        output[at] ^= std::byte{0x40};
      }
    } else if (output.size() > 40) {
      output[40] ^= std::byte{0x10};
    }
  }

 private:
  [[nodiscard]] bool applies(const loadforge::worker::Executor& executor) const noexcept {
    switch (fault_.kind) {
      case Fault::Kind::kAll:
        return true;
      case Fault::Kind::kCore:
        return fault_.cpu == sched_getcpu();
      case Fault::Kind::kTransient:
        return !fault_.rerun;
      case Fault::Kind::kThreads:
        return executor.threads() > 1;
      case Fault::Kind::kDense:
        return true;
      case Fault::Kind::kInput:
      case Fault::Kind::kUnchecked:
      case Fault::Kind::kNone:
        return false;
    }
    return false;
  }

  std::unique_ptr<Workload> inner_;
  Fault fault_;
  mutable std::mutex mutex_;
  mutable std::map<std::uint64_t, std::uint64_t> calls_;
};

/// An oracle that judges nothing.
class Unchecking final : public loadforge::verification::Oracle {
 public:
  explicit Unchecking(std::unique_ptr<loadforge::verification::Oracle> inner)
      : inner_(std::move(inner)) {}
  [[nodiscard]] std::string_view workload() const noexcept override { return inner_->workload(); }
  [[nodiscard]] loadforge::verification::Contract contract() const noexcept override {
    return inner_->contract();
  }
  [[nodiscard]] loadforge::verification::Verdict check(
      const loadforge::verification::UnitEvidence& /*evidence*/) const override {
    return loadforge::verification::NotChecked{"the unit", "this oracle declines to judge"};
  }

 private:
  std::unique_ptr<loadforge::verification::Oracle> inner_;
};

class UncheckingRegistry final : public loadforge::verification::OracleRegistry {
 public:
  explicit UncheckingRegistry(const loadforge::verification::OracleRegistry& inner)
      : inner_(&inner) {}
  [[nodiscard]] std::vector<std::string> names() const override { return inner_->names(); }
  [[nodiscard]] loadforge::core::Result<std::unique_ptr<loadforge::verification::Oracle>,
                                        loadforge::verification::OracleError>
  create(std::string_view name, const loadforge::workloads::WorkloadConfig& config,
         const loadforge::workloads::Plan& plan) const override {
    auto made = inner_->create(name, config, plan);
    if (!made.has_value()) {
      return made.error();
    }
    std::unique_ptr<loadforge::verification::Oracle> oracle =
        std::make_unique<Unchecking>(std::move(made).value());
    return oracle;
  }

 private:
  const loadforge::verification::OracleRegistry* inner_;
};

class CorruptingRegistry final : public loadforge::workloads::Registry {
 public:
  CorruptingRegistry(const loadforge::workloads::Registry& inner, Fault fault)
      : inner_(&inner), fault_(fault) {}

  [[nodiscard]] std::vector<std::string> names() const override { return inner_->names(); }
  [[nodiscard]] loadforge::core::Result<std::unique_ptr<Workload>,
                                        loadforge::workloads::WorkloadError>
  create(std::string_view name, const loadforge::workloads::WorkloadConfig& config) const override {
    auto made = inner_->create(name, config);
    if (!made.has_value()) {
      return made.error();
    }
    std::unique_ptr<Workload> workload =
        std::make_unique<Corrupting>(std::move(made).value(), fault_);
    return workload;
  }

 private:
  const loadforge::workloads::Registry* inner_;
  Fault fault_;
};

Fault read_fault(bool rerun) {
  Fault fault;
  fault.rerun = rerun;
  const char* text = std::getenv("LOADFORGE_FAULT");
  const std::string_view spec = text == nullptr ? "" : text;
  if (spec == "all") {
    fault.kind = Fault::Kind::kAll;
  } else if (spec == "transient") {
    fault.kind = Fault::Kind::kTransient;
  } else if (spec == "input") {
    fault.kind = Fault::Kind::kInput;
  } else if (spec == "unchecked") {
    fault.kind = Fault::Kind::kUnchecked;
  } else if (spec == "dense") {
    fault.kind = Fault::Kind::kDense;
  } else if (spec == "threads") {
    fault.kind = Fault::Kind::kThreads;
  } else if (spec.substr(0, 5) == "core:") {
    fault.kind = Fault::Kind::kCore;
    fault.cpu = std::atoi(std::string{spec.substr(5)}.c_str());
  }
  return fault;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }
  if (args.empty()) {
    return 2;
  }
  const bool rerun = args.front() == loadforge::worker::kRerunSwitch;
  loadforge::platform::RealSyscalls syscalls;
  const loadforge::worker::ProductionServices production;
  const Fault fault = read_fault(rerun);
  CorruptingRegistry registry(*production.services().workloads, fault);
  const UncheckingRegistry unchecking(*production.services().oracles);
  loadforge::worker::Services services = production.services();
  services.workloads = &registry;
  if (fault.kind == Fault::Kind::kUnchecked) {
    services.oracles = &unchecking;
  }
  const std::span<const std::string_view> rest = std::span{args}.subspan(1);
  if (rerun) {
    return loadforge::worker::run_rerun(syscalls, rest, std::cerr, services);
  }
  return loadforge::worker::run(syscalls, rest, std::cerr, services);
}
