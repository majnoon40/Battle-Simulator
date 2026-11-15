// Shared operator new/delete replacement counting heap allocations for the
// whole battle_tests binary (M3/M4 steady-state allocation tests). GoogleTest
// allocates between tests; assertions only compare counts across a tick()
// window, outside which nothing runs.
#include <cstdlib>
#include <new>
#include <stdexcept>

#include "allocation_counter.hpp"

namespace {
std::size_t g_allocations = 0;
}

std::size_t* allocation_counter() noexcept { return &g_allocations; }

void* operator new(std::size_t size) {
  ++g_allocations;
  if (size == 0) {
    size = 1;
  }
  void* pointer = std::malloc(size);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void* operator new[](std::size_t size) { return operator new(size); }

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
