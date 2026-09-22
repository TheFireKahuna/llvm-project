// RUN: %clang_crt_main -O2 -UNDEBUG %s -o %t.exe
// RUN: %run %t.exe
// RUN: %clang_crt_main -O2 -UNDEBUG -DINTRINSICS_FIRST %s -o %t.reverse.exe
// RUN: %run %t.reverse.exe
// REQUIRES: windows, crt

// Both include orders must retain Clang's free-compatible intrinsic pair.
// clang-format off
#ifdef INTRINSICS_FIRST
#  include <mm_malloc.h>
#  include <malloc.h>
#else
#  include <malloc.h>
#  include <mm_malloc.h>
#endif
// clang-format on
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_mm_malloc) || defined(_mm_free)
#  error "UCRT must not redirect the intrinsic allocation pair"
#endif

int main(void) {
  const size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 4096};
  for (size_t i = 0; i < sizeof(alignments) / sizeof(*alignments); ++i) {
    void *p = _mm_malloc(1031, alignments[i]);
    assert(p && (uintptr_t)p % alignments[i] == 0);
    assert(_msize(p) >= 1031);
    memset(p, 0x5a, 1031);
    p = realloc(p, 2062);
    assert(p);
    for (size_t j = 0; j < 1031; ++j)
      assert(((unsigned char *)p)[j] == 0x5a);
    _mm_free(p);
    p = _mm_malloc(1031, alignments[i]);
    assert(p);
    free(p);
  }
  volatile size_t invalid = 3;
  assert(_mm_malloc(37, invalid) == NULL);
  invalid = 0;
  assert(_mm_malloc(37, invalid) == NULL);
  _mm_free(NULL);
  return 0;
}
