#pragma once

#include <cstddef>

// Shared allocation counter for the battle_tests binary: allocation_counter.cpp
// defines it together with the global operator new/delete replacements, so
// exactly one definition is linked into the test executable. Tests snapshot
// *allocation_counter() around a window and assert it did not move.
[[nodiscard]] std::size_t* allocation_counter() noexcept;
