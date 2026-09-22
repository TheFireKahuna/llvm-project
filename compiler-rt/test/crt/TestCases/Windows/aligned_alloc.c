// C17 7.22.3/7.22.3.1 and C23 7.24.3/7.24.3.1, including WG14 DR 460:
// https://www.open-std.org/jtc1/sc22/wg14/issues/c11c17/issue0460.html
// The final correction removes the size-multiple restriction. Test it in
// every dialect: wincrt provides the corrected semantics in C11 mode too.
// Disable builtins so optimization cannot remove the calls under test or
// infer the very alignment/disjointness properties that we need to check.
// RUN: %clang_crt_main -std=c11 -O0 -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.exe
// RUN: %run %t.exe
// RUN: %clang_crt_main -std=c17 -O2 -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.opt.exe
// RUN: %run %t.opt.exe
// RUN: %clang_crt_main -std=c23 -O2 -flto=thin -mguard=cf -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.lto.exe -Wl,/guard:cf
// RUN: %run %t.lto.exe
// REQUIRES: windows, crt

#include "Inputs/aligned_alloc.h"
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct block {
  unsigned char *ptr;
  size_t size;
};

static void fill(struct block b, unsigned char value) {
  volatile unsigned char *p = b.ptr;
  for (size_t i = 0; i < b.size; ++i)
    p[i] = value;
}

static void verify(struct block b, unsigned char value) {
  volatile unsigned char *p = b.ptr;
  for (size_t i = 0; i < b.size; ++i)
    assert(p[i] == value);
}

static void disjoint(struct block a, struct block b) {
  uintptr_t x = (uintptr_t)a.ptr, y = (uintptr_t)b.ptr;
  assert(x != y);
  if (x < y)
    assert(y - x >= a.size);
  else
    assert(x - y >= b.size);
}

static void test_storage(void) {
  static const size_t alignments[] = {1,  2,   4,    8,     16,      32,
                                      64, 256, 4096, 65536, 1 << 20, 1 << 26};
  static const size_t sizes[] = {1, 7, 16, 17, 63, 4096, 65536, 1 << 20};
  struct block blocks[sizeof(alignments) / sizeof(alignments[0]) *
                      (sizeof(sizes) / sizeof(sizes[0]))];
  size_t count = 0;
  _Static_assert(_Alignof(max_align_t) <= 16,
                 "fundamental alignment supported");

  // Keep allocations live together, including ordinary C allocations. Their
  // requested byte ranges must be disjoint and remain valid until freed.
  struct block ordinary = {malloc(1024), 1024};
  struct block zeroed = {calloc(1024, 1), 1024};
  assert(ordinary.ptr && zeroed.ptr);
  fill(ordinary, 0xAB);
  for (size_t a = 0; a < sizeof(alignments) / sizeof(alignments[0]); ++a) {
    if (alignments[a] > test_max_alignment())
      break;
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
      struct block b = {aligned_alloc(alignments[a], sizes[s]), sizes[s]};
      // These modest requests must work in the test environment: an
      // implementation that always returns null must not pass this test.
      assert(b.ptr);
      assert((uintptr_t)b.ptr % alignments[a] == 0);
      // The general allocation alignment guarantee still applies when the
      // requested alignment is smaller. C23 restricts this guarantee to
      // types whose size fits the allocation; C17 does not.
#if __STDC_VERSION__ >= 202311L
      if (b.size >= sizeof(max_align_t))
#endif
        assert((uintptr_t)b.ptr % _Alignof(max_align_t) == 0);
      disjoint(b, ordinary);
      disjoint(b, zeroed);
      for (size_t i = 0; i < count; ++i)
        disjoint(b, blocks[i]);
      blocks[count++] = b;
      // Initialize before reading: aligned_alloc's contents are indeterminate.
      fill(b, (unsigned char)count);
    }
  }
  for (size_t i = 0; i < count; ++i)
    verify(blocks[i], (unsigned char)(i + 1));
  for (size_t i = 0; i < count; i += 2)
    free(blocks[i].ptr);
  for (size_t i = 1; i < count; i += 2) {
    verify(blocks[i], (unsigned char)(i + 1));
    free(blocks[i].ptr);
  }
  verify(ordinary, 0xAB);
  verify(zeroed, 0);
  free(ordinary.ptr);
  free(zeroed.ptr);
}

static void test_failure(void) {
  if (test_max_alignment() == 16) {
    errno = 0;
    assert(aligned_alloc(32, 64) == NULL && errno == EINVAL);
  }
  // Every unsupported alignment must return null, for both zero and
  // nonzero sizes. An unsupported alignment must not accidentally succeed
  // just because malloc happened to align the result.
  static const size_t invalid[] = {0, 3, 6, 15, 17, 24, 31, SIZE_MAX};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    errno = EDOM;
    assert(aligned_alloc(invalid[i], 0) == NULL);
    assert(errno == EINVAL);
    errno = EDOM;
    assert(aligned_alloc(invalid[i], 64) == NULL);
    assert(errno == EINVAL);
  }
  for (size_t alignment = ((size_t)1 << 63); alignment != 0; alignment <<= 1) {
    assert(aligned_alloc(alignment, 0) == NULL);
    assert(aligned_alloc(alignment, alignment) == NULL);
  }
  for (size_t alignment = 1; alignment <= test_max_alignment();
       alignment <<= 1) {
    // Impossible sizes on Win64, with valid alignments and size multiples.
    // Exercise both native failure and wincrt's excessive-size rejection
    // without consuming physical memory or relying on machine exhaustion.
    errno = EDOM;
    assert(aligned_alloc(alignment, (SIZE_MAX / 2) & ~(alignment - 1)) == NULL);
    assert(errno == ENOMEM);
    errno = EDOM;
    assert(aligned_alloc(alignment, SIZE_MAX & ~(alignment - 1)) == NULL);
    assert(errno == ENOMEM);
  }
  // A failed allocation must not invalidate an existing allocation.
  struct block b = {aligned_alloc(16, 256), 256};
  assert(b.ptr);
  fill(b, 0xCD);
  assert(aligned_alloc(16, SIZE_MAX - 15) == NULL);
  verify(b, 0xCD);
  free(b.ptr);
}

static void test_wincrt_policy(void) {
  // POSIX.1-2024 aligned_alloc: a null zero-size result indicates an error and
  // must set errno. EINVAL is the permitted zero-size rejection policy. Never
  // dereference a zero-size result or assume allocation order or initial data.
  for (size_t alignment = 1; alignment <= test_max_alignment();
       alignment <<= 1) {
    errno = EDOM;
    void *p = aligned_alloc(alignment, 0);
    assert(p == NULL);
    assert(errno == EINVAL);
    free(p);
  }
  errno = 0;
  assert(aligned_alloc(3, 64) == NULL);
  assert(errno == EINVAL);
  errno = 0;
  assert(aligned_alloc(24, 64) == NULL);
  assert(errno == EINVAL);
  errno = 0;
  assert(aligned_alloc(16, SIZE_MAX - 15) == NULL);
  assert(errno == ENOMEM);
}

static void test_realloc(void) {
  // C17 7.22.3.5 / C23 7.24.3.7 also permit realloc on a pointer returned
  // by aligned_alloc. Only fundamental alignment is required afterwards.
  for (size_t alignment = 16; alignment <= test_max_alignment();
       alignment <<= 1) {
    struct block b = {aligned_alloc(alignment, 1048577), 1048577};
    assert(b.ptr);
    fill(b, 0xEF);
    unsigned char *p = realloc(b.ptr, 2097153);
    assert(p);
    b.ptr = p;
    verify(b, 0xEF);
    p = realloc(b.ptr, 128);
    assert(p);
    b.ptr = p;
    b.size = 128;
    verify(b, 0xEF);
    assert(realloc(b.ptr, SIZE_MAX - 15) == NULL);
    verify(b, 0xEF);
    free(b.ptr);
  }
}

int main(void) {
  test_storage();
  test_failure();
  test_wincrt_policy();
  test_realloc();
  return 0;
}
