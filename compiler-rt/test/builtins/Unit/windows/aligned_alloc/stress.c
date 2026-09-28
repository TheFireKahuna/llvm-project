// RUN: %clang_wincrt %s -o %t.exe
// RUN: %run %t.exe | FileCheck %s

// Threads allocate at random alignments up to 2 MiB and random sizes, while
// they also reallocate and free those blocks and churn blocks of malloc's,
// and another thread walks the heap under its lock. Every block made is
// aligned and keeps its contents through realloc, and the heap stays valid.

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define THREADS 8
#define OPERATIONS 20000
#define SLOTS 512
#define NOISE 256

typedef struct {
  unsigned char *P;
  size_t Size;
  unsigned char Tag;
} Slot;

static volatile LONG Failures, Stop;

static unsigned long long next(unsigned long long *State) {
  *State ^= *State << 13;
  *State ^= *State >> 7;
  *State ^= *State << 17;
  return *State;
}

// Mostly small sizes, some up to 1 MiB and a few up to 8 MiB, and some zero.
static size_t pickSize(unsigned long long *State) {
  unsigned R = next(State) % 100;
  if (R < 3)
    return 0;
  unsigned Log = R < 60   ? next(State) % 13
                 : R < 95 ? 12 + next(State) % 8
                          : 20 + next(State) % 3;
  size_t Size = (size_t)1 << Log;
  return Size + next(State) % Size;
}

static void fill(Slot *S) {
  size_t N = S->Size < 64 ? S->Size : 64;
  for (size_t I = 0; I < N; ++I)
    S->P[I] = (unsigned char)(S->Tag + I);
}

static void verify(const Slot *S, size_t Size) {
  size_t N = Size < 64 ? Size : 64;
  for (size_t I = 0; I < N; ++I)
    if (S->P[I] != (unsigned char)(S->Tag + I)) {
      InterlockedIncrement(&Failures);
      return;
    }
}

static DWORD WINAPI allocate(void *Seed) {
  unsigned long long State = 0x9E3779B97F4A7C15ull * ((uintptr_t)Seed + 1);
  Slot *Slots = calloc(SLOTS, sizeof(Slot));
  void *Noise[NOISE] = {0};
  for (int Op = 0; Op < OPERATIONS; ++Op) {
    Slot *S = &Slots[next(&State) % SLOTS];
    unsigned R = next(&State) % 100;
    if (R < 10) {
      unsigned K = next(&State) % NOISE;
      free(Noise[K]);
      Noise[K] = malloc(next(&State) % 3000 + 1);
      continue;
    }
    if (S->P) {
      verify(S, S->Size);
      if (_msize(S->P) < S->Size)
        InterlockedIncrement(&Failures);
      if (R < 70) {
        free(S->P);
        S->P = NULL;
        continue;
      }
      size_t Size = pickSize(&State) + 1;
      unsigned char *Q = realloc(S->P, Size);
      if (!Q)
        continue;
      S->P = Q;
      verify(S, S->Size < Size ? S->Size : Size);
      S->Size = Size;
      fill(S);
      continue;
    }
    size_t Alignment = (size_t)1 << (next(&State) % 22);
    size_t Size = pickSize(&State);
    void *P = NULL;
    if (R & 1) {
      P = aligned_alloc(Alignment, Size);
    } else {
      if (Alignment < sizeof(void *))
        Alignment = sizeof(void *);
      if (posix_memalign(&P, Alignment, Size))
        P = NULL;
    }
    if (!P)
      continue;
    if ((uintptr_t)P % Alignment)
      InterlockedIncrement(&Failures);
    S->P = P;
    S->Size = Size ? Size : 1;
    S->Tag = (unsigned char)next(&State);
    fill(S);
  }
  for (int I = 0; I < SLOTS; ++I) {
    if (Slots[I].P)
      verify(&Slots[I], Slots[I].Size);
    free(Slots[I].P);
  }
  for (int K = 0; K < NOISE; ++K)
    free(Noise[K]);
  free(Slots);
  return 0;
}

static DWORD WINAPI walk(void *Heap) {
  while (!Stop) {
    if (!HeapLock(Heap))
      continue;
    PROCESS_HEAP_ENTRY Entry;
    Entry.lpData = NULL;
    while (HeapWalk(Heap, &Entry))
      ;
    HeapUnlock(Heap);
    Sleep(1);
  }
  return 0;
}

int main(void) {
  HANDLE Heap = (HANDLE)_get_heap_handle();
  HANDLE Threads[THREADS];
  HANDLE Walker = CreateThread(NULL, 0, walk, Heap, 0, NULL);
  for (int I = 0; I < THREADS; ++I)
    Threads[I] = CreateThread(NULL, 0, allocate, (void *)(uintptr_t)I, 0, NULL);
  WaitForMultipleObjects(THREADS, Threads, TRUE, INFINITE);
  Stop = 1;
  WaitForSingleObject(Walker, INFINITE);

  // A call made while the caller holds the heap's lock succeeds.
  HeapLock(Heap);
  void *P = aligned_alloc(65536, 1000);
  HeapUnlock(Heap);
  free(P);

  printf("failures: %ld\n", Failures);
  printf("heap valid: %d\n", HeapValidate(Heap, 0, NULL));
  return 0;
}

// CHECK:      failures: 0
// CHECK-NEXT: heap valid: 1
