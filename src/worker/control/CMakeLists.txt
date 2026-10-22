# SPDX-License-Identifier: GPL-3.0-or-later
# Build fragment (ADR 0004): src/worker/control and nothing else.
#
# The encodings of worker/control.hpp: what the controller tells a running
# worker and what the worker answers, as bytes. Both ends link this one
# library; the behaviour the messages ask for lives in the worker's loop
# (src/worker/) and in the controller (src/controller/).
loadforge_component(loadforge_worker_control
  SOURCES control.cpp)
