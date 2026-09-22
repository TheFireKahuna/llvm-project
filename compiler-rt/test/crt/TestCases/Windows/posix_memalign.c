// Exercise the real C entry and its C++ declaration, including errno and output
// preservation. Disable builtins so the optimizer cannot supply the properties
// being tested or remove allocations whose failure is observable here.
// RUN: %clang_crt_dll -std=c17 -O0 %S/Inputs/aligned_alloc.c -o %t.dll
// RUN: %clang_crt_main -std=c17 -O0 -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.exe
// RUN: %run %t.exe %t.dll
// RUN: %clang_crt_dll -std=c23 -O2 -flto=thin -mguard=cf %S/Inputs/aligned_alloc.c -o %t.opt.dll -Wl,/guard:cf
// RUN: %clang_crt_main -std=c23 -O2 -flto=thin -mguard=cf -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.opt.exe -Wl,/guard:cf
// RUN: %run %t.opt.exe %t.opt.dll
// RUN: %clangxx_crt_main -x c++ -std=c++17 -O2 -mguard=cf -UNDEBUG -fno-builtin -Wall -Wextra -Werror %s -o %t.cxx.exe -Wl,/guard:cf
// RUN: %run %t.cxx.exe %t.opt.dll
// REQUIRES: windows, crt

#include "Inputs/aligned_alloc.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

static void expect_failure(size_t alignment, size_t size, int error) {
  unsigned char sentinel;
  void *p = &sentinel;
  errno = EDOM;
  assert(posix_memalign(&p, alignment, size) == error);
  assert(p == &sentinel);
  assert(errno == EDOM);
}

static void check_storage(void *memory, size_t alignment, size_t size) {
  assert(memory && ((uintptr_t)memory & (alignment - 1)) == 0);
  assert(HeapValidate(GetProcessHeap(), 0, memory));
  volatile unsigned char *bytes = (volatile unsigned char *)memory;
  for (size_t i = 0; i != size; ++i)
    bytes[i] = (unsigned char)i;
  for (size_t i = 0; i != size; ++i)
    assert(bytes[i] == (unsigned char)i);
}

static void check_requests(void) {
  const size_t invalid[] = {0, 1, 2, 4, 7, 12, 24, 4095, SIZE_MAX};
  for (size_t i = 0; i != sizeof(invalid) / sizeof(invalid[0]); ++i) {
    expect_failure(invalid[i], 17, EINVAL);
    expect_failure(invalid[i], 0, EINVAL);
  }

  // Every valid zero-size request succeeds without allocating, including an
  // alignment too large to service for a nonzero request. This differs from
  // aligned_alloc's permitted zero-size rejection policy.
  for (size_t alignment = sizeof(void *); alignment; alignment <<= 1) {
    unsigned char sentinel;
    void *p = &sentinel;
    errno = EDOM;
    assert(posix_memalign(&p, alignment, 0) == 0);
    assert(p == NULL && errno == EDOM);
  }

  const size_t maximum = test_max_alignment();
  const size_t sizes[] = {1, 17, 97, 257, 4096, 1048577};
  for (size_t alignment = sizeof(void *); alignment <= maximum;
       alignment <<= 1) {
    for (size_t i = 0; i != sizeof(sizes) / sizeof(sizes[0]); ++i) {
      void *p = NULL;
      void *q = NULL;
      errno = EDOM;
      assert(posix_memalign(&p, alignment, sizes[i]) == 0);
      assert(errno == EDOM);
      assert(posix_memalign(&q, alignment, sizes[i]) == 0);
      assert(errno == EDOM);
      check_storage(p, alignment, sizes[i]);
      check_storage(q, alignment, sizes[i]);
      assert((uintptr_t)p + sizes[i] <= (uintptr_t)q ||
             (uintptr_t)q + sizes[i] <= (uintptr_t)p);
      errno = EDOM;
      free(q);
      free(p);
      assert(errno == EDOM);
    }
  }

  expect_failure(sizeof(void *), SIZE_MAX, ENOMEM);
  expect_failure(4096, SIZE_MAX, ENOMEM);
  expect_failure((size_t)1 << (sizeof(size_t) * 8 - 1), 1, ENOMEM);
  // Unsupported capability is not bad alignment.
  if (maximum == 16)
    expect_failure(32, 17, ENOMEM);
}

static DWORD WINAPI allocate_on_thread(void *result) {
  errno = ERANGE;
  const int error = posix_memalign((void **)result, sizeof(void *), 97);
  assert(errno == ERANGE);
  return (DWORD)error;
}

static void check_thread(void) {
  void *p = NULL;
  HANDLE thread = CreateThread(NULL, 0, allocate_on_thread, &p, 0, NULL);
  assert(thread);
  assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
  DWORD result = 1;
  assert(GetExitCodeThread(thread, &result) && result == 0);
  CloseHandle(thread);
  check_storage(p, sizeof(void *), 97);
  free(p);
}

static void check_dll(const char *path) {
  typedef int (*allocate_fn)(void **, size_t, size_t);
  typedef void (*free_fn)(void *);
  HMODULE module = LoadLibraryA(path);
  assert(module);
  allocate_fn allocate =
      (allocate_fn)GetProcAddress(module, "posix_allocate_in_c");
  free_fn release = (free_fn)GetProcAddress(module, "free_in_c");
  assert(allocate && release);

  const size_t alignment = test_max_alignment() > 16 ? 4096 : 8;
  void *from_host = NULL;
  void *from_dll = NULL;
  errno = EDOM;
  assert(posix_memalign(&from_host, alignment, 257) == 0);
  assert(allocate(&from_dll, alignment, 257) == 0);
  assert(errno == EDOM);
  check_storage(from_host, alignment, 257);
  check_storage(from_dll, alignment, 257);
  release(from_host);
  assert(FreeLibrary(module));
  // Metadata and ownership must survive the producing DLL's unload.
  check_storage(from_dll, alignment, 257);
  errno = EDOM;
  free(from_dll);
  assert(errno == EDOM);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  check_requests();
  check_thread();
  check_dll(argv[1]);
  return 0;
}
