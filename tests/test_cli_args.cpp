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
  EXPECT_NE(text.find("--help"), std::string::npos);
}

}  // namespace
