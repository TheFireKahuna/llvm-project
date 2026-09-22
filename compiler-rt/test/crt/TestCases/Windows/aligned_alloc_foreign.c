// A foreign startup does not run wincrt's EXE segment-heap policy gate. Strip
// the test image's manifest to exercise its DLL on the actual default heap.
// RUN: %clang_crt_dll -std=c17 -O2 -mguard=cf %S/Inputs/aligned_alloc.c -o %t.dll -Wl,/guard:cf,/implib:%t.lib
// RUN: %clang_crt_main -std=c17 -O2 -mguard=cf -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s %t.lib -o %t.exe -Wl,/entry:foreign_entry,/guard:cf
// RUN: %python %S/Inputs/segment_heap_without_manifest.py %t.exe %t.foreign.exe %t.dll --require-success
// REQUIRES: windows, crt
#include "Inputs/aligned_alloc.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
__declspec(dllimport) void *allocate_in_c(size_t, size_t);
__declspec(dllimport) int posix_allocate_in_c(void **, size_t, size_t);

void foreign_entry(void) {
  void *p = allocate_in_c(16, 17);
  assert(p && ((uintptr_t)p & 15) == 0);
  free(p);
  errno = 0;
  p = allocate_in_c(4096, 1048576);
  if (test_max_alignment() == 16) {
    assert(!p && errno == EINVAL);
  } else {
    assert(p && ((uintptr_t)p & 4095) == 0);
    ((volatile char *)p)[1048575] = 42;
  }
  free(p);
  void *output = &p;
  errno = EDOM;
  const int status = posix_allocate_in_c(&output, 4096, 1048576);
  assert(errno == EDOM);
  if (test_max_alignment() == 16) {
    assert(status == ENOMEM && output == &p);
  } else {
    assert(status == 0 && output && ((uintptr_t)output & 4095) == 0);
    free(output);
  }
  ExitProcess(0);
}
