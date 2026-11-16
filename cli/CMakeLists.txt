# The shared CLI utility library: argument parsing, terrain map loading
# (PNG via the pinned stb_image, JSON grids) and the --mod overlay
# resolver. battle_game links it too, so the window loads maps and mods
# exactly like the CLI.
add_library(cli_args STATIC
  src/cli_args.cpp
  src/map_load.cpp
  src/mods.cpp
  src/trace.cpp)
target_include_directories(cli_args PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(cli_args PUBLIC cxx_std_20)
target_link_libraries(cli_args PUBLIC sim stb_headers nlohmann_json::nlohmann_json)

# M10 Part 2a: the battle report generators (markdown/JSON/CSV/PNG),
# shared by battle_cli and the test suite.
add_library(cli_report STATIC src/report.cpp src/report_png.cpp src/replay.cpp)
target_include_directories(cli_report PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(cli_report PUBLIC cxx_std_20)
target_link_libraries(cli_report PUBLIC sim render game_core)

add_executable(battle_cli src/main.cpp)
target_compile_features(battle_cli PRIVATE cxx_std_20)
# M7: the CLI links the viewer's pure drawing pipeline (game_core camera/
# view/ascii + render DrawList/rasterizer/PNG) for the headless
# --render-frame/--render-frames/--ascii modes; no raylib anywhere.
target_link_libraries(battle_cli PRIVATE sim render game_core cli_args cli_report)
