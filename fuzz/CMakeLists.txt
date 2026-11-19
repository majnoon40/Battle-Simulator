# m13 item 6 — the libFuzzer targets. Built only with
# BUILD_FUZZING=ON under clang (-fsanitize=fuzzer,address,undefined);
# the harness logic itself is plain code that the normal test suite
# also runs over the seed corpus (tests/test_fuzz_harnesses.cpp), so
# the ASan/UBSan CI job covers it too.
add_library(fuzz_harnesses STATIC harnesses.cpp)
target_include_directories(fuzz_harnesses PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_features(fuzz_harnesses PUBLIC cxx_std_20)
target_link_libraries(fuzz_harnesses PUBLIC sim cli_report game_core)
# Coverage instrumentation for the harness code itself (the executables
# add the full fuzzer runtime at link time).
target_compile_options(fuzz_harnesses PRIVATE
  -fsanitize=fuzzer-no-link,address,undefined)

set(FUZZ_TARGETS
  fuzz_unit_types
  fuzz_scenario
  fuzz_personalities
  fuzz_morale
  fuzz_tactics
  fuzz_formations
  fuzz_commanders
  fuzz_combat
  fuzz_terrain_grid
  fuzz_settings
  fuzz_replay
  fuzz_order_script
  fuzz_simulation)

foreach(target IN LISTS FUZZ_TARGETS)
  add_executable(${target} ${target}.cpp)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_link_libraries(${target} PRIVATE fuzz_harnesses)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
