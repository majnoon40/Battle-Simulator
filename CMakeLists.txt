cmake_minimum_required(VERSION 3.16)
project(autobattle VERSION 0.1.0 LANGUAGES CXX
        DESCRIPTION "Deterministic 2D autonomous battle simulation")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Executables land directly in the build root (e.g. ./build/battle_cli).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(BUILD_GAME "Build the optional raylib graphical application (not implemented in M1)" OFF)

# Deterministic floating point: no fast-math, no FMA contraction.
# The same flags apply to every build used for determinism comparisons.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fno-fast-math -ffp-contract=off)
endif()

include(FetchContent)

# nlohmann::json — pinned to exact release tag v3.11.3.
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
set(JSON_ImplicitConversions OFF CACHE INTERNAL "")
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(json)

# stb (single-header libraries) — pinned to an exact commit; only
# stb_image_write.h is used, by render/ (the headless PNG backend).
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 9600522de53973336fa033a289858a4354cbb04c
  GIT_SHALLOW TRUE
  GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(stb)
add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE ${stb_SOURCE_DIR})

add_subdirectory(sim)
add_subdirectory(render)
add_subdirectory(game)
add_subdirectory(cli)

enable_testing()
add_subdirectory(tests)

# battle_game (the raylib window) is declared inside game/ and builds only
# with BUILD_GAME=ON; raylib is fetched and pinned there. sim, battle_cli,
# render (software PNG) and tests build without any display dependency.
# raylib needs X11/GL development headers — see docs/viewer.md for the
# per-OS list.
