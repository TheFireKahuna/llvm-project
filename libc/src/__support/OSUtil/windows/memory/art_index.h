//===- art_index.h - Outer ART index for va_tracker ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Outer index of va_tracker's two-level composition: a ROWEX adaptive
// radix tree (Leis et al., ICDE 2013; DaMoN 2016) keyed on 8-byte VA
// prefixes, with internal-node reclamation via Crystalline-W (Nikolaev
// and Ravindran, PLDI 2024). The leaf payload is `Arena *`; the inner
// per-arena range index is the interval skiplist.
//
// Readers never restart and never write memory, so the lookup path is
// SIGSEGV-callable. Each descent step pins through `pinned_get_child`,
// which routes the child fetch through the domain's `protect()`.
//
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

// Structurally fixed 8-byte key. The encoder emits `va >> 32` as a
// big-endian 64-bit word (see va_tracker.cpp::encode_art_key), so the
// high 4 bytes are zero by construction on a 47-bit user-mode VA and
// only the low 4 bytes carry information — the width is held at 8 to
// keep the ART node layout invariant against future arch widening.
inline constexpr uint32_t kArtKeyLen = 8;

// Materialises a leaf's canonical 8-byte big-endian key. The tree calls
// this when stored-prefix bytes are insufficient to decide correctness:
// optimistic-prefix tail validation (stored prefix is capped at
// `kArtMaxStoredPrefixLength`), and lazy-leaf-expansion on insert
// collision (LCP of two full keys determines the new N4's compressed
// prefix). SysV ABI — entire call chain is libc-internal, never crosses
// the NT / PE-loader boundary.
using ArtLoadKeyFn = void (*)(Arena *leaf, uint8_t out_key[kArtKeyLen]);

// Default-constructible so the singleton lives in `.bss`. `root` and
// `load_key` are populated by `art_index_init` once the chunk-pool
// partitions are live; root-node allocation must defer until then.
struct alignas(64) ArtTree {
  cpp::Atomic<ArtNodeBase *> root{nullptr};
  ArtLoadKeyFn load_key{nullptr};
};

// Wait-free; pins descent slots internally so the caller needs no
// outer domain pin. The returned arena is process-lifetime — arenas
// are never retired from a published position — so validity outlives
// the reservation drop.
[[nodiscard]] Arena *art_lookup(ArtTree &tree, const uint8_t *key,
                                  uint32_t key_len);

// Idempotent if `key` already decodes to `leaf_arena`. On a colliding
// pre-existing leaf, lazy-leaf-expansion via LCP yields a fresh N4
// carrying both leaves. Returns false on chunk-pool exhaustion; the
// caller surfaces this as `-ENOMEM`.
[[nodiscard]] bool art_insert(ArtTree &tree, const uint8_t *key,
                                uint32_t key_len, Arena *leaf_arena);

// Silent no-op when the existing leaf does not decode to `leaf_arena`.
// Handles single-child collapse via `add_prefix_before` and per-type
// threshold shrink (N256→N48 at 37, N48→N16 at 12, N16→N4 at 3).
[[nodiscard]] bool art_remove(ArtTree &tree, const uint8_t *key,
                                uint32_t key_len, Arena *leaf_arena);

// Function-pointer form is mandatory: no lambdas in this libc.
using ArtVisitor = void (*)(const uint8_t *key, uint32_t key_len,
                             Arena *leaf, void *ctx);

// Ordered iteration over `[lo_key, hi_key]`. Visitor runs under the
// caller's Crystalline pin and must not re-enter ART mutating entry
// points.
uint32_t art_walk_range(ArtTree &tree, const uint8_t *lo_key,
                          const uint8_t *hi_key, uint32_t key_len,
                          ArtVisitor visitor, void *ctx);

// Reserves the four pinned per-node-type 4 GiB partitions (chunk-owner
// contract requires one writer per partition range), populates per-type
// slot/chunk sizes, allocates the process-lifetime root Node256, and
// stores `load_key`. Must run after chunk-pool partitions come online.
void art_index_init(ArtLoadKeyFn load_key);

// Per chunk: rederive `chunk_canary` and every live node's
// `node_canary` from the rotated partition_secret, then scrub each live
// node's writer lock via `fetch_add(0b10)` — a dead-thread-held writer
// lock would otherwise spinlock inheriting threads inside
// `write_lock_or_restart`. Sentinel slots roll back through partition
// unregister + page decommit; retired-but-unreaped descriptor slots are
// reclaimed; per-type alloc hints reset; `next_chunk_id` is preserved.
void art_index_fork_reinit();

// Co-located with the algorithm — per-TU duplicates would silently
// bypass `load_key` installation.
extern ArtTree g_art_tree;

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
