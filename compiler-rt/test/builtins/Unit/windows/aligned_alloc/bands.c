// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// On the segment heap, every alignment up to 2 MiB gets an aligned block, of
// any size, from both functions, and leaves the last error alone. The C
// runtime's free, realloc and _msize take each block as a block of malloc's.

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define KIB ((size_t)1024)
#define MIB (1024 * KIB)

static int Failures;

static void fail(const char *What, size_t Alignment, size_t Size) {
  printf("%s: alignment %zu, size %zu\n", What, Alignment, Size);
  ++Failures;
}

static void use(unsigned char *P, size_t Alignment, size_t Size) {
  if (!P) {
    fail("no block", Alignment, Size);
    return;
  }
  if ((uintptr_t)P % Alignment)
    fail("misaligned", Alignment, Size);
  if (_msize(P) < Size)
    fail("_msize too small", Alignment, Size);
  memset(P, 0x5A, Size);
  size_t Grown = 2 * Size + 5000;
  unsigned char *Q = realloc(P, Grown);
  if (!Q) {
    fail("realloc failed", Alignment, Size);
    free(P);
    return;
  }
  for (size_t I = 0; I < Size; ++I)
    if (Q[I] != 0x5A) {
      fail("realloc lost the contents", Alignment, Size);
      break;
    }
  if (_msize(Q) < Grown)
    fail("_msize too small after realloc", Alignment, Size);
  free(Q);
}

static void check(size_t Alignment, size_t Size) {
  SetLastError(12345);
  void *P = aligned_alloc(Alignment, Size);
  if (GetLastError() != 12345)
    fail("last error changed", Alignment, Size);
  use(P, Alignment, Size);
  if (Alignment >= sizeof(void *)) {
    SetLastError(12345);
    if (posix_memalign(&P, Alignment, Size))
      P = NULL;
    if (GetLastError() != 12345)
      fail("last error changed", Alignment, Size);
    use(P, Alignment, Size);
  }
}

int main(void) {
  // Up to 16 bytes, which every heap block has.
  for (size_t A = 1; A <= 16; A *= 2) {
    check(A, 1);
    check(A, 100);
    check(A, 200 * KIB);
  }
  printf("16 bytes: %d failures\n", Failures);

  // Up to 4 KiB: a power-of-two size of the low-fragmentation heap, or 4 KiB
  // pages for a larger size.
  for (size_t A = 32; A <= 4 * KIB; A *= 2) {
    check(A, 1);
    check(A, A - 8);
    check(A, A);
    check(A, 3 * A);
    check(A, 8 * KIB);
    check(A, 8 * KIB + 1);
    check(A, 300 * KIB);
  }
  printf("4 KiB: %d failures\n", Failures);

  // Up to 64 KiB: 64 KiB units.
  for (size_t A = 8 * KIB; A <= 64 * KIB; A *= 2) {
    check(A, 1);
    check(A, 100 * KIB);
    check(A, 600 * KIB);
    check(A, 9 * MIB);
  }
  printf("64 KiB: %d failures\n", Failures);

  // Up to 2 MiB: a reservation of its own.
  for (size_t A = 128 * KIB; A <= 2 * MIB; A *= 2) {
    check(A, 1);
    check(A, 3 * MIB);
    check(A, 20 * MIB);
  }
  printf("2 MiB: %d failures\n", Failures);

  printf("heap valid: %d\n", HeapValidate((HANDLE)_get_heap_handle(), 0, NULL));
  return Failures != 0;
}

// CHECK:      16 bytes: 0 failures
// CHECK-NEXT: 4 KiB: 0 failures
// CHECK-NEXT: 64 KiB: 0 failures
// CHECK-NEXT: 2 MiB: 0 failures
// CHECK-NEXT: heap valid: 1
