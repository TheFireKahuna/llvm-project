//===- alloc/pagemap_classifier.h - Band-level VA classifier ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Band-dispatch over the pagemap, used by three callers: the master VEH
// handler (first-line dispatch before falling through to
// va_tracker::resolve on Empty), is_libc_pointer membership tests, and
// validate_map_fixed_target cordon-overlap rejection.
//
// POSIX-visible VA is NOT indexed in the pagemap. A probe decodes to
// Empty; VEH master and mem_fault_handler treat Empty as "consult
// va_tracker::resolve" and propagate to SEH on tracker miss.
//
// Every entry point is wait-free, non-faulting, allocation-free: one
// ACQUIRE load + cookie XOR + bounds check + one integer-range
// comparison. No syscalls, no Crystalline pins, no skiplist walks —
// safe from VEH masters, debugger probes, and __cxa_finalize teardown.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_CLASSIFIER_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_CLASSIFIER_H

#include "hdr/errno_macros.h"
#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace pagemap {

// Libc-owned body band: Layer-2/3 chunk allocators, sealed Tier-A /
// hardening kinds, Layer-1 va_tracker internals. Excludes Empty (0x00)
// and the cordon band (0x30..0x3F).
inline constexpr uint8_t kLibcBodyBandLo = 0x01;
inline constexpr uint8_t kLibcBodyBandHi = 0x2F;

inline constexpr uint8_t kCordonBandLo = 0x30;
inline constexpr uint8_t kCordonBandHi = 0x3F;

// Facade band: subsystems that own VA outside the chunk allocator's
// descriptor pools.
inline constexpr uint8_t kLibcFacadeBandLo = 0x40;
inline constexpr uint8_t kLibcFacadeBandHi = 0x4F;

// Routing tuple. slot_idx is 0 for cordons (no descriptor pool — tag
// alone routes), per-slab / per-region for Layer-2 / Layer-1 / facade
// kinds. For Empty entries the field is meaningless; callers MUST gate
// on tag first.
struct ChunkClassification {
  VaChunkConsumer tag;
  uint32_t slot_idx;
};

// Wait-free, non-faulting band-dispatch decode. Out-of-bounds decodes
// to {Empty, 0}.
[[nodiscard]] LIBC_INLINE ChunkClassification
classify(const void *addr) noexcept {
  PagemapDecoded d = pagemap_load_decoded(addr);
  return ChunkClassification{d.tag, d.slot_idx};
}

// Predicates below compose classify() with a single numeric range test.
// Ranges are expressed against `static_cast<uint8_t>(d.tag)` so they
// survive non-contiguous enumerator additions and cannot be defeated by
// reordering of `enum class` members.

// True for retired, never-stamped, out-of-bounds, or POSIX-visible
// addresses (POSIX VA is never stamped). On the VEH path Empty causes
// the master handler to fall through to EXCEPTION_CONTINUE_SEARCH.
[[nodiscard]] LIBC_INLINE bool is_empty(const void *addr) noexcept {
  return classify(addr).tag == VaChunkConsumer::Empty;
}

// True iff the chunk is currently tagged in the cordon band. Cordon
// overlap detection is pagemap-only — cordons never enter the va_tracker
// skiplist.
[[nodiscard]] LIBC_INLINE bool is_cordon(const void *addr) noexcept {
  uint8_t t = static_cast<uint8_t>(classify(addr).tag);
  return t >= kCordonBandLo && t <= kCordonBandHi;
}

// Positive allow-list over the libc-stamped bands (body, facade, Misc);
// false for unstamped (possibly POSIX), cordon, out-of-bounds, and any
// other band (0x50..0x5F, 0x60..0x6F reserved-empty, 0x70..0xFE).
// Defence in depth: a wild write that flips an entry into an
// unallocated band cannot fool is_libc_pointer into declaring
// attacker-controlled VA as ours. Adding a new libc band requires
// editing this predicate — intentional reviewer nudge.
[[nodiscard]] LIBC_INLINE bool is_libc_owned(const void *addr) noexcept {
  uint8_t t = static_cast<uint8_t>(classify(addr).tag);
  if (t >= kLibcBodyBandLo && t <= kLibcBodyBandHi)
    return true;
  if (t >= kLibcFacadeBandLo && t <= kLibcFacadeBandHi)
    return true;
  if (t == static_cast<uint8_t>(VaChunkConsumer::Misc))
    return true;
  return false;
}

//===----------------------------------------------------------------------===//
// MAP_FIXED / MREMAP_FIXED pre-validation
//===----------------------------------------------------------------------===//

// Single-callsite anti-data-loss gate for destructive placement
// (MAP_FIXED, MAP_FIXED_NOREPLACE, MREMAP_FIXED). Per-chunk decisions:
//
//   * Empty (POSIX-visible VA, MEM_FREE, out-of-bounds) → 0. Substrate
//     typed-op envelope handles the rest (split/replace for an existing
//     POSIX desc; gap-fill placeholder reserve for MEM_FREE).
//   * Image / Kernel cordon → EINVAL. Caller cannot meaningfully
//     replace PE images or kernel-loaned regions.
//   * Foreign / ForeignStale cordon → ENOMEM. Third-party VirtualAllocEx
//     or otherwise-unowned VA: POSIX's "cannot allocate" errno is
//     ENOMEM, and EINVAL would lose the "address space contended by
//     foreign tenant" signal apps rely on for retry-with-NULL fallback.
//   * Libc body / facade / Misc → EINVAL. Overwriting corrupts runtime.
//
// Walk strides 64 KiB aligned to the chunk grid so every overlapping
// chunk is probed exactly once even with sub-chunk-aligned addr.
[[nodiscard]] LIBC_INLINE int validate_map_fixed_target(void *addr,
                                                        size_t size) noexcept {
  if (addr == nullptr || size == 0)
    return 0;

  const uintptr_t end = reinterpret_cast<uintptr_t>(addr) + size;
  uintptr_t chunk =
      reinterpret_cast<uintptr_t>(addr) & ~(kPagemapChunkBytes - 1);
  while (chunk < end) {
    const VaChunkConsumer t =
        classify(reinterpret_cast<void *>(chunk)).tag;
    if (t != VaChunkConsumer::Empty) {
      if (t == VaChunkConsumer::Foreign ||
          t == VaChunkConsumer::ForeignStale)
        return ENOMEM;
      return EINVAL;
    }
    chunk += kPagemapChunkBytes;
  }
  return 0;
}

} // namespace pagemap
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
