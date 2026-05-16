//===- alloc/pagemap_cordon.h - Cordon stamp / retire / toggle --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cordon stamp surface.
//
// A cordon is a 64 KiB-aligned VA range that is observable to user code but
// is NOT a POSIX mapping: PE images, kernel-loaned regions (PEB / TEB /
// KUSER_SHARED_DATA / ALPC), or VA created outside libc by an embedded host
// or injected DLL. Cordons are stamped into the pagemap so the cordon-band
// predicates in pagemap_classifier.h (read by the VEH master,
// validate_map_fixed_target, is_libc_pointer) reject them in O(1) per chunk.
// POSIX-visible mappings carry NO pagemap entry — they live in va_tracker
// and the classifier reaches them via the Empty fallthrough; the two
// indexes are disjoint so a POSIX mmap never pays a per-64-KiB publish.
//
// Cordon entries reuse the (slot_idx, tag) encoding from pagemap.h with
// slot_idx pinned at zero — cordons have no descriptor pool; the tag
// alone routes diagnostic dispatch. The wait-free, non-faulting reader
// contract from pagemap.h applies unchanged.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_CORDON_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_CORDON_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {
namespace pagemap {

// Caller-facing taxonomy of non-POSIX VA the libc classifies. ForeignStale
// is intentionally absent — it is a state reachable only via the toggle
// entry points, never via stamp_cordon directly.
enum class CordonKind : uint8_t {
  Image = 0,   // Executable / DLL image ranges.
  Kernel = 1,  // Kernel-loaned regions (PEB, TEB, KUSER_SHARED_DATA, ALPC).
  Foreign = 2, // NtPssCaptureVaSpaceBulk captures outside libc / image / kernel.
};

// Stamp every 64 KiB chunk in [base, base + bytes) with the cordon tag for
// `kind`. Idempotent: same-kind re-stamp is a no-op, different-kind is a
// clean rewrite (single RELEASE store per chunk). base must be
// kPagemapChunkBytes-aligned; bytes must be a positive multiple thereof.
// Returns 0, -EINVAL (alignment / zero-length — no state observable), or
// -ENOMEM (pagemap_register_range could not upgrade backing OS pages
// RO -> RW; runs before publish, so no partial stamp). Single-publisher
// per chunk; callers MUST NOT race two stamp_cordon calls on overlapping
// ranges. Readers are wait-free.
[[nodiscard]] int stamp_cordon(void *base, size_t bytes, CordonKind kind);

// Symmetric inverse of stamp_cordon — zeroes every covered entry when a
// cordoned range disappears (DLL unloaded, foreign region torn down).
// Storing literal zero matches the shared-zero state of an untouched
// pagemap OS page (see pagemap.h retire protocol). Preconditions match
// stamp_cordon; on violation: silent no-op — teardown paths are fail-soft
// and surfacing an error would force callers to invent recovery for state
// that is already correct.
void retire_cordon(void *base, size_t bytes);

// Toggle Foreign -> ForeignStale across [base, base + bytes). Walks
// chunk-by-chunk; for each chunk currently decoded as Foreign, CASes the
// encoded word to (slot_idx=0, ForeignStale). A lost CAS reloads and
// retries once; a second loss leaves the chunk alone (concurrent writer
// in the band — not our place to fight). Chunks outside the foreign band
// are left untouched. Best-effort and idempotent. Precondition violation:
// silent no-op (fail-soft).
void set_cordon_stale(void *base, size_t bytes);

// Toggle ForeignStale -> Foreign; mirror of set_cordon_stale. Called when
// a previously-stale foreign mapping is observed restored.
void clear_cordon_stale(void *base, size_t bytes);

// The general is_cordon(addr) predicate lives in pagemap_classifier.h —
// it ranges against kCordonBandLo..kCordonBandHi covering every CordonKind
// plus ForeignStale. Only the stale-specific predicate is here because it
// pins a VaChunkConsumer enumerator the classifier doesn't expose as a
// constant. Same wait-free non-faulting envelope: safe from VEH / SIGSEGV
// / __cxa_finalize.
[[nodiscard]] LIBC_INLINE bool is_cordon_stale(const void *addr) noexcept {
  PagemapDecoded d = pagemap_load_decoded(addr);
  return d.tag == VaChunkConsumer::ForeignStale;
}

} // namespace pagemap
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
