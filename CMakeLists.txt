cmake_minimum_required(VERSION 3.16)
# m13 item 7: the project version follows sim::kSimVersion (the
# packaging below and the release archives use it).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sim/include/sim/version.hpp"
     SIM_VERSION_HPP)
string(REGEX MATCH "kSimVersion = \"([^\"]+)\"" _ "${SIM_VERSION_HPP}")
set(BATTLE_SIM_VERSION "${CMAKE_MATCH_1}")
project(autobattle VERSION ${BATTLE_SIM_VERSION} LANGUAGES CXX
        DESCRIPTION "Deterministic 2D autonomous battle simulation")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Executables land directly in the build root (e.g. ./build/battle_cli).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(BUILD_GAME "Build the optional raylib graphical application (not implemented in M1)" OFF)
# m13 item 6: the libFuzzer targets (clang only — -fsanitize=fuzzer).
option(BUILD_FUZZING "Build the libFuzzer fuzzing targets (clang)" OFF)

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
# No GIT_SHALLOW: on 2026-11-19 GitHub's shallow fetch of this pinned
# commit started failing ("fatal: unable to read tree <sha>" after a
# full object transfer) on both local machines and CI runners; a full
# clone fetches the commit fine and costs ~7 MB.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 9600522de53973336fa033a289858a4354cbb04c
  GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(stb)
# m13 item 4: the deterministic worker pool (sim/thread_pool.hpp) needs
# std::thread and its condition variables.
find_package(Threads REQUIRED)

add_library(stb_headers INTERFACE)
target_include_directories(stb_headers INTERFACE ${stb_SOURCE_DIR})

add_subdirectory(sim)
add_subdirectory(render)
add_subdirectory(game)
add_subdirectory(cli)

if(BUILD_FUZZING)
  add_subdirectory(fuzz)
endif()

# m13 item 7: install + package rules. `cmake --install` lays out
# bin/battle_cli + data/ + docs/ + the project texts; `cpack` builds
# the release archive (TGZ + ZIP) with the sim version from
# sim/version.hpp.
install(TARGETS battle_cli RUNTIME DESTINATION bin)
install(DIRECTORY data/ DESTINATION data)
install(DIRECTORY docs/ DESTINATION docs FILES_MATCHING PATTERN "*.md")
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
set(CPACK_PACKAGE_NAME "battle-simulator")
set(CPACK_PACKAGE_VERSION "${BATTLE_SIM_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Deterministic 2D autonomous battle simulation")
set(CPACK_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_GENERATOR "TGZ")
include(CPack)

enable_testing()
add_subdirectory(tests)

# battle_game (the raylib window) is declared inside game/ and builds only
# with BUILD_GAME=ON; raylib is fetched and pinned there. sim, battle_cli,
# render (software PNG) and tests build without any display dependency.
# raylib needs X11/GL development headers — see docs/viewer.md for the
# per-OS list.
