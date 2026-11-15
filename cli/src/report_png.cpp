#include <algorithm>
#include <cmath>
#include <string>

#include "cli/report.hpp"
#include "render/draw_list.hpp"
#include "render/png_writer.hpp"
#include "render/raster.hpp"
#include "sim/version.hpp"
#include "game/theme.hpp"

namespace cli {
namespace {

constexpr game::Color kWhite{255, 255, 255, 255};
constexpr game::Color kInk{40, 40, 40, 255};
constexpr game::Color kGrid{210, 210, 210, 255};
constexpr game::Color kRed{200, 60, 60, 255};
constexpr game::Color kBlue{60, 90, 200, 255};

// One panel: axes, gridlines, labels and the two faction polylines.
void draw_panel(render::DrawList& list, double x0, double y0, double x1,
                double y1, std::uint64_t max_tick, double max_value,
                const std::string& title,
                const std::vector<std::pair<std::uint64_t, double>>& red,
                const std::vector<std::pair<std::uint64_t, double>>& blue,
                const char* y_unit) {
  const double w = x1 - x0;
  const double h = y1 - y0;
  // Frame + gridlines (5 horizontal bands).
  list.rect(x0, y0, w, h, kWhite, true);
  for (int g = 0; g <= 4; ++g) {
    const double y = y0 + h * g / 4.0;
    list.line(x0, y, x1, y, 1, kGrid);
  }
  list.line(x0, y1, x1, y1, 1, kInk);
  list.line(x0, y0, x0, y1, 1, kInk);
  // Y labels: 0 .. max_value in 4 steps.
  for (int g = 0; g <= 4; ++g) {
    const double value = max_value * (4 - g) / 4.0;
    char label[32];
    std::snprintf(label, sizeof(label), "%.0f", value);
    list.text(x0 - 34, y0 + h * g / 4.0 - 3, 7, label, kInk);
  }
  // X labels: 0, half, max tick.
  char label[32];
  std::snprintf(label, sizeof(label), "%llu",
                static_cast<unsigned long long>(max_tick));
  list.text(x1 - 22, y1 + 6, 7, label, kInk);
  list.text(x0, y1 + 6, 7, "0", kInk);
  // Title + legend.
  list.text(x0, y0 - 16, 7, title, kInk);
  list.text(x0, y0 - 28, 7, y_unit, kInk);
  const auto polyline = [&](const std::vector<std::pair<std::uint64_t,
                                                          double>>& points,
                            game::Color color) {
    for (std::size_t i = 1; i < points.size(); ++i) {
      const auto& a = points[i - 1];
      const auto& b = points[i];
      const double ax =
          x0 + w * (max_tick > 0 ? static_cast<double>(a.first) /
                                       static_cast<double>(max_tick)
                                 : 0.0);
      const double ay = y1 - h * (max_value > 0.0 ? a.second / max_value
                                                  : 0.0);
      const double bx =
          x0 + w * (max_tick > 0 ? static_cast<double>(b.first) /
                                       static_cast<double>(max_tick)
                                 : 0.0);
      const double by = y1 - h * (max_value > 0.0 ? b.second / max_value
                                                  : 0.0);
      list.line(ax, ay, bx, by, 2, color);
    }
  };
  polyline(red, kRed);
  polyline(blue, kBlue);
  // Legend: swatch + name, top right of the panel.
  list.line(x1 - 96, y0 - 22, x1 - 76, y0 - 22, 2, kRed);
  list.text(x1 - 72, y0 - 26, 7, "Red", kInk);
  list.line(x1 - 44, y0 - 22, x1 - 24, y0 - 22, 2, kBlue);
  list.text(x1 - 20, y0 - 26, 7, "Blue", kInk);
}

}  // namespace

bool write_report_png(const std::string& path,
                      const sim::Simulation& simulation) {
  const std::vector<sim::TimelineRow>& rows = simulation.timeline();
  if (rows.empty()) {
    return false;
  }
  const std::uint64_t max_tick = rows.back().tick;
  std::vector<std::pair<std::uint64_t, double>> dead[2];
  std::vector<std::pair<std::uint64_t, double>> morale[2];
  double max_dead = 0.0;
  for (const sim::TimelineRow& row : rows) {
    for (std::size_t f = 0; f < 2; ++f) {
      dead[f].emplace_back(row.tick,
                           static_cast<double>(row.factions[f].dead));
      max_dead = std::max(max_dead, static_cast<double>(row.factions[f].dead));
      const sim::FactionTimeline& side = row.factions[f];
      morale[f].emplace_back(
          row.tick, side.morale_n > 0 ? side.morale_sum / side.morale_n
                                      : 0.0);
    }
  }
  if (max_dead < 1.0) {
    max_dead = 1.0;  // keep the scale readable in bloodless battles
  }

  render::DrawList list;
  list.width = 860;
  list.height = 660;
  list.background = kWhite;
  list.rect(0, 0, list.width, list.height, kWhite, true);

  char header[128];
  std::snprintf(header, sizeof(header), "Battle report — sim version %s",
                sim::kSimVersion);
  list.text(16, 10, 7, header, kInk);

  draw_panel(list, 70, 70, 840, 300, max_tick, max_dead,
             "Casualties over time", dead[0], dead[1],
             "dead units per faction (timeline samples)");
  draw_panel(list, 70, 380, 840, 610, max_tick, 100.0,
             "Average morale over time", morale[0], morale[1],
             "mean morale 0-100 per faction");

  const render::Image image = render::rasterize(list);
  return render::write_png(image, path);
}

}  // namespace cli
