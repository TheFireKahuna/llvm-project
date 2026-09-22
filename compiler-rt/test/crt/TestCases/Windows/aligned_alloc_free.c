// Exercise the UCRT deallocator, not just the returned pointer's alignment.
// A forwarding implementation using _aligned_malloc can return an aligned
// interior pointer which ordinary free cannot release, even at alignment 16.
// HeapValidate checks the UCRT heap base before free; errno detects UCRT's
// reported HeapFree failures. These are UCRT integration checks, not ISO C
// requirements on errno. Never pass a Microsoft aligned allocation to free.
// RUN: %clang_crt_dll -std=c17 -O0 %S/Inputs/aligned_alloc.c -o %t.dll
// RUN: %clang_crt_main -std=c17 -O0 -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.exe
// RUN: %run %t.exe %t.dll
// RUN: %clang_crt_dll -std=c23 -O2 -flto=thin -mguard=cf %S/Inputs/aligned_alloc.c -o %t.opt.dll -Wl,/guard:cf
// RUN: %clang_crt_main -std=c23 -O2 -flto=thin -mguard=cf -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.opt.exe -Wl,/guard:cf
// RUN: %run %t.opt.exe %t.opt.dll
// REQUIRES: windows, crt

#include "Inputs/aligned_alloc.h"
#include <assert.h>
#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

struct block {
  void *ptr;
  size_t size;
};

static void fill(struct block b) {
  volatile unsigned char *p = b.ptr;
  for (size_t i = 0; i < b.size; ++i)
    p[i] = (unsigned char)i;
}

static void check_base(struct block b) {
  assert(b.ptr);
  assert(HeapValidate((HANDLE)_get_heap_handle(), 0, b.ptr));
  volatile unsigned char *p = b.ptr;
  for (size_t i = 0; i < b.size; ++i)
    assert(p[i] == (unsigned char)i);
}

static void release(struct block b) {
  if (b.ptr)
    check_base(b);
  errno = 0;
  free(b.ptr);
  assert(errno == 0);
}

struct transfer {
  struct block *blocks;
  size_t count;
  struct block returned;
};

static DWORD WINAPI worker(void *arg) {
  struct transfer *t = arg;
  for (size_t i = 0; i < t->count; ++i)
    release(t->blocks[i]);
  t->returned = (struct block){aligned_alloc(16, 65537), 65537};
  assert(t->returned.ptr);
  fill(t->returned);
  return 0;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  HMODULE dll = LoadLibraryA(argv[1]);
  assert(dll);
  typedef void *(__cdecl * allocate_fn)(size_t, size_t);
  typedef void(__cdecl * free_fn)(void *);
  allocate_fn allocate_in_c = (allocate_fn)GetProcAddress(dll, "allocate_in_c");
  free_fn free_in_c = (free_fn)GetProcAddress(dll, "free_in_c");
  assert(allocate_in_c && free_in_c);

  static const size_t sizes[] = {1,    7,    8,    15,    16,     17,
                                 4095, 4096, 4097, 65537, 1048577};
  struct block blocks[27 * (sizeof(sizes) / sizeof(sizes[0]))];
  size_t count = 0;
  for (size_t alignment = 1; alignment <= test_max_alignment();
       alignment <<= 1) {
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
      struct block b = {allocate_in_c(alignment, sizes[i]), sizes[i]};
      assert(b.ptr);
      fill(b);
      blocks[count++] = b;

      // Opposite ownership direction, while the DLL is still loaded.
      b.ptr = aligned_alloc(alignment, b.size);
      assert(b.ptr);
      fill(b);
      check_base(b);
      errno = 0;
      free_in_c(b.ptr);
      assert(errno == 0);
    }
  }

  errno = EDOM;
  free_in_c(NULL);
  assert(errno == EDOM);

  // Neither the allocating image nor the allocating thread must remain alive
  // for a UCRT allocation to be released by ordinary free.
  assert(FreeLibrary(dll));
  struct transfer t = {blocks, count, {NULL, 0}};
  HANDLE thread = CreateThread(NULL, 0, worker, &t, 0, NULL);
  assert(thread);
  assert(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);
  DWORD code;
  assert(GetExitCodeThread(thread, &code) && code == 0);
  assert(CloseHandle(thread));
  release(t.returned);

  // Cleanup is valid even if allocation returned null. Check the result
  // before release so an implementation returning storage here cannot pass.
  const size_t requests[][2] = {
      {16, 0}, {0, 16}, {3, 16}, {((size_t)1 << 63), 64}, {16, SIZE_MAX - 15}};
  for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
    void *p = aligned_alloc(requests[i][0], requests[i][1]);
    assert(p == NULL);
    release((struct block){p, 0});
  }

  // Microsoft allocations coexist with ours, but retain their own matching
  // deallocator. Test small alignments too: pointer alignment does not tell
  // us which allocation family owns a block.
  for (size_t alignment = 1; alignment <= 4096; alignment <<= 1) {
    void *p = _aligned_malloc(17, alignment);
    assert(p && (uintptr_t)p % alignment == 0);
    fill((struct block){p, 17});
    struct block b = {aligned_alloc(16, 17), 17};
    assert(b.ptr);
    fill(b);
    release(b);
    volatile unsigned char *bytes = p;
    for (size_t i = 0; i < 17; ++i)
      assert(bytes[i] == (unsigned char)i);
    // Failure must retain the original block for its matching deallocator.
    assert(_aligned_realloc(p, SIZE_MAX - 15, alignment) == NULL);
    for (size_t i = 0; i < 17; ++i)
      assert(bytes[i] == (unsigned char)i);
    errno = 0;
    _aligned_free(p);
    assert(errno == 0);
  }
  void *offset = _aligned_offset_malloc(17, 64, 3);
  assert(offset && ((uintptr_t)offset + 3) % 64 == 0);
  fill((struct block){offset, 17});
  _aligned_free(offset);
  errno = EDOM;
  _aligned_free(NULL);
  assert(errno == EDOM);
  return 0;
}
