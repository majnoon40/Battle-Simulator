// Unit type table loading tests: valid tables, every validation failure from
// the M2 spec (malformed JSON, missing fields, zero/negative values, wrong
// types, duplicate ids, unknown fields), and the committed data/units.json.
#include "sim/unit_types.hpp"

#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

// parse_unit_types must reject `text` with a std::invalid_argument whose
// message mentions `fragment`; anything else fails the test.
void expect_reject(const std::string& text, const std::string& fragment) {
  try {
    static_cast<void>(sim::parse_unit_types(text));
    FAIL() << "expected rejection, input was: " << text;
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find(fragment), std::string::npos)
        << "error was: " << error.what();
  }
}

constexpr const char* kValidTable = R"json([
  {"id": "infantry", "health": 100.0, "damage": 12.0, "speed": 2.0, "range": 2.0, "sight_radius": 40.0, "cooldown": 5},
  {"id": "archer", "health": 70.0, "damage": 9.0, "speed": 1.6, "range": 8.0, "sight_radius": 60.0, "cooldown": 8},
  {"id": "cavalry", "health": 160.0, "damage": 20.0, "speed": 4.5, "range": 2.0, "sight_radius": 50.0, "cooldown": 7}
])json";

// A complete, valid entry; tests replace one member to build invalid ones.
constexpr const char* kFullEntry =
    "{\"id\": \"a\", \"health\": 10.0, \"damage\": 2.0, \"speed\": 1.0, "
    "\"range\": 3.0, \"cooldown\": 4, \"sight_radius\": 40.0}";

TEST(UnitTypesTest, ParsesValidTable) {
  const sim::UnitTypeTable table = sim::parse_unit_types(kValidTable);
  ASSERT_EQ(table.size(), 3u);
  EXPECT_EQ(table.types[0].id, "infantry");
  EXPECT_DOUBLE_EQ(table.types[0].stats.max_health, 100.0);
  EXPECT_DOUBLE_EQ(table.types[0].stats.damage, 12.0);
  EXPECT_DOUBLE_EQ(table.types[0].stats.speed, 2.0);
  EXPECT_DOUBLE_EQ(table.types[0].stats.attack_range, 2.0);
  EXPECT_EQ(table.types[0].stats.attack_cooldown_ticks, 5u);
  EXPECT_DOUBLE_EQ(table.types[0].stats.sight_radius, 40.0);

  EXPECT_EQ(table.types[1].id, "archer");
  EXPECT_DOUBLE_EQ(table.types[1].stats.max_health, 70.0);
  EXPECT_DOUBLE_EQ(table.types[1].stats.damage, 9.0);
  EXPECT_DOUBLE_EQ(table.types[1].stats.speed, 1.6);
  EXPECT_DOUBLE_EQ(table.types[1].stats.attack_range, 8.0);
  EXPECT_EQ(table.types[1].stats.attack_cooldown_ticks, 8u);
  EXPECT_DOUBLE_EQ(table.types[1].stats.sight_radius, 60.0);

  EXPECT_EQ(table.types[2].id, "cavalry");
  EXPECT_DOUBLE_EQ(table.types[2].stats.max_health, 160.0);
  EXPECT_EQ(table.types[2].stats.attack_cooldown_ticks, 7u);
  EXPECT_DOUBLE_EQ(table.types[2].stats.sight_radius, 50.0);
}

TEST(UnitTypesTest, StreamOverloadParsesTheSameTable) {
  std::istringstream stream(kValidTable);
  const sim::UnitTypeTable table = sim::parse_unit_types(stream);
  ASSERT_EQ(table.size(), 3u);
  EXPECT_EQ(table.types[1].id, "archer");
}

TEST(UnitTypesTest, SightRadiusIsRequired) {
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4}])",
                "missing required field 'sight_radius'");
}

TEST(UnitTypesTest, SightRadiusMustExceedAttackRange) {
  // Below the range: rejected (a unit must see at least as far as it shoots).
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 3.0}])",
                "sight_radius must strictly exceed");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 1.0}])",
                "sight_radius must strictly exceed");
  // Equal to the range is also rejected; strictly greater is required.
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 8.0, "cooldown": 4, "sight_radius": 8.0}])",
                "sight_radius must strictly exceed");
  // Zero and negative values are rejected like any other non-positive stat.
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 0.0}])",
                "'sight_radius' must be > 0");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": -5.0}])",
                "'sight_radius' must be > 0");
}

TEST(UnitTypesTest, IndexOfResolvesIds) {
  const sim::UnitTypeTable table = sim::parse_unit_types(kValidTable);
  EXPECT_EQ(table.index_of("infantry"), 0);
  EXPECT_EQ(table.index_of("archer"), 1);
  EXPECT_EQ(table.index_of("cavalry"), 2);
  EXPECT_EQ(table.index_of("catapult"), -1);
  EXPECT_EQ(table.index_of(""), -1);
}

TEST(UnitTypesTest, MissingFieldsAreRejected) {
  expect_reject(R"([{"id": "a", "damage": 1.0, "speed": 1.0, "range": 1.0, "cooldown": 1, "sight_radius": 40.0}])",
                "missing required field 'health'");
  expect_reject(R"([{"id": "a", "health": 1.0, "speed": 1.0, "range": 1.0, "cooldown": 1, "sight_radius": 40.0}])",
                "missing required field 'damage'");
  expect_reject(R"([{"id": "a", "health": 1.0, "damage": 1.0, "range": 1.0, "cooldown": 1, "sight_radius": 40.0}])",
                "missing required field 'speed'");
  expect_reject(R"([{"id": "a", "health": 1.0, "damage": 1.0, "speed": 1.0, "cooldown": 1, "sight_radius": 40.0}])",
                "missing required field 'range'");
  expect_reject(R"([{"id": "a", "health": 1.0, "damage": 1.0, "speed": 1.0, "range": 1.0}])",
                "missing required field 'cooldown'");
  expect_reject(R"([{"health": 1.0, "damage": 1.0, "speed": 1.0, "range": 1.0, "cooldown": 1, "sight_radius": 40.0}])",
                "missing required field 'id'");
}

TEST(UnitTypesTest, ZeroOrNegativeValuesAreRejected) {
  expect_reject(R"([{"id": "a", "health": 0.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}])",
                "'health' must be > 0");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": -1.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}])",
                "'damage' must be > 0");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 0.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}])",
                "'speed' must be > 0");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 0.0, "cooldown": 4, "sight_radius": 40.0}])",
                "'range' must be > 0");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 0, "sight_radius": 40.0}])",
                "'cooldown' must be >= 1");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": -2}])",
                "'cooldown' must be >= 1");
}

TEST(UnitTypesTest, WrongValueTypesAreRejected) {
  expect_reject(R"([{"id": "a", "health": "100", "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}])",
                "'health' must be a number");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": true, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}])",
                "'damage' must be a number");
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 2.5}])",
                "'cooldown' must be a whole number");
  expect_reject(R"([{"id": 7, "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}])",
                "'id' must be a string");
  expect_reject(R"([{"id": "", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "sight_radius": 40.0}])",
                "id must not be empty");
}

TEST(UnitTypesTest, DuplicateIdsAreRejected) {
  expect_reject(std::string("[") + kFullEntry + ", " + kFullEntry + "]",
                "duplicate unit type id 'a'");
}

TEST(UnitTypesTest, UnknownFieldsAreRejected) {
  expect_reject(R"([{"id": "a", "health": 10.0, "damage": 2.0, "speed": 1.0, "range": 3.0, "cooldown": 4, "raneg": 1.0}])",
                "unknown field 'raneg'");
}

TEST(UnitTypesTest, EmptyTableIsRejected) {
  expect_reject("[]", "at least one type");
}

TEST(UnitTypesTest, NonArrayRootIsRejected) {
  expect_reject(kFullEntry, "must be a JSON array");
  expect_reject("42", "must be a JSON array");
}

TEST(UnitTypesTest, MalformedJsonIsRejected) {
  expect_reject("{", "not valid JSON");
  expect_reject("[{\"id\": }]", "not valid JSON");
}

TEST(UnitTypesTest, CommittedDataFileIsValid) {
  std::ifstream file(TEST_DATA_DIR "/units.json");
  ASSERT_TRUE(file.good()) << "cannot open " TEST_DATA_DIR "/units.json";
  const sim::UnitTypeTable table = sim::parse_unit_types(file);

  const int infantry = table.index_of("infantry");
  const int archer = table.index_of("archer");
  const int cavalry = table.index_of("cavalry");
  ASSERT_GE(infantry, 0);
  ASSERT_GE(archer, 0);
  ASSERT_GE(cavalry, 0);

  // Infantry must equal the M1 defaults (regression requirement).
  const sim::UnitStats& m1 = table.types[infantry].stats;
  EXPECT_DOUBLE_EQ(m1.max_health, 100.0);
  EXPECT_DOUBLE_EQ(m1.damage, 12.0);
  EXPECT_DOUBLE_EQ(m1.speed, 2.0);
  EXPECT_DOUBLE_EQ(m1.attack_range, 2.0);
  EXPECT_EQ(m1.attack_cooldown_ticks, 5u);

  // M2 spec: archer range > infantry range; cavalry speed > infantry speed.
  EXPECT_GT(table.types[archer].stats.attack_range, m1.attack_range);
  EXPECT_GT(table.types[cavalry].stats.speed, m1.speed);
}

}  // namespace
