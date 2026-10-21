// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LOADFORGE_WORKER_SERVICES_HPP
#define LOADFORGE_WORKER_SERVICES_HPP

// What a worker is built from, other than the system-call seam: the registry
// of workloads, the registry of their oracles, the floating-point control
// registers, the thread starter, the ISA probe and the runner's factory. Each
// is an interface of a contract (or, for the thread starter, of the executor
// stream's own), borrowed for the worker's lifetime, so that every refusal in
// the worker's start-up is reached by a double that makes it and not by a
// machine that happens to.
//
// The shipped worker uses ProductionServices. A test executable that wants a
// worker which misbehaves on purpose (the isolation ladder needs a fault that
// recurs on one core and not on another) builds its own Services around the
// real registries and runs the same worker::run with it; nothing in the
// shipped binary can be told to misbehave.

#include <memory>

#include "core/result.hpp"
#include "platform/fpenv/fpenv.hpp"
#include "primitives/simd/dispatch.hpp"
#include "verification/oracle.hpp"
#include "verification/runner.hpp"
#include "worker/executor/thread_pool.hpp"
#include "workloads/workload.hpp"

namespace loadforge::worker {

using RunnerFactory = core::Result<std::unique_ptr<verification::SliceRunner>,
                                   verification::RunnerError> (*)(const verification::RunnerSetup&);

struct Services {
  const workloads::Registry* workloads = nullptr;
  const verification::OracleRegistry* oracles = nullptr;
  fpenv::FpControl* fp = nullptr;
  Threads* threads = nullptr;
  const simd::IsaProbe* isa = nullptr;
  RunnerFactory make_runner = nullptr;
};

/// The services of the shipped binary: the built-in registries, the real
/// registers, real threads, the native ISA probe, the input-integrity runner.
/// Owns what it points at.
class ProductionServices {
 public:
  ProductionServices();
  ~ProductionServices();
  ProductionServices(const ProductionServices&) = delete;
  ProductionServices(ProductionServices&&) = delete;
  ProductionServices& operator=(const ProductionServices&) = delete;
  ProductionServices& operator=(ProductionServices&&) = delete;

  [[nodiscard]] const Services& services() const noexcept { return services_; }

 private:
  struct Owned;
  std::unique_ptr<Owned> owned_;
  Services services_;
};

}  // namespace loadforge::worker

#endif
