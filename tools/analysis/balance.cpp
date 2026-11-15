// M10 — balance tool: runs one scenario for N seeds in BOTH orientations
// (normal and --swap-sides) and reports the per-orientation winner split,
// the side-averaged win rate and the orientation effect.
//
// The side-averaged win rate answers "how strong is this FACTION (color)
// on this map, averaging away which band it deploys in"; the orientation
// effect answers "how much does deploying west vs east matter". A perfectly
// symmetric setup has a 50% side-averaged rate and a 0% orientation effect.
//
// Usage (from the repository root):
//   ./build/balance --scenario default --seeds 40 [--variance on|off]
//                   [--mod DIR] [--max-ticks N]
//
// Implementation: spawns battle_cli (the same loading path as users get —
// scenarios, maps, mods) and parses its report lines.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct RunResult {
  int code = 0;
  std::string winner;
  std::string reason;
  long ticks = 0;
};

RunResult run_battle(const std::string& scenario, unsigned seed,
                     bool swapped, const std::string& variance,
                     const std::string& mod, long max_ticks) {
  const char* cli_override = std::getenv("BATTLE_CLI");
  const std::string cli = cli_override != nullptr ? cli_override
                                                   : "./build/battle_cli";
  std::string command = cli + " --scenario " + scenario +
                        " --seed " + std::to_string(seed) +
                        " --variance " + variance;
  if (swapped) {
    command += " --swap-sides";
  }
  if (!mod.empty()) {
    command += " --mod " + mod;
  }
  if (max_ticks > 0) {
    command += " --ticks " + std::to_string(max_ticks);
  }
  command += " 2>/dev/null";
  std::FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    std::fprintf(stderr, "balance: popen failed\n");
    std::exit(2);
  }
  RunResult result;
  char line[512];
  while (std::fgets(line, sizeof(line), pipe) != nullptr) {
    const std::string text(line);
    const auto grab = [&text, &result](const char* key, std::string& out) {
      const std::size_t at = text.find(key);
      if (at == std::string::npos) {
        return;
      }
      std::size_t begin = at + std::strlen(key);
      while (begin < text.size() && text[begin] == ' ') {
        ++begin;
      }
      std::size_t end = begin;
      while (end < text.size() && text[end] != '\n' && text[end] != ' ') {
        ++end;
      }
      out = text.substr(begin, end - begin);
    };
    grab("Winner:", result.winner);
    grab("Termination reason:", result.reason);
    const std::size_t at = text.find("Ticks elapsed:");
    if (at != std::string::npos) {
      result.ticks = std::strtol(text.c_str() + at + 14, nullptr, 10);
    }
  }
  result.code = pclose(pipe);
  return result;
}

struct OrientationStats {
  unsigned red = 0;
  unsigned blue = 0;
  unsigned decided = 0;  // battles with a winner (not the tick limit)
  long tick_sum = 0;
  unsigned n = 0;
};

void report(const char* name, const OrientationStats& stats) {
  const double red_share =
      stats.n > 0 ? 100.0 * static_cast<double>(stats.red) / stats.n : 0.0;
  const double pre_limit =
      stats.n > 0
          ? 100.0 * static_cast<double>(stats.decided) / stats.n
          : 0.0;
  const double mean_ticks =
      stats.n > 0 ? static_cast<double>(stats.tick_sum) / stats.n : 0.0;
  std::printf("  %-8s Red %u / Blue %u / none %u  (Red %.1f%%)\n", name,
              stats.red, stats.blue, stats.n - stats.red - stats.blue,
              red_share);
  std::printf("           ended before the tick limit: %.1f%%, mean end "
              "tick %.0f\n",
              pre_limit, mean_ticks);
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenario = "default";
  unsigned seeds = 40;
  std::string variance = "on";
  std::string mod;
  long max_ticks = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto value = [argc, argv, i]() -> const char* {
      return i + 1 < argc ? argv[i + 1] : nullptr;
    };
    if (flag == "--scenario" && value() != nullptr) {
      scenario = value();
      ++i;
    } else if (flag == "--seeds" && value() != nullptr) {
      seeds = static_cast<unsigned>(std::strtoul(value(), nullptr, 10));
      ++i;
    } else if (flag == "--variance" && value() != nullptr) {
      variance = value();
      ++i;
    } else if (flag == "--mod" && value() != nullptr) {
      mod = value();
      ++i;
    } else if (flag == "--max-ticks" && value() != nullptr) {
      max_ticks = std::strtol(value(), nullptr, 10);
      ++i;
    } else {
      std::fprintf(stderr,
                   "usage: balance --scenario NAME --seeds N "
                   "[--variance on|off] [--mod DIR] [--max-ticks N]\n");
      return 2;
    }
  }
  if (seeds == 0) {
    std::fprintf(stderr, "balance: --seeds must be >= 1\n");
    return 2;
  }

  OrientationStats normal;
  OrientationStats swapped;
  for (unsigned seed = 1; seed <= seeds; ++seed) {
    RunResult run = run_battle(scenario, seed, false, variance, mod,
                               max_ticks);
    if (run.code != 0) {
      std::fprintf(stderr, "balance: seed %u failed (exit %d)\n", seed,
                   run.code);
      return 2;
    }
    normal.n += 1;
    normal.tick_sum += run.ticks;
    if (run.winner == "Red") {
      normal.red += 1;
    } else if (run.winner == "Blue") {
      normal.blue += 1;
    }
    if (run.reason != "tick_limit") {
      normal.decided += 1;
    }
    run = run_battle(scenario, seed, true, variance, mod, max_ticks);
    if (run.code != 0) {
      std::fprintf(stderr, "balance: seed %u (swapped) failed (exit %d)\n",
                   seed, run.code);
      return 2;
    }
    swapped.n += 1;
    swapped.tick_sum += run.ticks;
    if (run.winner == "Red") {
      swapped.red += 1;
    } else if (run.winner == "Blue") {
      swapped.blue += 1;
    }
    if (run.reason != "tick_limit") {
      swapped.decided += 1;
    }
  }

  std::printf("scenario '%s', %u seeds per orientation, variance %s\n",
              scenario.c_str(), seeds, variance.c_str());
  report("normal", normal);
  report("swapped", swapped);

  // Side-averaged win rates: how often a COLOR wins averaging away the
  // deployment band. Orientation effect: how much the band alone swings
  // a color's fortunes (the difference between its rates).
  const double normal_red =
      100.0 * static_cast<double>(normal.red) / normal.n;
  const double swapped_red =
      100.0 * static_cast<double>(swapped.red) / swapped.n;
  const double side_averaged_red = (normal_red + swapped_red) / 2.0;
  const double orientation_effect = std::fabs(normal_red - swapped_red);
  std::printf("  side-averaged Red win rate: %.1f%% (Blue %.1f%%)\n",
              side_averaged_red, 100.0 - side_averaged_red);
  std::printf("  orientation effect: %.1f%% (Red normal %.1f%% vs swapped "
              "%.1f%%)\n",
              orientation_effect, normal_red, swapped_red);
  return 0;
}
