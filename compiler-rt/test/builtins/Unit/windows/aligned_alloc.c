// RUN: %clang %s -o %t.exe
// RUN: %run %t.exe bands | FileCheck %s --check-prefix=BANDS
// RUN: %run %t.exe warm-up | FileCheck %s --check-prefix=WARM-UP
// RUN: %run %t.exe errno | FileCheck %s --check-prefix=ERRNO
// RUN: %run %t.exe threads | FileCheck %s --check-prefix=THREADS

// Each mode runs in a process of its own, since the heap's warm-up is
// process-wide, and prints each failure and then its count.
// [BANDS] On the segment heap, every alignment up to 2 MiB gets an aligned
// block, of any size, from both functions, and leaves the last error alone;
// free, realloc and _msize take each block as one of malloc's. A request for
// no bytes gets a unique block, as malloc(0) does.
// [WARM-UP] An alignment up to 4 KiB with a small size is served by the
// low-fragmentation heap's class for the next power of two, which takes on a
// size only after some requests for it: the first miss in a size warms it up,
// and every request in it then gets a class block, even one that only
// aligned_alloc uses.
// [ERRNO] aligned_alloc sets errno to EINVAL for an alignment that is not a
// power of two, and to ENOMEM for one above 2 MiB or a size the heap refuses.
// posix_memalign returns those errors instead, for an alignment that is also
// not a multiple of sizeof(void *), and changes neither errno nor the pointer
// when it fails. Neither changes errno when it succeeds.
// [THREADS] Threads that race on the first misses of the same classes all get
// aligned blocks, a call made while the caller holds the heap's lock
// succeeds, and the heap stays valid.

// BANDS:   bands: 0 failures, heap valid 1
// WARM-UP: warm-up: 0 failures
// ERRNO:   errno: 0 failures
// THREADS: threads: 0 failures, heap valid 1

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define KIB ((size_t)1024)
#define MIB (1024 * KIB)
#define UNCHANGED 12345

static volatile LONG Failures;

static void fail(const char *What, size_t Alignment, size_t Size) {
  printf("%s: alignment %zu, size %zu\n", What, Alignment, Size);
  InterlockedIncrement(&Failures);
}

static void use(unsigned char *P, size_t Alignment, size_t Size) {
  size_t Grown = 2 * Size + 5000, I = 0;
  if (!P || (uintptr_t)P % Alignment || _msize(P) < Size) {
    fail("no aligned block of the size", Alignment, Size);
    return;
  }
  memset(P, 0x5A, Size);
  unsigned char *Q = realloc(P, Grown);
  while (Q && I < Size && Q[I] == 0x5A)
    ++I;
  if (!Q || I != Size || _msize(Q) < Grown)
    fail("realloc lost the block", Alignment, Size);
  free(Q ? Q : P);
}

static void check(size_t Alignment, size_t Size) {
  void *P, *Q;
  SetLastError(UNCHANGED);
  P = aligned_alloc(Alignment, Size);
  if (Alignment < sizeof(void *))
    Q = aligned_alloc(Alignment, Size);
  else if (posix_memalign(&Q, Alignment, Size))
    Q = NULL;
  if (GetLastError() != UNCHANGED)
    fail("last error changed", Alignment, Size);
  if (!Size && (P == Q || !_msize(P)))
    fail("not a unique block", Alignment, Size);
  use(P, Alignment, Size);
  use(Q, Alignment, Size);
}

#define SIZES(Alignment, ...)                                                   \
  for (size_t I = 0, S[] = {__VA_ARGS__}; I < sizeof(S) / sizeof(*S); ++I)     \
  check(Alignment, S[I])

static void bands(void) {
  // Up to 16 bytes, which every heap block has.
  for (size_t A = 1; A <= 16; A *= 2)
    SIZES(A, 0, 1, 100, 200 * KIB);
  // Up to 4 KiB: a power-of-two size of the low-fragmentation heap, or 4 KiB
  // pages for a larger size.
  for (size_t A = 32; A <= 4 * KIB; A *= 2)
    SIZES(A, 0, 1, A - 8, A, 3 * A, 8 * KIB, 8 * KIB + 1, 300 * KIB);
  // Up to 64 KiB: 64 KiB units.
  for (size_t A = 8 * KIB; A <= 64 * KIB; A *= 2)
    SIZES(A, 0, 1, 100 * KIB, 600 * KIB, 9 * MIB);
  // Up to 2 MiB: a reservation of its own.
  for (size_t A = 128 * KIB; A <= 2 * MIB; A *= 2)
    SIZES(A, 0, 1, 3 * MIB, 20 * MIB);
}

// A request the class did not serve would come from 4 KiB pages, shrunk to
// the request.
static void warmUp(size_t Alignment, size_t Size, size_t ClassSize) {
  static void *Blocks[400];
  for (int I = 0; I < 400; ++I) {
    Blocks[I] = aligned_alloc(Alignment, Size);
    if (!Blocks[I] || (uintptr_t)Blocks[I] % Alignment || _msize(Blocks[I]) != ClassSize)
      fail("not a class block", Alignment, Size);
  }
  for (int I = 0; I < 400; ++I)
    free(Blocks[I]);
}

static void alignedAlloc(size_t Alignment, size_t Size, int Error) {
  errno = UNCHANGED;
  void *P = aligned_alloc(Alignment, Size);
  if (Error ? P || errno != Error : !P || errno != UNCHANGED)
    fail("aligned_alloc", Alignment, Size);
  free(P);
}

static void posixMemalign(size_t Alignment, size_t Size, int Error) {
  void *P = &P;
  errno = UNCHANGED;
  if (posix_memalign(&P, Alignment, Size) != Error || errno != UNCHANGED ||
      (P == &P) != (Error != 0))
    fail("posix_memalign", Alignment, Size);
  if (!Error)
    free(P);
}

static HANDLE Start;

static DWORD WINAPI allocate(LPVOID Unused) {
  static const size_t Classes[][2] = {{64, 40}, {256, 200}, {1024, 1000}, {4096, 5000}};
  void *Blocks[100];
  WaitForSingleObject(Start, INFINITE);
  for (int Round = 0; Round < 50; ++Round)
    for (int C = 0; C < 4; ++C) {
      for (int I = 0; I < 100; ++I)
        if (!(Blocks[I] = aligned_alloc(Classes[C][0], Classes[C][1])) ||
            (uintptr_t)Blocks[I] % Classes[C][0])
          fail("misaligned", Classes[C][0], Classes[C][1]);
      for (int I = 0; I < 100; ++I)
        free(Blocks[I]);
    }
  return 0;
}

int main(int argc, char **argv) {
  const char *Mode = argv[1];
  HANDLE Heap = (HANDLE)_get_heap_handle();
  if (!strcmp(Mode, "bands"))
    bands();
  if (!strcmp(Mode, "warm-up")) {
    warmUp(64, 40, 64);
    warmUp(256, 200, 256);
    warmUp(1024, 1000, 1024);
    warmUp(2048, 3000, 4096);
    warmUp(4096, 5000, 8192);
  }
  if (!strcmp(Mode, "errno")) {
    alignedAlloc(0, 16, EINVAL);
    alignedAlloc(24, 16, EINVAL);
    alignedAlloc(4 * MIB, 16, ENOMEM);
    alignedAlloc(1, 16, 0);
    alignedAlloc(16, SIZE_MAX, ENOMEM);
    alignedAlloc(64, SIZE_MAX, ENOMEM);
    alignedAlloc(2 * MIB, SIZE_MAX, ENOMEM);
    posixMemalign(0, 16, EINVAL);
    posixMemalign(4, 16, EINVAL);
    posixMemalign(24, 16, EINVAL);
    posixMemalign(4 * MIB, 16, ENOMEM);
    posixMemalign(8, 16, 0);
    posixMemalign(16, SIZE_MAX, ENOMEM);
    posixMemalign(64, SIZE_MAX, ENOMEM);
  }
  if (!strcmp(Mode, "threads")) {
    HANDLE Threads[8];
    Start = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (int I = 0; I < 8; ++I)
      Threads[I] = CreateThread(NULL, 0, allocate, NULL, 0, NULL);
    SetEvent(Start);
    WaitForMultipleObjects(8, Threads, TRUE, INFINITE);
    HeapLock(Heap);
    void *P = aligned_alloc(65536, 1000);
    HeapUnlock(Heap);
    if (!P)
      fail("no block under the heap lock", 65536, 1000);
    free(P);
  }
  printf("%s: %ld failures", Mode, Failures);
  if (!strcmp(Mode, "bands") || !strcmp(Mode, "threads"))
    printf(", heap valid %d", HeapValidate(Heap, 0, NULL));
  printf("\n");
  return 0;
}
