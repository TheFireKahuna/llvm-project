//===- art_index.h - Outer ART index for va_tracker ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Public façade for the ROWEX-synchronised Adaptive Radix Tree that maps
/// 8-byte big-endian VA prefixes to `Arena *` leaves. ART is the outer
/// index of the va_tracker two-level composition (Leis et al., ICDE 2013);
/// the inner per-leaf range index is the interval skiplist.
///
/// Keys partition the 47-bit user VA into 4 GiB regions, one leaf per
/// region (the low 32 bits of the VA address the inner per-arena
/// skiplist; the outer ART keys on the upper bits — see
/// `leaf_va_prefix` in `va_tracker.cpp`). Internal nodes are reclaimed
/// through `g_va_tracker_art_domain`
/// (Crystalline-W, Nikolaev and Ravindran, PLDI 2024), so the SIGSEGV-
/// callable lookup path is wait-free over ART traversal.
///
/// Synchronisation follows ROWEX (Leis et al., DaMoN 2016): readers do
/// not write memory and never restart; writers serialise via a per-node
/// lock+version word and acquire parent-then-child. Each read pin is
/// established inside `pinned_get_child` via the domain's `protect()`
/// fast path; the `LoadKeyFn` callback resolves optimistic prefix tails
/// and lazy-leaf collisions against a descendant leaf's full key.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_INDEX_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_INDEX_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/art_node.h"
#include "src/__support/OSUtil/windows/memory/art_node_alloc.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

/// Key width in bytes for all ART keys in this deployment. 8 bytes
/// covers the 47-bit user-mode VA space with three bytes of headroom
/// after the partition shift.
inline constexpr uint32_t kArtKeyLen = 8;

/// Callback that materialises a leaf's canonical key. Given a leaf
/// `Arena *`, writes its 8-byte big-endian key into \p out_key.
///
/// The tree calls this in two situations where the stored prefix alone
/// cannot decide correctness:
///   * Optimistic-prefix tail validation. Stored prefixes are capped at
///     `kArtMaxStoredPrefixLength = 4` bytes; key bytes beyond that are
///     verified against a descendant leaf's real key.
///   * Lazy-leaf-expansion on insert collision. When an insert descends
///     to an existing leaf with a different key, the LCP of the two
///     full keys determines the new N4's compressed prefix.
///
/// SysV ABI by default; `LIBC_MSABI` is intentionally not applied. The
/// entire call chain is libc-internal and never crosses into NT / PE
/// loader code.
using ArtLoadKeyFn = void (*)(Arena *leaf, uint8_t out_key[kArtKeyLen]);

/// Process-singleton ART instance: root pointer plus the key-decode
/// callback installed at init.
///
/// Default-constructible so the singleton can live in `.bss`. The
/// constructor leaves `root` null and `load_key` null; both fields are
/// populated by `art_index_init` once the partition allocator is live
/// (Tier A Phase 6, after Phase 5 partitions come online). The
/// reference implementation does this in its constructor; the two-phase
/// init here defers root-node allocation until the chunk pool exists.
struct alignas(64) ArtTree {
  cpp::Atomic<ArtNodeBase *> root{nullptr};
  ArtLoadKeyFn load_key{nullptr};
};

/// Wait-free point lookup. Returns the arena bound to \p key, or null
/// if no mapping exists.
///
/// Internally establishes Crystalline-W reservations on the descent
/// slots via `pinned_get_child`; the caller need not hold a domain pin.
/// The returned arena is process-lifetime (arenas are never retired
/// from a published position), so its validity extends past the
/// reservation drop on return.
///
/// \pre \p key_len equals `kArtKeyLen`.
[[nodiscard]] Arena *art_lookup(ArtTree &tree, const uint8_t *key,
                                  uint32_t key_len);

/// ROWEX-synchronised insert binding \p key to \p leaf_arena.
///
/// If a leaf already exists for \p key, the implementation matches the
/// reference: lazy-leaf-expansion via the LCP of the two keys yields a
/// fresh N4 carrying both leaves. If the existing leaf already decodes
/// to \p leaf_arena the insert is idempotent.
///
/// \returns true on success, false on chunk-pool exhaustion (surface
///          as `-ENOMEM` at the caller).
[[nodiscard]] bool art_insert(ArtTree &tree, const uint8_t *key,
                                uint32_t key_len, Arena *leaf_arena);

/// ROWEX-synchronised remove. Drops the mapping for \p key when its
/// leaf decodes to \p leaf_arena; otherwise a silent no-op.
///
/// Handles single-child collapse via `ArtNodeBase::add_prefix_before`
/// and threshold shrink at the per-type hysteresis points (Node256 to
/// Node48 at count 37, Node48 to Node16 at 12, Node16 to Node4 at 3).
///
/// \returns true when a matching mapping was removed.
[[nodiscard]] bool art_remove(ArtTree &tree, const uint8_t *key,
                                uint32_t key_len, Arena *leaf_arena);

/// Visitor signature for `art_walk_range`. The function pointer form
/// is mandatory: no lambdas in this libc.
using ArtVisitor = void (*)(const uint8_t *key, uint32_t key_len,
                             Arena *leaf, void *ctx);

/// Ordered iteration over leaves with keys in `[lo_key, hi_key]`.
/// Returns the number of leaves visited.
///
/// The visitor is invoked under the caller's Crystalline pin and must
/// not re-enter ART mutating entry points.
uint32_t art_walk_range(ArtTree &tree, const uint8_t *lo_key,
                          const uint8_t *hi_key, uint32_t key_len,
                          ArtVisitor visitor, void *ctx);

/// One-shot tree initialiser, called from the memory-primitives bring-up
/// (Tier A Phase 6, after partition Phase 5).
///
/// Reserves the four pinned `VaTrackerArtNode4..256` 4 GiB partitions
/// (one per node type — the partition contract requires the chunk owner
/// to be the sole writer of any chunk pagemap entry within the partition
/// range), populates per-type slot/chunk sizes, allocates the
/// process-lifetime root Node256, and stores \p load_key on the
/// singleton.
void art_index_init(ArtLoadKeyFn load_key);

/// Fork-reinit hook, registered at `kForkPrioVaTracker = 39`. Runs in
/// the child after partition reinit (priority 38) and Zone 0b cookie
/// rotation (priority 36).
///
/// Per chunk, the hook re-derives `chunk_canary` from the rotated
/// partition_secret, re-derives every live node's `node_canary`, and
/// scrubs each live node's writer lock via a `fetch_add(0b10)` on the
/// lock word: dead-thread-held writer locks would otherwise spinlock
/// inheriting threads inside `write_lock_or_restart`. Sentinel-
/// installing slots roll back through partition unregister plus page
/// decommit; retired-but-unreaped descriptor slots are reclaimed; per-
/// type alloc hints reset; the high-water `next_chunk_id` is preserved.
void art_index_fork_reinit();

/// Process-singleton tree instance. Defined in `art_tree.cpp`; consumed
/// by `va_tracker.cpp`. Keeping the definition co-located with the
/// algorithm prevents per-TU duplicates that would silently bypass
/// `load_key` installation.
extern ArtTree g_art_tree;

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
