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

// Observe the heap selected before image initialization. DLLs see their host's
// heap. Callers needing ownership must compare this observation with UCRT's
// retained handle; the PEB alone does not establish allocator ownership.
[[nodiscard]] inline void *processHeap() noexcept {
#if defined(__x86_64__)
  const auto *PEB =
      reinterpret_cast<const unsigned char *>(__readgsqword(0x60));
#elif defined(__aarch64__)
  const auto *TEB = reinterpret_cast<const unsigned char *>(__getReg(18));
  const unsigned char *PEB;
  __builtin_memcpy(&PEB, TEB + 0x60, sizeof(PEB));
#else
#error "wincrt supports x86_64 and aarch64"
#endif
  void *Heap;
  __builtin_memcpy(&Heap, PEB + 0x30, sizeof(Heap));
  return Heap;
}

// Requires a valid heap header. This is RTL's family discriminator; it neither
// authenticates the pointer nor qualifies a private metadata layout.
[[nodiscard]] inline bool isSegmentHeap(const void *Heap) noexcept {
  uint32_t Signature;
  __builtin_memcpy(&Signature, static_cast<const unsigned char *>(Heap) + 0x10,
                   sizeof(Signature));
  return Signature == 0xDDEEDDEE;
}

} // namespace wincrt

#endif
