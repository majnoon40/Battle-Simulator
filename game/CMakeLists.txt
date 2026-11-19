# game/ — the viewer (M7). game_core is the pure, raylib-free library
# (camera math, fixed-timestep accumulator, picking, HUD text, colors and
# shapes, ascii map) and is always built; battle_game is the raylib window
# and is built only with BUILD_GAME=ON (raylib is fetched and pinned to an
# exact release tag in that case — see the root CMakeLists and
# docs/viewer.md for the per-OS dev libraries raylib needs).
add_library(game_core STATIC
  core/src/battle_input.cpp
  core/src/theme.cpp
  core/src/picking.cpp
  core/src/settings.cpp
  core/src/setup_edit.cpp
  core/src/ascii.cpp)
add_library(game::game_core ALIAS game_core)
target_include_directories(game_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_compile_features(game_core PUBLIC cxx_std_20)
target_link_libraries(game_core PUBLIC sim)

if(BUILD_GAME)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(BUILD_GAMES OFF CACHE INTERNAL "")
  set(USE_MSVC_RUNTIME_LIBRARY_DLL OFF CACHE INTERNAL "")
  # raylib release tags have no "v" prefix; 5.5 is
  # c1ab645ca298a2801097931d1079b10ff7eb9df8.
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE)
  FetchContent_MakeAvailable(raylib)
  add_executable(battle_game src/main.cpp)
  target_compile_features(battle_game PRIVATE cxx_std_20)
  target_link_libraries(battle_game PRIVATE sim render game_core cli_args
  cli_report raylib)
endif()
