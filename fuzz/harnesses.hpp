#pragma once

// m13 item 6 — the fuzzing harnesses. Every harness is a plain
// function over raw bytes (no libFuzzer dependency): fuzz/<name>.cpp
// wraps one in LLVMFuzzerTestOneInput for the clang/libFuzzer builds
// (BUILD_FUZZING=ON), and tests/test_fuzz_harnesses.cpp runs every
// harness over the committed seed corpus (fuzz/corpus/<name>/) inside
// the normal ctest suite — which the ASan/UBSan CI job then covers
// too.
//
// The contract: a harness NEVER crashes, hangs or corrupts memory on
// any input. Parse rejections are the parsers' job — every harness
// catches std::exception (and anything else) and returns 0; a crash,
// a sanitizer report or a timeout is the finding.

#include <cstddef>
#include <cstdint>

namespace fuzz {

int unit_types_harness(const std::uint8_t* data, std::size_t size);
int scenario_harness(const std::uint8_t* data, std::size_t size);
int personalities_harness(const std::uint8_t* data, std::size_t size);
int morale_harness(const std::uint8_t* data, std::size_t size);
int tactics_harness(const std::uint8_t* data, std::size_t size);
int formations_harness(const std::uint8_t* data, std::size_t size);
int commanders_harness(const std::uint8_t* data, std::size_t size);
int combat_harness(const std::uint8_t* data, std::size_t size);
int terrain_grid_harness(const std::uint8_t* data, std::size_t size);
int settings_harness(const std::uint8_t* data, std::size_t size);
int replay_harness(const std::uint8_t* data, std::size_t size);
int order_script_harness(const std::uint8_t* data, std::size_t size);

// Scenario text -> parse -> build a small simulation -> run a few
// ticks. Catches simulation-level crashes/UB on weird-but-valid
// scenarios (bounded: at most 1,000 units and 10 ticks per input, so
// the fuzzer stays fast; the full-scale battle space is the test
// suite's job).
int simulation_harness(const std::uint8_t* data, std::size_t size);

}  // namespace fuzz
