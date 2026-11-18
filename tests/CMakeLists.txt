include(FetchContent)

# GoogleTest — pinned to exact release tag v1.14.0.
set(INSTALL_GTEST OFF CACHE INTERNAL "")
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(battle_tests
  allocation_counter.cpp
  test_rng.cpp
  test_units.cpp
  test_simulation.cpp
  test_json_export.cpp
  test_cli_args.cpp
  test_unit_types.cpp
  test_scenario.cpp
  test_type_battle.cpp
  test_spatial_hash.cpp
  test_ai.cpp
  test_morale.cpp
  test_squads.cpp
  test_viewer.cpp
  test_terrain.cpp
  test_earth_maps.cpp
  test_tactics.cpp
  test_events.cpp
  test_replay.cpp
  test_orders.cpp
  test_ui.cpp
  test_screens.cpp
  test_shell.cpp
  test_avail_end.cpp
  test_combat.cpp
  test_output_failures.cpp
  test_thread_pool.cpp
  test_threads.cpp)
target_compile_features(battle_tests PRIVATE cxx_std_20)
target_link_libraries(battle_tests PRIVATE sim render game_core cli_args cli_report gtest_main)
# The committed data files are validated by the loader tests.
target_compile_definitions(battle_tests PRIVATE
  TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
  BATTLE_CLI_PATH="${CMAKE_BINARY_DIR}/battle_cli")

include(GoogleTest)
gtest_discover_tests(battle_tests)
