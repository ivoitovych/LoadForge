// SPDX-License-Identifier: GPL-3.0-or-later
//
// T8, the real-against-real tier of ADR 0003: the telemetry sources, the
// sampler, the timeline, the statistics, the result and both writers, as built
// by their own streams, run together over the kernel's files as fixtures.

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <string>

#include "controller/results/aggregate.hpp"
#include "machine.hpp"
#include "main/run_result.hpp"
#include "main/telemetry_rig.hpp"
#include "platform/fs.hpp"
#include "reporters/csv/csv_writer.hpp"
#include "reporters/json/json_writer.hpp"
#include "statistics/summarizer.hpp"
#include "telemetry/capability/capability.hpp"

namespace loadforge::m3 {
namespace {

TEST(Experiment, Probe) {
  Machine machine;
  machine.install_everything();
  platform::FileSystem files(machine.syscalls());
  auto rig = cli::make_rig(machine.syscalls(), files, cli::MachineRoots{}, 1000);
  ASSERT_TRUE(rig.has_value());
  std::ostringstream text;
  text << telemetry::render_capabilities(rig.value()->capabilities);
  for (const auto& source : rig.value()->capabilities.sources) {
    for (const auto& m : source.metrics) {
      text << source.source << " " << m.metric.name << " unit "
           << telemetry::describe(m.metric.unit) << " scope " << telemetry::describe(m.metric.scope)
           << "/" << m.metric.scope_index << (m.unavailable ? " UNAVAILABLE" : " present") << "\n";
    }
  }
  std::cout << text.str();
  std::int64_t t = 1'000'000'000;
  for (int i = 0; i < 3; ++i) {
    auto n = rig.value()->sampler->round(t);
    ASSERT_TRUE(n.has_value());
    std::cout << "round appended " << n.value() << "\n";
    t += 1'000'000'000;
  }
  statistics::CompensatedSummarizer summarizer;
  auto sig = summarizer.summarize(rig.value()->timeline, {1'000'000'000, t});
  ASSERT_TRUE(sig.has_value());
  for (const auto& m : sig.value().metrics) {
    std::cout << m.metric.name << " " << m.metric.scope_index << " idx=" << m.value.index() << "\n";
  }
}

}  // namespace
}  // namespace loadforge::m3
