// m13 item 1 — the window shell policy implementation. See shell.hpp.
#include "render/ui/shell.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "game/camera.hpp"
#include "render/ui/demo.hpp"

namespace render::ui {

std::vector<std::string> list_scenarios(const std::string& dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".json") {
      names.push_back(entry.path().stem().string());
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::vector<std::string> list_replays(const std::string& dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().extension() == ".replay") {
      names.push_back(entry.path().stem().string());
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

game::Settings settings_from(const State& settings) {
  game::Settings out;
  for (const Widget& widget : settings.tree.widgets) {
    if (widget.id == kIdResolution) {
      switch (widget.selected) {
        case 1:
          out.width = 1600;
          out.height = 900;
          break;
        case 2:
          out.width = 1920;
          out.height = 1080;
          break;
        default:
          out.width = 1280;
          out.height = 720;
          break;
      }
    } else if (widget.id == kIdFullscreen) {
      out.fullscreen = widget.on;
    } else if (widget.id == kIdVsync) {
      out.vsync = widget.on;
    } else if (widget.id == kIdColorblind) {
      out.colorblind = widget.on;
    } else if (widget.id == kIdUiScale) {
      out.ui_scale = 0.75 + 1.25 * std::clamp(widget.value, 0.0, 1.0);
    }
  }
  return out;
}

bool settings_apply_clicked(const State& settings) {
  return settings.clicked == kIdSettingsApply;
}

SetupClicks setup_clicks(const State& setup) {
  SetupClicks clicks;
  if (setup.clicked == kIdSetupSave) {
    clicks.save = true;
  } else if (setup.clicked == kIdSetupStart) {
    clicks.start = true;
  } else if (setup.clicked.rfind("plus-", 0) == 0) {
    clicks.plus_type = setup.clicked.substr(5);
  } else if (setup.clicked.rfind("minus-", 0) == 0) {
    clicks.minus_type = setup.clicked.substr(6);
  }
  return clicks;
}

EndClicks end_clicks(const State& end) {
  EndClicks clicks;
  if (end.clicked == kIdRematch) {
    clicks.rematch = true;
  } else if (end.clicked == kIdSaveReplay) {
    clicks.save_replay = true;
  } else if (end.clicked == kIdSaveReport) {
    clicks.save_report = true;
  }
  return clicks;
}

ReplayClicks replay_clicks(const State& replay) {
  ReplayClicks clicks;
  if (replay.clicked == kIdLoad) {
    clicks.load = true;
  } else if (replay.clicked == kIdVerify) {
    clicks.verify = true;
  }
  if (clicks.load) {
    for (const Widget& widget : replay.tree.widgets) {
      if (widget.id == kIdReplayList) {
        const std::string items = widget.text;
        const int selected = std::max(0, widget.selected);
        int index = 0;
        std::size_t pos = 0;
        while (pos <= items.size()) {
          const std::size_t newline = items.find('\n', pos);
          const std::string line =
              items.substr(pos, newline == std::string::npos
                                    ? std::string::npos
                                    : newline - pos);
          if (index == selected && line != "(no .replay files)") {
            clicks.load_stem = line;
            break;
          }
          if (newline == std::string::npos) {
            break;
          }
          pos = newline + 1;
          ++index;
        }
      }
    }
  }
  return clicks;
}

bool ui_covers(const App& app, Point p) {
  const Tree& tree = app.active().tree;
  for (const Widget& widget : tree.widgets) {
    if (widget.kind == Kind::Panel && widget.chrome &&
        widget.rect.contains(p)) {
      return true;
    }
  }
  return false;
}

ArrowOwnership ui_arrow_ownership(const App& app) {
  ArrowOwnership owned;
  if (app.screen != Screen::Battle) {
    owned.left_right = true;
    owned.up_down = true;
    return owned;
  }
  const std::int32_t focus = app.battle.focus;
  if (focus < 0 ||
      focus >= static_cast<std::int32_t>(app.battle.tree.size())) {
    return owned;
  }
  const Widget& widget = app.battle.tree.at(static_cast<std::size_t>(focus));
  if (widget.kind == Kind::Slider) {
    owned.left_right = true;
  } else if (widget.kind == Kind::Select && widget.on) {
    owned.up_down = true;
  }
  return owned;
}

BattleClicks battle_clicks(const State& battle) {
  BattleClicks clicks;
  const std::string& clicked = battle.clicked;
  if (clicked == kIdPause) {
    clicks.pause = true;
  } else if (clicked == kIdPlay) {
    clicks.play = true;
  } else if (clicked == kIdSpeed1) {
    clicks.speed = 1;
  } else if (clicked == kIdSpeed2) {
    clicks.speed = 2;
  } else if (clicked == kIdSpeed4) {
    clicks.speed = 4;
  }
  return clicks;
}

double zoom_from_slider(double slider, double base) {
  slider = std::clamp(slider, 0.0, 1.0);
  const double zoom = base * std::pow(16.0, 2.0 * slider - 1.0);
  return std::clamp(zoom, game::kMinZoom, game::kMaxZoom);
}

double slider_from_zoom(double zoom, double base) {
  if (!(base > 0.0)) {
    return 0.5;
  }
  zoom = std::clamp(zoom, game::kMinZoom, game::kMaxZoom);
  const double clamped = std::clamp(zoom / base, 1.0 / 16.0, 16.0);
  // ratio = 16^(2s-1)  <=>  s = 1/2 + log16(ratio)/2 = 1/2 + log2(ratio)/8.
  const double slider = 0.5 + std::log2(clamped) / 8.0;
  return std::clamp(slider, 0.0, 1.0);
}

BattleView apply_battle_frame(const App& app, const BattleView& previous,
                              double base_zoom) {
  BattleView view = previous;
  view.options = options_from(app.battle);
  view.zoom = zoom_from_slider(zoom_from(app.battle), base_zoom);
  const BattleClicks clicks = battle_clicks(app.battle);
  if (clicks.pause) {
    view.paused = true;
  }
  if (clicks.play) {
    view.paused = false;
  }
  if (clicks.speed != 0) {
    view.speed = static_cast<double>(clicks.speed);
  }
  return view;
}

}  // namespace render::ui
