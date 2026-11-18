#include "game/settings.hpp"

#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace game {
namespace {

bool g_colorblind = false;

void fail(std::string& error, const std::string& message) {
  error = message;
  throw std::invalid_argument(message);
}

}  // namespace

bool parse_settings(const std::string& text, Settings& out,
                    std::string& error) {
  try {
    const nlohmann::json root = nlohmann::json::parse(text);
    if (!root.is_object()) {
      fail(error, "settings must be a JSON object");
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
      if (it.key() != "width" && it.key() != "height" &&
          it.key() != "fullscreen" && it.key() != "vsync" &&
          it.key() != "ui_scale" && it.key() != "colorblind") {
        fail(error, "unknown settings field '" + it.key() + "'");
      }
    }
    Settings parsed;
    if (root.contains("width")) {
      if (!root["width"].is_number_integer() || root["width"].get<int>() < 320 ||
          root["width"].get<int>() > 7680) {
        fail(error, "settings width must be an integer in [320, 7680]");
      }
      parsed.width = root["width"].get<int>();
    }
    if (root.contains("height")) {
      if (!root["height"].is_number_integer() ||
          root["height"].get<int>() < 240 ||
          root["height"].get<int>() > 4320) {
        fail(error, "settings height must be an integer in [240, 4320]");
      }
      parsed.height = root["height"].get<int>();
    }
    if (root.contains("fullscreen")) {
      if (!root["fullscreen"].is_boolean()) {
        fail(error, "settings fullscreen must be a boolean");
      }
      parsed.fullscreen = root["fullscreen"].get<bool>();
    }
    if (root.contains("vsync")) {
      if (!root["vsync"].is_boolean()) {
        fail(error, "settings vsync must be a boolean");
      }
      parsed.vsync = root["vsync"].get<bool>();
    }
    if (root.contains("ui_scale")) {
      if (!root["ui_scale"].is_number()) {
        fail(error, "settings ui_scale must be a number");
      }
      const double scale = root["ui_scale"].get<double>();
      if (!std::isfinite(scale) || scale < 0.75 || scale > 2.0) {
        fail(error, "settings ui_scale must be in [0.75, 2.0]");
      }
      parsed.ui_scale = scale;
    }
    if (root.contains("colorblind")) {
      if (!root["colorblind"].is_boolean()) {
        fail(error, "settings colorblind must be a boolean");
      }
      parsed.colorblind = root["colorblind"].get<bool>();
    }
    out = parsed;
    return true;
  } catch (const nlohmann::json::exception& error) {
    // Parse errors keep the invalid_argument message for consistency.
    return false;
  } catch (const std::invalid_argument&) {
    return false;
  }
}

std::string write_settings(const Settings& settings) {
  nlohmann::json out;
  out["width"] = settings.width;
  out["height"] = settings.height;
  out["fullscreen"] = settings.fullscreen;
  out["vsync"] = settings.vsync;
  out["ui_scale"] = settings.ui_scale;
  out["colorblind"] = settings.colorblind;
  return out.dump(2) + "\n";
}

void set_colorblind_palette(bool on) { g_colorblind = on; }

bool colorblind_palette() noexcept { return g_colorblind; }

}  // namespace game
