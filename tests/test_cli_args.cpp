// CLI argument parsing tests, including invalid configuration rejection.
#include "cli/cli_args.hpp"

#include <initializer_list>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

// Runs parse_args over an argv-style array.
cli::ParseOutcome parse(std::initializer_list<const char*> args) {
  int argc = static_cast<int>(args.size());
  char** argv = new char*[argc == 0 ? 1 : argc];
  int i = 0;
  for (const char* arg : args) {
    argv[i++] = const_cast<char*>(arg);
  }
  const cli::ParseOutcome outcome = cli::parse_args(argc, argv);
  delete[] argv;
  return outcome;
}

TEST(CliArgsTest, DefaultsAreSensible) {
  const cli::ParseOutcome outcome = parse({"battle_cli"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.args.help);
  EXPECT_EQ(outcome.args.seed, 42u);
  EXPECT_EQ(outcome.args.units, 500u);
  EXPECT_FALSE(outcome.args.units_explicit);
  EXPECT_EQ(outcome.args.ticks, 3000u);
  EXPECT_TRUE(outcome.args.config_path.empty());   // -> data/units.json
  EXPECT_TRUE(outcome.args.scenario_path.empty());  // -> data/scenarios/default.json
  EXPECT_TRUE(outcome.args.grid_mode);              // M3 default: grid
  EXPECT_FALSE(outcome.args.neighbor_mode_explicit);
  EXPECT_DOUBLE_EQ(outcome.args.grid_cell_size, 16.0);
  EXPECT_FALSE(outcome.args.grid_cell_explicit);
  EXPECT_TRUE(outcome.args.utility_ai);             // M4 default: utility
  EXPECT_FALSE(outcome.args.ai_mode_explicit);
  EXPECT_TRUE(outcome.args.personalities_path.empty());  // -> data/personalities.json
  EXPECT_EQ(outcome.args.ai_debug_unit, 0u);
  EXPECT_EQ(outcome.args.ai_debug_tick, 0u);
  EXPECT_FALSE(outcome.args.ai_debug_tick_explicit);
  EXPECT_FALSE(outcome.args.export_json);
}

TEST(CliArgsTest, ParsesAllOptions) {
  const cli::ParseOutcome outcome = parse(
      {"battle_cli", "--seed", "123", "--ticks", "500",
       "--config", "custom/units.json", "--scenario", "custom/battle.json",
       "--export-json", "output/final.json"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.seed, 123u);
  EXPECT_EQ(outcome.args.ticks, 500u);
  EXPECT_EQ(outcome.args.config_path, "custom/units.json");
  EXPECT_EQ(outcome.args.scenario_path, "custom/battle.json");
  EXPECT_FALSE(outcome.args.units_explicit);
  EXPECT_TRUE(outcome.args.export_json);
  EXPECT_EQ(outcome.args.export_path, "output/final.json");
}

TEST(CliArgsTest, UnitsFlagMarksExplicitMode) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--units", "400"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.units, 400u);
  EXPECT_TRUE(outcome.args.units_explicit);
}

TEST(CliArgsTest, AiModeFlagParses) {
  cli::ParseOutcome outcome = parse({"battle_cli", "--ai-mode", "legacy"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.args.utility_ai);
  EXPECT_TRUE(outcome.args.ai_mode_explicit);

  outcome = parse({"battle_cli", "--ai-mode", "utility"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.utility_ai);
  EXPECT_TRUE(outcome.args.ai_mode_explicit);
}

TEST(CliArgsTest, InvalidAiModeIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--ai-mode", "smart"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("'legacy' or 'utility'"), std::string::npos)
      << "error was: " << outcome.error;
}

TEST(CliArgsTest, PersonalitiesPathParses) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--personalities", "custom/personalities.json"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.personalities_path, "custom/personalities.json");
}

TEST(CliArgsTest, EmptyPersonalitiesPathIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--personalities", ""});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--personalities"), std::string::npos);
}

TEST(CliArgsTest, AiDebugFlagsParse) {
  cli::ParseOutcome outcome =
      parse({"battle_cli", "--ai-debug", "17"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.ai_debug_unit, 17u);
  EXPECT_EQ(outcome.args.ai_debug_tick, 0u);
  EXPECT_FALSE(outcome.args.ai_debug_tick_explicit);

  outcome = parse({"battle_cli", "--ai-debug", "17", "--ai-debug-tick", "5"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.ai_debug_unit, 17u);
  EXPECT_EQ(outcome.args.ai_debug_tick, 5u);
  EXPECT_TRUE(outcome.args.ai_debug_tick_explicit);
}

TEST(CliArgsTest, InvalidAiDebugValuesAreRejected) {
  EXPECT_FALSE(parse({"battle_cli", "--ai-debug", "0"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--ai-debug", "abc"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--ai-debug-tick", "3"}).ok);  // needs --ai-debug
  EXPECT_FALSE(parse({"battle_cli", "--ai-debug", "3", "--ai-debug-tick", "0"}).ok);
}

TEST(CliArgsTest, AiDebugRequiresUtilityMode) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--ai-mode", "legacy", "--ai-debug", "3"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--ai-debug requires --ai-mode utility"),
            std::string::npos);
}

TEST(CliArgsTest, MoraleFlagParses) {
  cli::ParseOutcome outcome = parse({"battle_cli", "--morale", "off"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.args.morale_on);
  EXPECT_TRUE(outcome.args.morale_explicit);

  outcome = parse({"battle_cli", "--morale", "on"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.morale_on);
  EXPECT_TRUE(outcome.args.morale_explicit);
}

TEST(CliArgsTest, MoraleDefaultsToOn) {
  const cli::ParseOutcome outcome = parse({"battle_cli"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.morale_on);  // M5 default
  EXPECT_FALSE(outcome.args.morale_explicit);
  EXPECT_TRUE(outcome.args.morale_config_path.empty());  // -> data/morale.json
}

TEST(CliArgsTest, InvalidMoraleValueIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--morale", "maybe"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("'on' or 'off'"), std::string::npos)
      << "error was: " << outcome.error;
}

TEST(CliArgsTest, MoraleConfigPathParses) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--morale-config", "custom/morale.json"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.morale_config_path, "custom/morale.json");
}

TEST(CliArgsTest, EmptyMoraleConfigPathIsRejected) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--morale-config", ""});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--morale-config"), std::string::npos);
}

TEST(CliArgsTest, ExplicitMoraleWithLegacyIsRejected) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--ai-mode", "legacy", "--morale", "on"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--morale cannot be combined"),
            std::string::npos);
}

TEST(CliArgsTest, SquadsFlagParses) {
  cli::ParseOutcome outcome = parse({"battle_cli", "--squads", "off"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.args.squads_on);
  EXPECT_TRUE(outcome.args.squads_explicit);

  outcome = parse({"battle_cli", "--squads", "on"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.squads_on);
  EXPECT_TRUE(outcome.args.squads_explicit);
}

TEST(CliArgsTest, SquadsDefaultToOn) {
  const cli::ParseOutcome outcome = parse({"battle_cli"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.squads_on);  // M6 default
  EXPECT_FALSE(outcome.args.squads_explicit);
  EXPECT_TRUE(outcome.args.formations_path.empty());  // -> data/formations.json
  EXPECT_TRUE(outcome.args.commanders_path.empty());  // -> data/commanders.json
}

TEST(CliArgsTest, InvalidSquadsValueIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--squads", "maybe"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("'on' or 'off'"), std::string::npos)
      << "error was: " << outcome.error;
}

TEST(CliArgsTest, SquadConfigPathsParse) {
  cli::ParseOutcome outcome =
      parse({"battle_cli", "--formations", "custom/formations.json",
             "--commanders", "custom/commanders.json"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.formations_path, "custom/formations.json");
  EXPECT_EQ(outcome.args.commanders_path, "custom/commanders.json");
}

TEST(CliArgsTest, EmptySquadConfigPathsAreRejected) {
  cli::ParseOutcome outcome = parse({"battle_cli", "--formations", ""});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--formations"), std::string::npos);

  outcome = parse({"battle_cli", "--commanders", ""});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--commanders"), std::string::npos);
}

TEST(CliArgsTest, SquadDebugIdParses) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--squad-debug", "3"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.squad_debug_id, 3);
}

TEST(CliArgsTest, InvalidSquadDebugValuesAreRejected) {
  cli::ParseOutcome outcome = parse({"battle_cli", "--squad-debug", "-1"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--squad-debug"), std::string::npos);

  outcome = parse({"battle_cli", "--squad-debug", "abc"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--squad-debug"), std::string::npos);
}

TEST(CliArgsTest, ExplicitSquadsWithLegacyIsRejected) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--ai-mode", "legacy", "--squads", "on"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--squads cannot be combined"),
            std::string::npos);
}

TEST(CliArgsTest, SquadDebugWithLegacyIsRejected) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--ai-mode", "legacy", "--squad-debug", "0"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--squad-debug requires --ai-mode utility"),
            std::string::npos);
}

TEST(CliArgsTest, NeighborModeFlagParses) {
  cli::ParseOutcome outcome =
      parse({"battle_cli", "--neighbor-mode", "brute"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.args.grid_mode);
  EXPECT_TRUE(outcome.args.neighbor_mode_explicit);

  outcome = parse({"battle_cli", "--neighbor-mode", "grid"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.grid_mode);
  EXPECT_TRUE(outcome.args.neighbor_mode_explicit);
}

TEST(CliArgsTest, InvalidNeighborModeIsRejected) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--neighbor-mode", "fast"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("'brute' or 'grid'"), std::string::npos)
      << "error was: " << outcome.error;
}

TEST(CliArgsTest, GridCellFlagParses) {
  cli::ParseOutcome outcome = parse({"battle_cli", "--grid-cell", "64"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_DOUBLE_EQ(outcome.args.grid_cell_size, 64.0);
  EXPECT_TRUE(outcome.args.grid_cell_explicit);

  outcome = parse({"battle_cli", "--grid-cell", "12.5"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_DOUBLE_EQ(outcome.args.grid_cell_size, 12.5);
}

TEST(CliArgsTest, InvalidGridCellValuesAreRejected) {
  EXPECT_FALSE(parse({"battle_cli", "--grid-cell", "0"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--grid-cell", "-5"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--grid-cell", "abc"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--grid-cell", "1e3"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--grid-cell", "8."}).ok);
}

TEST(CliArgsTest, GridCellRequiresGridMode) {
  const cli::ParseOutcome outcome = parse(
      {"battle_cli", "--neighbor-mode", "brute", "--grid-cell", "64"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--grid-cell"), std::string::npos);
}

TEST(CliArgsTest, UnitsAndScenarioAreMutuallyExclusive) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--units", "100", "--scenario", "battle.json"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("cannot be combined"), std::string::npos)
      << "error was: " << outcome.error;
}

TEST(CliArgsTest, EmptyConfigPathIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--config", ""});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--config"), std::string::npos);
}

TEST(CliArgsTest, EmptyScenarioPathIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--scenario", ""});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--scenario"), std::string::npos);
}

TEST(CliArgsTest, HelpShortCircuits) {
  cli::ParseOutcome outcome = parse({"battle_cli", "--help"});
  EXPECT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.help);

  outcome = parse({"battle_cli", "-h", "--units", "10"});
  EXPECT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.help);
}

TEST(CliArgsTest, SeedZeroIsValid) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--seed", "0"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.seed, 0u);
}

TEST(CliArgsTest, LargeSeedValuesAreAccepted) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--seed", "18446744073709551615"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.seed, 18446744073709551615ULL);
}

TEST(CliArgsTest, InvalidUnitsValuesAreRejected) {
  EXPECT_FALSE(parse({"battle_cli", "--units", "0"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--units", "1"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--units", "-5"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--units", "abc"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--units", "4.5"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--units", "99999999999"}).ok);  // > uint32
}

TEST(CliArgsTest, InvalidTicksValuesAreRejected) {
  EXPECT_FALSE(parse({"battle_cli", "--ticks", "0"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--ticks", "-1"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--ticks", "abc"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--ticks", "1.5"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--ticks", "99999999999999999999"}).ok);
}

TEST(CliArgsTest, InvalidSeedValuesAreRejected) {
  EXPECT_FALSE(parse({"battle_cli", "--seed", "-1"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--seed", "abc"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--seed", "42x"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--seed", "1e5"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--seed", "99999999999999999999"}).ok);
}

TEST(CliArgsTest, NegativeNumbersProduceClearErrors) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--units", "-5"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("non-negative"), std::string::npos)
      << "error was: " << outcome.error;
}

TEST(CliArgsTest, UnknownArgumentsAreRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--foo"});
  ASSERT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("unknown argument"), std::string::npos)
      << "error was: " << outcome.error;

  EXPECT_FALSE(parse({"battle_cli", "positional"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--seed", "42", "--verbose"}).ok);
}

TEST(CliArgsTest, MissingValuesAreRejected) {
  EXPECT_FALSE(parse({"battle_cli", "--seed"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--units"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--ticks"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--config"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--scenario"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--neighbor-mode"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--grid-cell"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--export-json"}).ok);
}

TEST(CliArgsTest, EmptyExportPathIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--export-json", ""});
  EXPECT_FALSE(outcome.ok);
}

TEST(CliArgsTest, LastValueWinsForRepeatedFlags) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--seed", "1", "--seed", "7"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_EQ(outcome.args.seed, 7u);
}

TEST(CliArgsTest, UsageTextDocumentsAllOptions) {
  std::ostringstream out;
  cli::print_usage(out);
  const std::string text = out.str();
  EXPECT_NE(text.find("--seed"), std::string::npos);
  EXPECT_NE(text.find("--units"), std::string::npos);
  EXPECT_NE(text.find("--ticks"), std::string::npos);
  EXPECT_NE(text.find("--config"), std::string::npos);
  EXPECT_NE(text.find("--scenario"), std::string::npos);
  EXPECT_NE(text.find("--neighbor-mode"), std::string::npos);
  EXPECT_NE(text.find("--grid-cell"), std::string::npos);
  EXPECT_NE(text.find("--squads"), std::string::npos);
  EXPECT_NE(text.find("--squad-debug"), std::string::npos);
  EXPECT_NE(text.find("--formations"), std::string::npos);
  EXPECT_NE(text.find("--commanders"), std::string::npos);
  EXPECT_NE(text.find("--export-json"), std::string::npos);
  EXPECT_NE(text.find("--nav"), std::string::npos);
  EXPECT_NE(text.find("--trace"), std::string::npos);
  EXPECT_NE(text.find("--help"), std::string::npos);
}

// ---------------------------------------------------------------------------
// m11a: --nav flag and the --swap-sides setup mirror.
// ---------------------------------------------------------------------------

TEST(CliArgsTest, NavDefaultsToV2) {
  const cli::ParseOutcome outcome = parse({"battle_cli"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.nav_v2);
}

TEST(CliArgsTest, NavFlagParses) {
  EXPECT_TRUE(parse({"battle_cli", "--nav", "v2"}).args.nav_v2);
  EXPECT_TRUE(parse({"battle_cli", "--nav", "on"}).args.nav_v2);
  EXPECT_FALSE(parse({"battle_cli", "--nav", "v1"}).args.nav_v2);
  EXPECT_FALSE(parse({"battle_cli", "--nav", "off"}).args.nav_v2);
}

TEST(CliArgsTest, InvalidNavValueIsRejected) {
  const cli::ParseOutcome outcome = parse({"battle_cli", "--nav", "v3"});
  EXPECT_FALSE(outcome.ok);
  EXPECT_NE(outcome.error.find("--nav"), std::string::npos);
}

TEST(CliArgsTest, TraceFlagsParse) {
  const cli::ParseOutcome outcome =
      parse({"battle_cli", "--trace", "out.jsonl", "--trace-interval", "25"});
  ASSERT_TRUE(outcome.ok);
  EXPECT_TRUE(outcome.args.trace_on);
  EXPECT_EQ(outcome.args.trace_path, "out.jsonl");
  EXPECT_EQ(outcome.args.trace_interval, 25u);
}

TEST(CliArgsTest, TraceIntervalBoundsAreEnforced) {
  EXPECT_FALSE(parse({"battle_cli", "--trace-interval", "0"}).ok);
  EXPECT_FALSE(parse({"battle_cli", "--trace-interval", "100001"}).ok);
}

// m13 audit finding 1: every numeric option must parse strictly —
// trailing characters, signs, empty strings and overflow are rejected,
// and the error names the offending argument.
TEST(CliArgsTest, TraceIntervalRejectsMalformedValues) {
  for (const char* bad :
       {"25junk", "10abc", "150garbage", "", "-1", "-5", "1e3", " 25",
        "25 ", "0x10", "+25", "1.5"}) {
    const cli::ParseOutcome outcome = parse(
        {"battle_cli", "--trace", "t.jsonl", "--trace-interval", bad});
    EXPECT_FALSE(outcome.ok) << "value: '" << bad << "'";
  }
  // The error identifies the offending argument and echoes the value.
  const cli::ParseOutcome named = parse(
      {"battle_cli", "--trace", "t.jsonl", "--trace-interval", "25junk"});
  ASSERT_FALSE(named.ok);
  EXPECT_NE(named.error.find("--trace-interval"), std::string::npos)
      << "error was: " << named.error;
  EXPECT_NE(named.error.find("25junk"), std::string::npos)
      << "error was: " << named.error;
  // Boundary values: 1 and 100000 are the valid ends.
  EXPECT_TRUE(parse(
      {"battle_cli", "--trace", "t.jsonl", "--trace-interval", "1"}).ok);
  EXPECT_TRUE(parse(
      {"battle_cli", "--trace", "t.jsonl", "--trace-interval", "100000"}).ok);
}

TEST(CliArgsTest, CsvIntervalRejectsMalformedValues) {
  for (const char* bad :
       {"25junk", "10abc", "", "-1", "1e3", " 25", "+25", "1.5"}) {
    const cli::ParseOutcome outcome =
        parse({"battle_cli", "--export-csv", "c.csv", "--csv-interval", bad});
    EXPECT_FALSE(outcome.ok) << "value: '" << bad << "'";
  }
  const cli::ParseOutcome named =
      parse({"battle_cli", "--export-csv", "c.csv", "--csv-interval", "10abc"});
  ASSERT_FALSE(named.ok);
  EXPECT_NE(named.error.find("--csv-interval"), std::string::npos)
      << "error was: " << named.error;
  EXPECT_TRUE(parse(
      {"battle_cli", "--export-csv", "c.csv", "--csv-interval", "1"}).ok);
  EXPECT_TRUE(parse(
      {"battle_cli", "--export-csv", "c.csv", "--csv-interval", "100000"}).ok);
}

TEST(CliArgsTest, SeekRejectsMalformedAndNegativeValues) {
  // "-1" used to wrap through strtoull into a huge seek tick.
  for (const char* bad :
       {"-1", "0", "25junk", "10abc", "", "1e3", " 5", "+5", "1.5",
        "18446744073709551616",  // 2^64: one past uint64 max
        "999999999999999999999999999999999"}) {
    const cli::ParseOutcome outcome =
        parse({"battle_cli", "--replay", "r.replay", "--seek", bad});
    EXPECT_FALSE(outcome.ok) << "value: '" << bad << "'";
  }
  const cli::ParseOutcome named =
      parse({"battle_cli", "--replay", "r.replay", "--seek", "25junk"});
  ASSERT_FALSE(named.ok);
  EXPECT_NE(named.error.find("--seek"), std::string::npos)
      << "error was: " << named.error;
  // The full uint64 range is accepted (minus zero).
  const cli::ParseOutcome max = parse(
      {"battle_cli", "--replay", "r.replay", "--seek",
       "18446744073709551615"});
  ASSERT_TRUE(max.ok);
  EXPECT_EQ(max.args.replay_seek, 18446744073709551615ULL);
}

TEST(CliArgsTest, InterveneRejectsMalformedValues) {
  for (const char* bad :
       {"kill:25junk:10", "kill:10abc:5", "kill:", "kill:0:10", "kill:5:0",
        "kill:-1:10", "kill:5:-1", "kill:5:10junk", "kill:1e3:5",
        "kill:4294967296:10",  // unit id above uint32 max
        "kill:5:18446744073709551616",  // tick above uint64 max
        "kill: 5:10", "kill:5: 10", "kill:+5:10"}) {
    const cli::ParseOutcome outcome =
        parse({"battle_cli", "--record", "r.replay", "--intervene", bad});
    EXPECT_FALSE(outcome.ok) << "value: '" << bad << "'";
  }
  // The error identifies the offending part.
  const cli::ParseOutcome named =
      parse({"battle_cli", "--record", "r.replay", "--intervene",
             "kill:25junk:10"});
  ASSERT_FALSE(named.ok);
  EXPECT_NE(named.error.find("--intervene"), std::string::npos)
      << "error was: " << named.error;
  // Boundaries: uint32-max unit id and uint64-max tick are valid.
  const cli::ParseOutcome edge = parse(
      {"battle_cli", "--record", "r.replay", "--intervene",
       "kill:4294967295:18446744073709551615"});
  ASSERT_TRUE(edge.ok);
  ASSERT_EQ(edge.args.interventions.size(), 1u);
  EXPECT_EQ(edge.args.interventions[0],
            "kill:4294967295:18446744073709551615");
}

// m13 audit finding 8: the duplicated --combat branch (unreachable after
// the combined --ai-avail/--end/--combat handler) was removed; the flag
// itself must parse exactly as before.
TEST(CliArgsTest, CombatFlagParsesThroughCombinedBranch) {
  EXPECT_TRUE(parse({"battle_cli", "--combat", "v2"}).args.combat_v2);
  EXPECT_TRUE(parse({"battle_cli", "--combat", "on"}).args.combat_v2);
  EXPECT_FALSE(parse({"battle_cli", "--combat", "v1"}).args.combat_v2);
  EXPECT_FALSE(parse({"battle_cli", "--combat", "off"}).args.combat_v2);
  EXPECT_TRUE(parse({"battle_cli"}).args.combat_v2);  // default v2
  const cli::ParseOutcome bad = parse({"battle_cli", "--combat", "v3"});
  EXPECT_FALSE(bad.ok);
  EXPECT_NE(bad.error.find("--combat"), std::string::npos)
      << "error was: " << bad.error;
  // The usage text still documents it.
  std::ostringstream out;
  cli::print_usage(out);
  EXPECT_NE(out.str().find("--combat"), std::string::npos);
}

TEST(SideSwapTest, SwapsExplicitObjectivesExactlyOnce) {
  // The M10 bug: the swap loop ran per faction, so the Red iteration
  // swapped the objectives and the Blue iteration swapped them right
  // back — a swapped battle parked each army on the objective inside
  // its own spawn band.
  sim::SimulationConfig config;
  const std::size_t red = static_cast<std::size_t>(sim::Faction::Red);
  const std::size_t blue = static_cast<std::size_t>(sim::Faction::Blue);
  config.objectives[red] = sim::ObjectivePoint{100.0, 200.0, true};
  config.objectives[blue] = sim::ObjectivePoint{300.0, 400.0, true};
  cli::apply_side_swap(config);
  EXPECT_TRUE(config.objectives[red].explicit_set);
  EXPECT_DOUBLE_EQ(config.objectives[red].x, 300.0);
  EXPECT_DOUBLE_EQ(config.objectives[red].y, 400.0);
  EXPECT_DOUBLE_EQ(config.objectives[blue].x, 100.0);
  EXPECT_DOUBLE_EQ(config.objectives[blue].y, 200.0);
}

TEST(SideSwapTest, SwapsCompositionsPlansAndPatterns) {
  sim::SimulationConfig config;
  const std::size_t red = static_cast<std::size_t>(sim::Faction::Red);
  const std::size_t blue = static_cast<std::size_t>(sim::Faction::Blue);
  config.composition[red] = {sim::TypeCount{0, 10}};
  config.composition[blue] = {sim::TypeCount{1, 20}};
  config.squad_plans[red] = sim::SquadPlan{2, 5};
  config.squad_plans[blue] = sim::SquadPlan{4, 3};
  config.spawn_patterns[red] = sim::SpawnPattern::Cluster;
  config.spawn_patterns[blue] = sim::SpawnPattern::Echelon;
  cli::apply_side_swap(config);
  EXPECT_EQ(config.composition[red][0].type_index, 1u);
  EXPECT_EQ(config.composition[red][0].count, 20u);
  EXPECT_EQ(config.composition[blue][0].type_index, 0u);
  EXPECT_EQ(config.composition[blue][0].count, 10u);
  EXPECT_EQ(config.squad_plans[red]->count, 4);
  EXPECT_EQ(config.squad_plans[blue]->count, 2);
  EXPECT_EQ(config.spawn_patterns[red], sim::SpawnPattern::Echelon);
  EXPECT_EQ(config.spawn_patterns[blue], sim::SpawnPattern::Cluster);
}

TEST(SideSwapTest, DefaultObjectivesAreNotTouched) {
  // Default objectives (explicit_set false) are computed inside the
  // simulation from the bands; the swap must leave them alone.
  sim::SimulationConfig config;
  config.objectives[static_cast<std::size_t>(sim::Faction::Red)] =
      sim::ObjectivePoint{0.0, 0.0, false};
  config.objectives[static_cast<std::size_t>(sim::Faction::Blue)] =
      sim::ObjectivePoint{0.0, 0.0, false};
  cli::apply_side_swap(config);
  EXPECT_FALSE(config.objectives[static_cast<std::size_t>(
                   sim::Faction::Red)].explicit_set);
  EXPECT_FALSE(config.objectives[static_cast<std::size_t>(
                   sim::Faction::Blue)].explicit_set);
}

}  // namespace

// m13 item 2: --ai-avail / --end rule-version flags.
TEST(CliArgsTest, AvailabilityAndEndFlagsParse) {
  EXPECT_TRUE(parse({"battle_cli", "--ai-avail", "v2"}).args.ai_avail_v2);
  EXPECT_TRUE(parse({"battle_cli", "--ai-avail", "on"}).args.ai_avail_v2);
  EXPECT_FALSE(parse({"battle_cli", "--ai-avail", "v1"}).args.ai_avail_v2);
  EXPECT_FALSE(parse({"battle_cli", "--ai-avail", "off"}).args.ai_avail_v2);
  EXPECT_TRUE(parse({"battle_cli", "--end", "v2"}).args.end_v2);
  EXPECT_TRUE(parse({"battle_cli", "--end", "on"}).args.end_v2);
  EXPECT_FALSE(parse({"battle_cli", "--end", "v1"}).args.end_v2);
  EXPECT_FALSE(parse({"battle_cli", "--end", "off"}).args.end_v2);
  // Defaults are v2 for both.
  EXPECT_TRUE(parse({"battle_cli", "--seed", "42"}).args.ai_avail_v2);
  EXPECT_TRUE(parse({"battle_cli", "--seed", "42"}).args.end_v2);
  // Bad values are rejected with an error naming the flag.
  const cli::ParseOutcome bad_avail =
      parse({"battle_cli", "--ai-avail", "v3"});
  EXPECT_FALSE(bad_avail.ok);
  EXPECT_NE(bad_avail.error.find("--ai-avail"), std::string::npos);
  const cli::ParseOutcome bad_end = parse({"battle_cli", "--end", "soon"});
  EXPECT_FALSE(bad_end.ok);
  EXPECT_NE(bad_end.error.find("--end"), std::string::npos);
  // The usage text documents them.
  std::ostringstream out;
  cli::print_usage(out);
  const std::string usage = out.str();
  EXPECT_NE(usage.find("--ai-avail"), std::string::npos);
  EXPECT_NE(usage.find("--end"), std::string::npos);
}
