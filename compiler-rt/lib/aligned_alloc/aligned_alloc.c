//===-- aligned_alloc.c - aligned_alloc and posix_memalign for Windows ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// C11's aligned_alloc and POSIX's posix_memalign for Windows Itanium, whose
// C library, the Universal CRT, provides neither. Every block is one that
// HeapAlloc returned on the C runtime's own heap, at most shrunk in place,
// which keeps its address. The C runtime's free, realloc and _msize
// therefore take it as they take a block of malloc's.
//
// No heap function takes an alignment, but the size of a request selects the
// allocator that serves it, and some of the segment heap's allocators align
// every block by the layout of their addresses:
//
// - A power-of-two size up to 8 KiB comes from the low-fragmentation heap,
//   which places such blocks at multiples of their size from a page
//   boundary, so a block of at most 4 KiB is aligned to its size.
// - Above 128 KiB, a block is a run of 4 KiB pages.
// - Above 0x7F000 bytes, a block is a run of 64 KiB units.
// - From 16 MiB, a block is a reservation of its own, aligned to 2 MiB.
//
// Each alignment band asks for the smallest such size that covers the
// request, once. The program must therefore run on the segment heap, which
// only its executable's manifest selects; a Windows Itanium executable's
// start-up fails without it. The result is still checked, so that a program
// that links this library by hand without the segment heap gets ENOMEM, not
// a misaligned block, for an extended alignment its heap does not provide.
//
//===----------------------------------------------------------------------===//

#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef _WIN64
#error "aligned_alloc supports 64-bit Windows only"
#endif

#define KIB ((size_t)1024)
#define MIB (1024 * KIB)

// The largest power-of-two size that the low-fragmentation heap serves.
#define MAX_CLASS_SIZE (8 * KIB)
// The first sizes past the variable-size allocator's limit, past the 4 KiB
// page ranges, and past the pool of 1 MiB reservations.
#define MIN_PAGE_RANGE_SIZE (128 * KIB + 1)
#define MIN_UNIT_RANGE_SIZE (0x7F000 + 1)
#define MIN_LARGE_SIZE (16 * MIB)
// Nothing public places a block at a larger alignment.
#define MAX_ALIGNMENT (2 * MIB)

// The low-fragmentation heap takes on a size once it has counted 17 requests
// for it, each of the size's blocks freed before then taking one back; until
// then the size's blocks come from the variable-size allocator, aligned to 16
// bytes only. The first miss in a size holds this many blocks, at most
// 128 KiB, so that the request after them crosses that threshold.
#define WARM_UP_BLOCKS 16

// The sizes, each a power of two, whose warm-up has been claimed.
static volatile LONG WarmedUpSizes;

static int isAligned(const void *P, size_t Alignment) {
  return ((uintptr_t)P & (Alignment - 1)) == 0;
}

static size_t roundUpToPowerOfTwo(size_t Size) {
  --Size;
  Size |= Size >> 1;
  Size |= Size >> 2;
  Size |= Size >> 4;
  Size |= Size >> 8;
  return Size + 1;
}

// Allocates Request bytes and keeps the block if it is aligned, shrunk in
// place to Size.
static void *allocateAndShrink(HANDLE Heap, size_t Request, size_t Size,
                               size_t Alignment) {
  void *P = HeapAlloc(Heap, 0, Request);
  if (!P)
    return NULL;
  if (!isAligned(P, Alignment)) {
    HeapFree(Heap, 0, P);
    return NULL;
  }
  if (Request > Size) {
    // A block that cannot shrink is still a valid block, but the failure
    // sets the last error, which a successful call leaves alone.
    DWORD LastError = GetLastError();
    if (!HeapReAlloc(Heap, HEAP_REALLOC_IN_PLACE_ONLY, P, Size))
      SetLastError(LastError);
  }
  return P;
}

// Holds WARM_UP_BLOCKS blocks of the given size, the misaligned First among
// them, makes one more request, and keeps the first aligned block of all of
// them. A block of the variable-size allocator may be aligned by chance, so
// the blocks are held whether or not they are aligned; freeing one before the
// size is taken on would undo its request.
static void *warmUp(HANDLE Heap, void *First, size_t Size, size_t Alignment) {
  void *Blocks[WARM_UP_BLOCKS + 1];
  unsigned Count = 0;
  Blocks[Count++] = First;
  while (Count <= WARM_UP_BLOCKS && (Blocks[Count] = HeapAlloc(Heap, 0, Size)))
    ++Count;
  void *Kept = NULL;
  for (unsigned I = 0; I < Count; ++I) {
    if (!Kept && isAligned(Blocks[I], Alignment))
      Kept = Blocks[I];
    else
      HeapFree(Heap, 0, Blocks[I]);
  }
  return Kept;
}

static void *allocateAligned(size_t Alignment, size_t Size) {
  HANDLE Heap = (HANDLE)_get_heap_handle();
  // As malloc(0) does, a request for no bytes gets a unique block.
  if (Size == 0)
    Size = 1;
  if (Alignment <= MEMORY_ALLOCATION_ALIGNMENT)
    return HeapAlloc(Heap, 0, Size);
  if (Alignment <= 4 * KIB) {
    size_t ClassSize = Size > Alignment ? Size : Alignment;
    if (ClassSize <= MAX_CLASS_SIZE) {
      ClassSize = roundUpToPowerOfTwo(ClassSize);
      void *P = HeapAlloc(Heap, 0, ClassSize);
      if (!P || isAligned(P, Alignment))
        return P;
      // Only the first miss in a size warms it up; any other goes on to the
      // page ranges.
      LONG Bit = (LONG)ClassSize;
      if (!(WarmedUpSizes & Bit) &&
          !(InterlockedOr(&WarmedUpSizes, Bit) & Bit)) {
        if ((P = warmUp(Heap, P, ClassSize, Alignment)))
          return P;
      } else {
        HeapFree(Heap, 0, P);
      }
    }
    return allocateAndShrink(
        Heap, Size > MIN_PAGE_RANGE_SIZE ? Size : MIN_PAGE_RANGE_SIZE, Size,
        Alignment);
  }
  if (Alignment <= 64 * KIB)
    return allocateAndShrink(
        Heap, Size > MIN_UNIT_RANGE_SIZE ? Size : MIN_UNIT_RANGE_SIZE, Size,
        Alignment);
  return allocateAndShrink(Heap, Size > MIN_LARGE_SIZE ? Size : MIN_LARGE_SIZE,
                           Size, Alignment);
}

void *aligned_alloc(size_t Alignment, size_t Size) {
  if (Alignment == 0 || (Alignment & (Alignment - 1))) {
    errno = EINVAL;
    return NULL;
  }
  void *P =
      Alignment <= MAX_ALIGNMENT ? allocateAligned(Alignment, Size) : NULL;
  if (!P)
    errno = ENOMEM;
  return P;
}

// Returns its error rather than set errno, and leaves *Memory alone on
// failure.
int posix_memalign(void **Memory, size_t Alignment, size_t Size) {
  if (Alignment < sizeof(void *) || (Alignment & (Alignment - 1)))
    return EINVAL;
  if (Alignment > MAX_ALIGNMENT)
    return ENOMEM;
  void *P = allocateAligned(Alignment, Size);
  if (!P)
    return ENOMEM;
  *Memory = P;
  return 0;
}
