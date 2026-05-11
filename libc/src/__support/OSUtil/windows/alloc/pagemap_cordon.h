//===- alloc/pagemap_cordon.h - Cordon stamp / retire / toggle --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cordon stamp surface for the NTPOSIX memory architecture.
///
/// A cordon is a 64 KiB-aligned VA range that is NOT a POSIX-visible
/// mapping but is observable to user code: an executable / DLL image
/// range, a kernel-loaned region (PEB / TEB / KUSER_SHARED_DATA / ALPC
/// shared sections), or a foreign mapping created outside libc by an
/// embedded host or injected DLL. Cordons are stamped into the pagemap
/// so the libc-internal-VA classifier (\c pagemap::classify, invoked
/// by the VEH master, \c validate_map_fixed_target, \c is_libc_pointer)
/// can reject them in O(1) per chunk without consulting any other
/// index. POSIX-visible mappings live in `memory/va_tracker.h` (ART +
/// interval skiplist) and are reached via the classifier's fallthrough
/// on an \c Empty pagemap decode; the two indexes are disjoint.
///
/// Cordons do NOT enter the va_tracker skiplist. The pagemap is the
/// index for cordon membership; this header is the stamp surface
/// that the kernel/image-discovery code and foreign-region capture code
/// route through.
///
/// Cordon entries carry the same encoded \c (slot_idx, tag) shape as
/// every other pagemap entry, but \c slot_idx is always zero — cordons
/// have no per-entry descriptor pool. The wait-free, non-faulting reader
/// contract from \c pagemap.h applies unchanged: \c is_cordon(addr) is
/// one ACQUIRE load + cookie XOR + numeric range check, safe from any
/// context including SIGSEGV / VEH / \c __cxa_finalize.
///
/// Tag lifecycle:
///
///   * \c stamp_cordon — install Image / Kernel / Foreign on a fresh or
///     re-stamped range. Idempotent.
///
///   * \c retire_cordon — SYMMETRIC inverse of \c stamp_cordon. Zeroes
///     every covered entry. Used when the cordoned VA range disappears
///     entirely (DLL fully unloaded, foreign region fully torn down).
///     Post-retire the range decodes to \c Empty.
///
///   * \c set_cordon_stale / \c clear_cordon_stale — flip Foreign
///     to/from ForeignStale in place. The lightweight signal that a
///     foreign mapping was observed torn down (\c set_cordon_stale) or
///     restored (\c clear_cordon_stale) by a periodic
///     \c NtPssCaptureVaSpaceBulk sweep. The cordon stays in the
///     pagemap; only the routing tag changes. Both calls walk
///     chunk-by-chunk and skip any chunk whose current tag isn't the
///     expected source tag — best-effort, idempotent under concurrent
///     retire / stamp / opposite toggle.
///
/// Idempotency summary:
///
///   * \c stamp_cordon — same kind is a no-op; different kind is a
///     clean rewrite.
///   * \c retire_cordon — zeroing already-zero entries is harmless.
///   * \c set_cordon_stale / \c clear_cordon_stale — single CAS with
///     one retry; second loss leaves the chunk alone (some other writer
///     published over us).
///
/// Hard caller invariants for every mutation entry point:
///
///   * \c base is \c kPagemapChunkBytes (64 KiB) aligned.
///   * \c bytes is a positive multiple of \c kPagemapChunkBytes.
///
/// \c stamp_cordon validates and returns \c -EINVAL on violation. The
/// teardown / toggle entry points (\c retire_cordon, \c set_cordon_stale,
/// \c clear_cordon_stale) silently no-op: they are fail-soft cleanup
/// hooks, and surfacing an error from a teardown path forces callers to
/// invent error-recovery for a state that's already correct.
///
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

//===----------------------------------------------------------------------===//
// CordonKind
//===----------------------------------------------------------------------===//

/// Caller-facing taxonomy of non-POSIX VA the libc classifies.
///
///   * \c Image   — executable / DLL image ranges (PE headers, .text,
///                  .rdata, .data, .pdata). Stamped by module discovery
///                  and by the DLL load/unload notification callback.
///   * \c Kernel  — kernel-loaned shared regions (KUSER_SHARED_DATA,
///                  PEB, TEB, ALPC shared sections, Win32 client shared
///                  section, heap, ApiSet schema). Stamped at Tier A
///                  by kernel-region discovery.
///   * \c Foreign — VA captured via \c NtPssCaptureVaSpaceBulk that
///                  belongs neither to libc nor to a known
///                  image/kernel range. Stamped by the foreign-region
///                  capture path.
///
/// \c VaChunkConsumer::ForeignStale is NOT a \c CordonKind — it is a
/// STATE reachable only via \c set_cordon_stale / \c clear_cordon_stale.
/// The public stamp surface never produces \c ForeignStale directly.
enum class CordonKind : uint8_t {
  Image = 0,
  Kernel = 1,
  Foreign = 2,
};

//===----------------------------------------------------------------------===//
// Public stamp / retire / toggle surface
//===----------------------------------------------------------------------===//

/// Stamp every 64 KiB chunk in `[base, base + bytes)` with the cordon
/// tag for \p kind. Idempotent.
///
/// \pre \p base is \c kPagemapChunkBytes-aligned.
/// \pre \p bytes is a positive multiple of \c kPagemapChunkBytes.
///
/// On success every covered chunk decodes to
/// `(slot_idx=0, tag=<kind-tag>)`. The pagemap range is upgraded from
/// \c PAGE_READONLY shared-zero to \c PAGE_READWRITE first; if that
/// upgrade fails, no entries are observable as stamped because the
/// publish loop is bypassed.
///
/// \returns
///   *  \c 0       — success.
///   * \c -EINVAL  — alignment / zero-length precondition violated.
///                   No pagemap state is observable.
///   * \c -ENOMEM  — \c pagemap_register_range failed (NT could not
///                   commit one of the pagemap OS pages from
///                   \c PAGE_READONLY shared-zero to \c PAGE_READWRITE).
///                   No stamp is observable.
///
/// Concurrency: single-publisher per chunk. Callers MUST NOT race two
/// \c stamp_cordon calls against overlapping ranges. Re-stamping the
/// same range with the same \p kind is a no-op; re-stamping with a
/// different \p kind is a clean rewrite.
[[nodiscard]] int stamp_cordon(void *base, size_t bytes, CordonKind kind);

/// Symmetric inverse of \c stamp_cordon. Zeroes every covered entry.
/// Used when a cordoned VA range disappears (DLL fully unloaded,
/// foreign region torn down).
///
/// \pre Same alignment / size preconditions as \c stamp_cordon. On
///      violation the call silently no-ops — teardown paths are
///      fail-soft.
///
/// Concurrency: single-publisher per chunk (do not race two
/// \c retire_cordon calls or a \c retire_cordon against a
/// \c stamp_cordon over the same VA). Idempotent against already-
/// retired ranges.
void retire_cordon(void *base, size_t bytes);

/// Toggle Foreign → ForeignStale across `[base, base + bytes)`.
///
/// Walks the range chunk-by-chunk; for each chunk whose current decoded
/// tag is \c Foreign, attempts a single CAS to flip the encoded word
/// to the \c (slot_idx=0, ForeignStale) form. A lost CAS triggers one
/// reload-and-retry; a second loss leaves the chunk alone (a
/// concurrent writer published over it; we don't fight that). Chunks
/// outside the foreign band (Image, Kernel, ForeignStale, libc-owned,
/// POSIX-tagged, Empty) are left untouched.
///
/// Best-effort and idempotent.
///
/// \pre Same alignment / size preconditions as \c stamp_cordon. On
///      violation: silent no-op.
void set_cordon_stale(void *base, size_t bytes);

/// Toggle ForeignStale → Foreign across `[base, base + bytes)`.
/// Identical shape to \c set_cordon_stale modulo the source /
/// destination tag pair. Called when a previously-stale foreign mapping
/// is observed restored.
void clear_cordon_stale(void *base, size_t bytes);

//===----------------------------------------------------------------------===//
// Wait-free predicates
//===----------------------------------------------------------------------===//

// Same shape as `pagemap_classifier::is_cordon`, kept here so
// cordon-aware callers don't have to pull in the full classifier
// surface. One ACQUIRE load + cookie XOR + numeric range check;
// non-faulting on any user-VA address (pagemap stays committed
// PAGE_READONLY for life). Safe from VEH / SIGSEGV / __cxa_finalize.

/// \c true iff \p addr falls in a chunk currently tagged \c Image,
/// \c Kernel, \c Foreign, or \c ForeignStale.
[[nodiscard]] LIBC_INLINE bool is_cordon(const void *addr) noexcept {
  PagemapDecoded d = pagemap_load_decoded(addr);
  uint8_t tag_byte = static_cast<uint8_t>(d.tag);
  return tag_byte >= static_cast<uint8_t>(VaChunkConsumer::Image) &&
         tag_byte <= static_cast<uint8_t>(VaChunkConsumer::ForeignStale);
}

/// \c true iff \p addr falls in a chunk currently tagged
/// \c ForeignStale. False for live \c Foreign, every other cordon kind,
/// and every non-cordon tag.
[[nodiscard]] LIBC_INLINE bool is_cordon_stale(const void *addr) noexcept {
  PagemapDecoded d = pagemap_load_decoded(addr);
  return d.tag == VaChunkConsumer::ForeignStale;
}

} // namespace pagemap
} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
