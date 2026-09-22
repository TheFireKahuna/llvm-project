//===-- heap.h - Process heap identity ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef COMPILER_RT_LIB_WINCRT_HEAP_H
#define COMPILER_RT_LIB_WINCRT_HEAP_H

#include <intrin.h>
#include <stdint.h>

namespace wincrt {

// Windows creates this heap before running image initializers. The shared UCRT
// retains this same handle; a DLL observes its host's heap, not its own
// manifest. Use byte copies for private OS storage, without imposing C++ object
// types on it. Clang folds each copy into a single load.
inline void *processHeap() {
  const unsigned char *Peb;
#if defined(__x86_64__)
  Peb = reinterpret_cast<const unsigned char *>(__readgsqword(0x60));
#elif defined(__aarch64__)
  const auto *Teb = reinterpret_cast<const unsigned char *>(__getReg(18));
  __builtin_memcpy(&Peb, Teb + 0x60, sizeof(Peb));
#else
#error "wincrt supports x86_64 and aarch64"
#endif
  void *Heap;
  __builtin_memcpy(&Heap, Peb + 0x30, sizeof(Heap));
  return Heap;
}

// This is the native family discriminator used by RtlAllocateHeap, not a
// qualification of private allocation routines or their metadata layouts.
inline bool isSegmentHeap(const void *Heap) {
  uint32_t Signature;
  __builtin_memcpy(&Signature, static_cast<const unsigned char *>(Heap) + 0x10,
                   sizeof(Signature));
  return Signature == 0xDDEEDDEE;
}

} // namespace wincrt

#endif
