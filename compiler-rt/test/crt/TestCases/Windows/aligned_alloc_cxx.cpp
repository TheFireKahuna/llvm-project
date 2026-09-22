// The public C and C++ declarations must link to wincrt's C definition.
// Allocation and ordinary free must also work across the C DLL/C++ host
// boundary, in both directions, without allocator-specific deallocation.
// RUN: %clang_crt_dll -std=c17 -O0 %S/Inputs/aligned_alloc.c -o %t.dll -Wl,/implib:%t.lib
// RUN: %clangxx_crt_main -std=c++17 -O0 -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s %t.lib -o %t.exe
// RUN: %run %t.exe
// RUN: %clang_crt_dll -std=c23 -O2 -flto=thin -mguard=cf %S/Inputs/aligned_alloc.c -o %t.opt.dll -Wl,/implib:%t.opt.lib -Wl,/guard:cf
// RUN: %clangxx_crt_main -std=c++23 -O2 -flto=thin -mguard=cf -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s %t.opt.lib -o %t.opt.exe -Wl,/guard:cf
// RUN: %run %t.opt.exe
// REQUIRES: windows, crt

#include "Inputs/aligned_alloc.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <type_traits>

extern "C" __declspec(dllimport) void *allocate_in_c(size_t, size_t);
extern "C" __declspec(dllimport) void free_in_c(void *);

static_assert(std::is_same<decltype(std::aligned_alloc(16, 64)), void *>::value,
              "std::aligned_alloc declared");
static_assert(std::is_same<decltype(::aligned_alloc(16, 64)), void *>::value,
              "::aligned_alloc declared");

static void check(void *p, size_t alignment, size_t size) {
  assert(p);
  assert(reinterpret_cast<std::uintptr_t>(p) % alignment == 0);
  volatile unsigned char *bytes = static_cast<unsigned char *>(p);
  for (size_t i = 0; i < size; ++i)
    bytes[i] = static_cast<unsigned char>(i);
  for (size_t i = 0; i < size; ++i)
    assert(bytes[i] == static_cast<unsigned char>(i));
}

int main() {
  assert(&std::aligned_alloc == &::aligned_alloc);
  for (size_t alignment = 1; alignment <= test_max_alignment();
       alignment <<= 1) {
    void *p = std::aligned_alloc(alignment, 256);
    check(p, alignment, 256);
    std::free(p);

    p = ::aligned_alloc(alignment, 256);
    check(p, alignment, 256);
    free_in_c(p);

    p = allocate_in_c(alignment, 256);
    check(p, alignment, 256);
    std::free(p);
  }
  assert(std::aligned_alloc(24, 64) == nullptr);
  std::free(std::aligned_alloc(16, 0));
  return 0;
}
