// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// aligned_alloc sets errno to EINVAL for an alignment that is not a power of
// two, and to ENOMEM for one above 2 MiB or a size the heap refuses.
// posix_memalign returns those errors instead, for an alignment that is also
// not a multiple of sizeof(void *), and changes neither errno nor the
// pointer when it fails. Neither changes errno when it succeeds.

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MIB ((size_t)1024 * 1024)
#define UNCHANGED 12345

// Keeps each result in use, so that no call can be optimized away.
static void *volatile Result;

static const char *name(int Error) {
  return Error == 0           ? "0"
         : Error == EINVAL    ? "EINVAL"
         : Error == ENOMEM    ? "ENOMEM"
         : Error == UNCHANGED ? "unchanged"
                              : "other";
}

static void alignedAlloc(size_t Alignment, size_t Size) {
  errno = UNCHANGED;
  void *P = Result = aligned_alloc(Alignment, Size);
  int Error = errno;
  printf("aligned_alloc(%zu, %zu): %s, errno %s\n", Alignment, Size,
         P ? "block" : "null", name(Error));
  free(P);
}

static void posixMemalign(size_t Alignment, size_t Size) {
  void *Unchanged = &Unchanged;
  void *P = Unchanged;
  errno = UNCHANGED;
  int Error = posix_memalign(&P, Alignment, Size);
  int Errno = errno;
  Result = P;
  printf("posix_memalign(%zu, %zu): %s, errno %s, pointer %s\n", Alignment,
         Size, name(Error), name(Errno), P == Unchanged ? "unchanged" : "set");
  if (!Error)
    free(P);
}

int main(void) {
  alignedAlloc(0, 16);
  alignedAlloc(24, 16);
  alignedAlloc(4 * MIB, 16);
  alignedAlloc(1, 16);
  alignedAlloc(16, SIZE_MAX);
  alignedAlloc(64, SIZE_MAX);
  alignedAlloc(2 * MIB, SIZE_MAX);

  posixMemalign(0, 16);
  posixMemalign(4, 16);
  posixMemalign(24, 16);
  posixMemalign(4 * MIB, 16);
  posixMemalign(8, 16);
  posixMemalign(16, SIZE_MAX);
  posixMemalign(64, SIZE_MAX);
  return 0;
}

// CHECK:      aligned_alloc(0, 16): null, errno EINVAL
// CHECK-NEXT: aligned_alloc(24, 16): null, errno EINVAL
// CHECK-NEXT: aligned_alloc(4194304, 16): null, errno ENOMEM
// CHECK-NEXT: aligned_alloc(1, 16): block, errno unchanged
// CHECK-NEXT: aligned_alloc(16, [[MAX:[0-9]+]]): null, errno ENOMEM
// CHECK-NEXT: aligned_alloc(64, [[MAX]]): null, errno ENOMEM
// CHECK-NEXT: aligned_alloc(2097152, [[MAX]]): null, errno ENOMEM

// CHECK-NEXT: posix_memalign(0, 16): EINVAL, errno unchanged, pointer unchanged
// CHECK-NEXT: posix_memalign(4, 16): EINVAL, errno unchanged, pointer unchanged
// CHECK-NEXT: posix_memalign(24, 16): EINVAL, errno unchanged, pointer unchanged
// CHECK-NEXT: posix_memalign(4194304, 16): ENOMEM, errno unchanged, pointer unchanged
// CHECK-NEXT: posix_memalign(8, 16): 0, errno unchanged, pointer set
// CHECK-NEXT: posix_memalign(16, [[MAX]]): ENOMEM, errno unchanged, pointer unchanged
// CHECK-NEXT: posix_memalign(64, [[MAX]]): ENOMEM, errno unchanged, pointer unchanged
