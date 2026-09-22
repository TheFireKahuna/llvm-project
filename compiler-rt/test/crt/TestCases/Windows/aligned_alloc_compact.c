// Check the production size-class path and its compatibility with native free.
// RUN: %clang_crt_main -std=c17 -O2 -mguard=cf -fno-builtin -UNDEBUG -Wall -Wextra -Werror %s -o %t.exe -Wl,/guard:cf
// RUN: %run %t.exe
// REQUIRES: windows, crt

#include "Inputs/aligned_alloc.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>

int main(void) {
  if (test_max_alignment() == 16)
    return 0;
  static const size_t sizes[] = {1, 17, 31, 32, 33, 63, 64, 65, 97, 1025, 2048};
  unsigned char *blocks[1024];
  for (size_t alignment = 16; alignment <= 2048; alignment <<= 1) {
    for (size_t s = 0; s != sizeof(sizes) / sizeof(sizes[0]); ++s) {
      const size_t size = sizes[s];
      unsigned compact = 0;
      for (unsigned i = 0; i != 1024; ++i) {
        blocks[i] = aligned_alloc(alignment, size);
        assert(blocks[i] && (uintptr_t)blocks[i] % alignment == 0);
        size_t actual = HeapSize(GetProcessHeap(), 0, blocks[i]);
        assert(actual >= size);
        compact += actual < 4096;
        memset(blocks[i], (unsigned char)i, size);
      }
      // A few cold allocations may go through VS/page ranges. Once native
      // LFH activates, this must use sub-page blocks, not one page per object.
      assert(compact >= 900);
      for (unsigned i = 0; i != 1024; ++i) {
        for (size_t j = 0; j != size; ++j)
          assert(blocks[i][j] == (unsigned char)i);
        unsigned char *p = realloc(blocks[i], size + 43);
        assert(p);
        for (size_t j = 0; j != size; ++j)
          assert(p[j] == (unsigned char)i);
        errno = EDOM;
        free(p);
        assert(errno == EDOM);
      }
    }
  }
  // Native page/large selection depends on payload as well as alignment.
  unsigned char *large = aligned_alloc(32, 32 * 1024 * 1024 + 1);
  assert(large && (uintptr_t)large % 32 == 0);
  large[0] = 1;
  large[32 * 1024 * 1024] = 2;
  assert(HeapValidate(GetProcessHeap(), 0, large));
  free(large);
  assert(HeapValidate(GetProcessHeap(), 0, NULL));
  return 0;
}
