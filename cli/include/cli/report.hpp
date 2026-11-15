#pragma once

#include <cstdint>
#include <string>

#include "sim/events.hpp"
#include "sim/simulation.hpp"
#include "sim/timeline.hpp"
#include "sim/unit_types.hpp"

namespace cli {

// Context printed in the report header.
struct ReportMeta {
  std::string scenario;
  std::uint64_t seed = 0;
};

// The full battle report (M10 Part 2a). Every sentence and number is
// derived from recorded simulation state (result, world, squads,
// timeline samples, recorded events); nothing is invented. The same
// battle always generates the same report.
[[nodiscard]] std::string generate_report_markdown(
    const sim::Simulation& simulation, const sim::EventRecorder* events,
    const sim::UnitTypeTable& types, const ReportMeta& meta);

// The same data as a JSON object (machine-readable twin of the
// markdown report).
[[nodiscard]] std::string generate_report_json(
    const sim::Simulation& simulation, const sim::EventRecorder* events,
    const sim::UnitTypeTable& types, const ReportMeta& meta);

// Timeline CSV: one row per sampled tick (tick, per faction alive /
// routed / dead / average morale / engagements, then alive per unit
// type per faction).
[[nodiscard]] std::string generate_timeline_csv(
    const sim::Simulation& simulation, const sim::UnitTypeTable& types);

// Report PNG: casualty and average-morale curves drawn with the
// software rasterizer (render::). Returns false when the file cannot
// be written.
[[nodiscard]] bool write_report_png(const std::string& path,
                                    const sim::Simulation& simulation);

}  // namespace cli
