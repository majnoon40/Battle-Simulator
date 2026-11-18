#pragma once

#include <sys/wait.h>

// Decode a wait status the way WEXITSTATUS does. The macro itself cannot
// be used in C++ on the macOS SDKs of newer Xcode: its _W_INT expands
// through a C compound literal whose address Apple Clang refuses to take
// in C++ mode ("cannot take the address of an rvalue"); gcc and Linux
// clang accept the extension. The manual decode is bit-identical on
// every POSIX platform this project builds on.
inline int exit_status_of(int raw) {
#if defined(__APPLE__)
  return (raw >> 8) & 0xff;
#else
  return WEXITSTATUS(raw);
#endif
}
