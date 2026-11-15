#pragma once

#include <string>

#include "sim/simulation.hpp"

namespace sim {

// Serializes the current simulation state to a deterministic JSON document
// (2-space indent, nlohmann::json dump). Object keys are emitted in stable
// (alphabetical) order, units are emitted in ascending id order, and all
// floating-point values pass through a locale-independent fixed-precision
// preparation (see docs/determinism.md). Throws std::runtime_error if any
// exported value is not finite. Performs no file I/O — callers write the
// returned string wherever they need it.
[[nodiscard]] std::string export_state_json(const Simulation& simulation);

}  // namespace sim
