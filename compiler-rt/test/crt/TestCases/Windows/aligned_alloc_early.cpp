// First extended use under loader lock, including a dependency DLL initializer.
// RUN: %clang_crt_dll -std=c++17 -O2 -mguard=cf -fno-builtin -UNDEBUG -DPRODUCER %s -o %t.dll -Wl,/guard:cf,/implib:%t.lib
// RUN: %clang_crt_main -std=c++17 -O2 -mguard=cf -fno-builtin -UNDEBUG %s %t.lib -o %t.exe -Wl,/guard:cf
// RUN: %run %t.exe
// REQUIRES: windows, crt
#include "Inputs/aligned_alloc.h"
#include <assert.h>
#include <stdlib.h>

static void *early;
struct Allocate {
  Allocate() {
    const size_t alignment = test_max_alignment();
    early = aligned_alloc(alignment, 1048577);
    assert(early && (uintptr_t)early % alignment == 0);
    ((char *)early)[0] = 42;
    ((char *)early)[1048576] = 43;
  }
};
static Allocate allocate;
#ifdef PRODUCER
extern "C" __declspec(dllexport) void *allocated_early() { return early; }
#else
extern "C" __declspec(dllimport) void *allocated_early();
int main() {
  void *p = allocated_early();
  assert(((char *)p)[0] == 42 && ((char *)p)[1048576] == 43);
  free(p);
  assert(((char *)early)[0] == 42 && ((char *)early)[1048576] == 43);
  free(early);
}
#endif
