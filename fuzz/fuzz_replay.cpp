// m13 item 6 — the replay libFuzzer target (see fuzz/harnesses.hpp).
#include <cstddef>
#include <cstdint>

#include "fuzz/harnesses.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  return fuzz::replay_harness(data, size);
}
