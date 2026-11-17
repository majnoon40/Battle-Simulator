add_library(sim STATIC
  src/factions.cpp
  src/rng.cpp
  src/config.cpp
  src/unit_types.cpp
  src/personalities.cpp
  src/combat.cpp
  src/morale.cpp
  src/squads.cpp
  src/events.cpp
  src/state_hash.cpp
  src/tactics.cpp
  src/terrain.cpp
  src/flow_field.cpp
  src/scenario.cpp
  src/world.cpp
  src/results.cpp
  src/neighbors.cpp
  src/spatial_hash.cpp
  src/ai.cpp
  src/simulation.cpp
  src/json_export.cpp)
add_library(sim::sim ALIAS sim)
target_include_directories(sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(sim PUBLIC cxx_std_20)
target_link_libraries(sim PUBLIC nlohmann_json::nlohmann_json)
