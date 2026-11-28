//===-- aligned_alloc.cpp - aligned_alloc and posix_memalign --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// C11's aligned_alloc and POSIX's posix_memalign, which the Universal CRT
// lacks. Every block is one that HeapAlloc returned on the C runtime's own
// heap, at most shrunk in place, which keeps its address, so the C runtime's
// free, realloc and _msize take it as they take a block of malloc's.
//
// No heap function takes an alignment, but the size of a request selects the
// allocator that serves it, and the segment heap's allocators align every
// block by the layout of their addresses:
//
// - A power-of-two size up to 8 KiB comes from the low-fragmentation heap,
//   which places such blocks at multiples of their size from a page
//   boundary, so a block of at most 4 KiB is aligned to its size.
// - Above 128 KiB, a block is a run of 4 KiB pages.
// - Above 0x7F000 bytes, a block is a run of 64 KiB units.
// - From 16 MiB, a block is a reservation of its own, aligned to 2 MiB.
//
// Each alignment asks, once, for the smallest such size that covers the
// request. The process must therefore run on the segment heap, which a
// Windows Itanium executable's start-up requires. The result is still
// checked, so that on any other heap, under another executable, an extended
// alignment fails with ENOMEM and never returns a misaligned block.
//
//===----------------------------------------------------------------------===//

#include "wincrt.h"

#include <errno.h>
#include <malloc.h>

namespace {

constexpr size_t KiB = 1024, MiB = 1024 * KiB;
// Nothing public places a block at a larger alignment.
constexpr size_t MaxAlignment = 2 * MiB;

bool isAligned(const void *P, size_t Alignment) {
  return !(reinterpret_cast<uintptr_t>(P) & (Alignment - 1));
}

// The low-fragmentation heap takes on a size once it has counted 17 requests
// for it, a block freed before then taking one back; until then the size's
// blocks come from the variable-size allocator, aligned to 16 bytes only. The
// first miss in a size therefore holds its block and 15 more, at most 128 KiB,
// makes the request that crosses the threshold, and keeps the first aligned
// block of them all. This is the set of sizes, each a power of two, whose
// warm-up has been claimed.
LONG WarmedUpSizes;

void *warmUp(HANDLE Heap, void *First, size_t Size, size_t Alignment) {
  void *Blocks[17] = {First};
  for (unsigned I = 1; I != 17 && (Blocks[I] = HeapAlloc(Heap, 0, Size)); ++I)
    ;
  void *Kept = nullptr;
  for (void *Block : Blocks) {
    if (!Kept && Block && isAligned(Block, Alignment))
      Kept = Block;
    else
      HeapFree(Heap, 0, Block);
  }
  return Kept;
}

void *allocate(size_t Alignment, size_t Size) {
  if (Alignment > MaxAlignment)
    return nullptr;
  auto Heap = reinterpret_cast<HANDLE>(_get_heap_handle());
  // As malloc(0) does, a request for no bytes gets a unique block.
  Size += !Size;
  if (Alignment <= MEMORY_ALLOCATION_ALIGNMENT)
    return HeapAlloc(Heap, 0, Size);
  size_t Class = Size > Alignment ? Size : Alignment;
  if (Alignment <= 4 * KiB && Class <= 8 * KiB) {
    Class = size_t(1) << (64 - __builtin_clzll(Class - 1));
    void *P = HeapAlloc(Heap, 0, Class);
    if (!P || isAligned(P, Alignment))
      return P;
    // Only the first miss in a size warms it up; any other goes on to the
    // pages.
    if (__atomic_fetch_or(&WarmedUpSizes, Class, __ATOMIC_RELAXED) & Class)
      HeapFree(Heap, 0, P);
    else if ((P = warmUp(Heap, P, Class, Alignment)))
      return P;
  }
  // The least size that the allocator with the alignment serves.
  size_t Request = Alignment <= 4 * KiB    ? 128 * KiB + 1
                   : Alignment <= 64 * KiB ? 0x7F000 + 1
                                           : 16 * MiB;
  if (Request < Size)
    Request = Size;
  void *P = HeapAlloc(Heap, 0, Request);
  if (P && !isAligned(P, Alignment)) {
    HeapFree(Heap, 0, P);
    return nullptr;
  }
  // A block that cannot shrink is still a valid block, but the failure sets
  // the last error, which a successful call leaves alone.
  if (P && Request > Size) {
    DWORD LastError = GetLastError();
    if (!HeapReAlloc(Heap, HEAP_REALLOC_IN_PLACE_ONLY, P, Size))
      SetLastError(LastError);
  }
  return P;
}

bool isPowerOfTwo(size_t Alignment) {
  return Alignment && !(Alignment & (Alignment - 1));
}

} // namespace

extern "C" {

void *__cdecl aligned_alloc(size_t Alignment, size_t Size) {
  if (!isPowerOfTwo(Alignment)) {
    errno = EINVAL;
    return nullptr;
  }
  void *P = allocate(Alignment, Size);
  if (!P)
    errno = ENOMEM;
  return P;
}

// Returns its error, and leaves errno, and on failure *Memory, alone.
int __cdecl posix_memalign(void **Memory, size_t Alignment, size_t Size) {
  if (Alignment < sizeof(void *) || !isPowerOfTwo(Alignment))
    return EINVAL;
  void *P = allocate(Alignment, Size);
  if (!P)
    return ENOMEM;
  *Memory = P;
  return 0;
}

} // extern "C"
