//===-- Direct TEB TLS slot access for Windows --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single-instruction TLS access and lock-free slot allocation.
//
// The TEB contains 64 inline TLS slots at a fixed offset (TlsSlots array):
//   x64:     gs:0xE10  + index*8
//   AArch64: x18+0x1480 + index*8
//
// Slot allocation operates directly on the PEB TlsBitmap via atomic CAS —
// no TlsAlloc, no ntdll calls, no kernel32 dependency. This is safe because
// the bitmap is a flat uint64_t and we only ever set bits during alloc.
//
// Thread-exit cleanup is handled by a .CRT$XL TLS callback registered by
// the caller — not by FLS callbacks. This eliminates the entire FLS
// subsystem (RtlFlsAlloc, FlsData, bucket radix tree, global thread list).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_TLS_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_TLS_H

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/ntdll.h"
#include "src/__support/common.h"
#include "src/__support/macros/config.h"
#include "src/__support/threads/windows/spin_wait.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// TEB.TlsSlots offsets (64 PVOID slots):
//   x64:     gs:0xE10
//   AArch64: x18+0x1480

inline constexpr DWORD TLS_OUT_OF_INDEXES = 0xFFFFFFFF;
inline constexpr unsigned TLS_INLINE_SLOT_COUNT = 64;

/// Read a TLS slot directly from the TEB. Index must be < 64.
LIBC_INLINE void *teb_tls_get(unsigned index) {
#if defined(__x86_64__)
  void *val;
  __asm__ __volatile__("movq %%gs:0xE10(,%1,8), %0"
                       : "=r"(val)
                       : "r"(static_cast<uint64_t>(index)));
  return val;
#elif defined(__aarch64__)
  void *val;
  __asm__ __volatile__("ldr %0, [x18, %1]"
                       : "=r"(val)
                       : "r"(0x1480 + static_cast<uint64_t>(index) * 8));
  return val;
#else
#error "teb_tls: unsupported architecture"
#endif
}

/// Write a TLS slot directly to the TEB. Index must be < 64.
LIBC_INLINE void teb_tls_set(unsigned index, void *val) {
#if defined(__x86_64__)
  __asm__ __volatile__("movq %0, %%gs:0xE10(,%1,8)"
                       :
                       : "r"(val), "r"(static_cast<uint64_t>(index))
                       : "memory");
#elif defined(__aarch64__)
  __asm__ __volatile__("str %0, [x18, %1]"
                       :
                       : "r"(val), "r"(0x1480 + static_cast<uint64_t>(index) * 8)
                       : "memory");
#else
#error "teb_tls: unsupported architecture"
#endif
}

/// Allocate an inline TLS slot via lock-free CAS on the PEB TlsBitmap.
/// Returns the slot index (0-63), or TLS_OUT_OF_INDEXES on failure.
/// No TlsAlloc, no ntdll, no kernel32 — one atomic cmpxchg on the PEB.
LIBC_INLINE DWORD tls_alloc() {
  PEB *peb = NtCurrentPeb();
  RTL_BITMAP *bmp = peb->TlsBitmap;
  auto *bits = reinterpret_cast<cpp::Atomic<uint64_t> *>(bmp->Buffer);

  uint64_t old = bits->load(cpp::MemoryOrder::RELAXED);
  while (old != ~uint64_t{0}) {
    uint32_t idx = static_cast<uint32_t>(__builtin_ctzll(~old));
    uint64_t want = old | (uint64_t{1} << idx);
    if (bits->compare_exchange_weak(old, want,
                                    cpp::MemoryOrder::ACQ_REL,
                                    cpp::MemoryOrder::RELAXED))
      return idx;
    // CAS failed — 'old' is updated, retry.
    spin_wait::relax_processor();
  }
  return TLS_OUT_OF_INDEXES;
}

/// Free an inline TLS slot. Clears the bit in the PEB TlsBitmap.
LIBC_INLINE void tls_free(DWORD index) {
  if (index >= TLS_INLINE_SLOT_COUNT)
    return;
  PEB *peb = NtCurrentPeb();
  RTL_BITMAP *bmp = peb->TlsBitmap;
  auto *bits = reinterpret_cast<cpp::Atomic<uint64_t> *>(bmp->Buffer);

  uint64_t old = bits->load(cpp::MemoryOrder::RELAXED);
  uint64_t mask = ~(uint64_t{1} << index);
  while (!bits->compare_exchange_weak(old, old & mask,
                                      cpp::MemoryOrder::ACQ_REL,
                                      cpp::MemoryOrder::RELAXED))
    spin_wait::relax_processor();
}

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_TEB_TLS_H
