// m13 item 6 — the combat libFuzzer target (see fuzz/harnesses.hpp).
#include <cstddef>
#include <cstdint>

#include "fuzz/harnesses.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  return fuzz::combat_harness(data, size);
}
