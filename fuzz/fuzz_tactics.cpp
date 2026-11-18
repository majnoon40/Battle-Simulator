// m13 item 6 — the tactics libFuzzer target (see fuzz/harnesses.hpp).
#include <cstddef>
#include <cstdint>

#include "fuzz/harnesses.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  return fuzz::tactics_harness(data, size);
}
