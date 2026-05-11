//===- art_tree.cpp - ROWEX ART tree operations ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Top-level tree operations for the Adaptive Radix Tree (Leis, Kemper,
// Neumann, ICDE 2013) synchronised under the Read-Optimised Write
// EXclusion protocol (Leis, Scheibner, Kemper, Neumann, DaMoN 2016).
//
// Reader paths (\c art_lookup, the snapshot portion of \c art_remove,
// \c art_walk_range) traverse without acquiring locks under a
// Crystalline-W pin (Nikolaev & Ravindran, PLDI 2024). Writer paths
// (\c art_insert, mutating portion of \c art_remove) acquire per-node
// version-lock words in a fixed root-first descent order; on parent /
// child relock conflicts the writer restarts from the root. Grow /
// shrink between Node4/16/48/256 happens in \c art_node.cpp via
// \c art_node_insert_and_unlock and \c art_node_remove_and_unlock —
// this file orchestrates the descent and lock-coupling.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/art_index.h"
#include "src/__support/OSUtil/windows/memory/art_node.h"
#include "src/__support/OSUtil/windows/memory/art_node_alloc.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/primitives/init_latch.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/stdint_proxy.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

namespace {
// Process-singleton init latch — guards art_index_init against
// double-call under Tier A serial bring-up.
::LIBC_NAMESPACE::internal::alloc_primitives::InitLatch g_art_init;
} // namespace

//===----------------------------------------------------------------------===//
//  Prefix-check helpers (Leis ICDE 2013 path compression)
//===----------------------------------------------------------------------===//

// Hybrid path compression (Leis ICDE 2013 §III) stores up to
// kArtMaxStoredPrefixLength bytes of a compressed path inline on the
// node; longer prefixes are "optimistic" — the unstored tail must be
// recovered from any descendant leaf's full key via the tree's
// load_key callback. The four helpers below mirror the reference's
// checkPrefix / checkPrefixPessimistic / checkPrefixCompare /
// checkPrefixEquals families, advancing the caller's level cursor by
// the number of prefix bytes consumed.

enum class CheckPrefixResult {
  Match,
  NoMatch,
  OptimisticMatch,
};

enum class CheckPrefixPessimisticResult {
  Match,
  NoMatch,
  SkippedLevel,
};

enum class PCCompareResults {
  Smaller,
  Equal,
  Bigger,
  SkippedLevel,
};

enum class PCEqualsResults {
  BothMatch,
  Contained,
  NoMatch,
  SkippedLevel,
};

namespace {

LIBC_INLINE uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

// Fast (no load_key) prefix check used on the lookup path.
//
// Returns Match when every stored prefix byte equals the corresponding
// key byte; OptimisticMatch when the prefix continues past the stored
// portion (caller must validate against a descendant leaf via
// check_key); NoMatch on a stored-byte divergence.
CheckPrefixResult check_prefix(ArtNodeBase *n, const uint8_t *k,
                                uint32_t key_len, uint32_t &level) {
  if (key_len <= n->get_level())
    return CheckPrefixResult::NoMatch;
  ArtPrefix p = n->get_prefix();
  if (p.prefix_count + level < n->get_level()) {
    level = n->get_level();
    return CheckPrefixResult::OptimisticMatch;
  }
  if (p.prefix_count > 0) {
    uint32_t start = (level + p.prefix_count) - n->get_level();
    uint32_t stop = min_u32(p.prefix_count, kArtMaxStoredPrefixLength);
    for (uint32_t i = start; i < stop; ++i) {
      if (p.prefix[i] != k[level])
        return CheckPrefixResult::NoMatch;
      ++level;
    }
    if (p.prefix_count > kArtMaxStoredPrefixLength) {
      level += p.prefix_count - kArtMaxStoredPrefixLength;
      return CheckPrefixResult::OptimisticMatch;
    }
  }
  return CheckPrefixResult::Match;
}

// Insert-side prefix check with load_key fallback for the optimistic
// tail.
//
// When the diverging byte falls past kArtMaxStoredPrefixLength, the
// callback recovers the unstored bytes from any descendant leaf. On
// NoMatch the caller receives the trailing prefix bytes via
// \p non_matching_prefix_out so the split-N4 can re-install them on
// the existing node post-split.
CheckPrefixPessimisticResult
check_prefix_pessimistic(ArtNodeBase *n, const uint8_t *k, uint32_t /*key_len*/,
                          uint32_t &level, uint8_t &non_matching_key_out,
                          ArtPrefix &non_matching_prefix_out,
                          ArtLoadKeyFn load_key) {
  ArtPrefix p = n->get_prefix();
  if (p.prefix_count + level < n->get_level())
    return CheckPrefixPessimisticResult::SkippedLevel;

  if (p.prefix_count > 0) {
    uint32_t prev_level = level;
    uint8_t kt[kArtKeyLen] = {};
    bool kt_loaded = false;
    uint32_t start = (level + p.prefix_count) - n->get_level();
    for (uint32_t i = start; i < p.prefix_count; ++i) {
      if (i == kArtMaxStoredPrefixLength && !kt_loaded) {
        Arena *any = art_node_get_any_child_tid(n);
        if (any != nullptr && load_key != nullptr) {
          load_key(any, kt);
          kt_loaded = true;
        }
      }
      uint8_t cur_key = (i >= kArtMaxStoredPrefixLength)
                            ? (level < kArtKeyLen ? kt[level] : 0)
                            : p.prefix[i];
      if (cur_key != k[level]) {
        non_matching_key_out = cur_key;
        if (p.prefix_count > kArtMaxStoredPrefixLength) {
          if (i < kArtMaxStoredPrefixLength) {
            Arena *any = art_node_get_any_child_tid(n);
            if (any != nullptr && load_key != nullptr) {
              load_key(any, kt);
              kt_loaded = true;
            }
          }
          uint32_t lim = (p.prefix_count - (level - prev_level) - 1);
          uint32_t lim_clamped = min_u32(lim, kArtMaxStoredPrefixLength);
          non_matching_prefix_out.prefix_count = lim_clamped;
          for (uint32_t j = 0; j < lim_clamped; ++j) {
            uint32_t src = level + j + 1;
            non_matching_prefix_out.prefix[j] =
                (src < kArtKeyLen && kt_loaded) ? kt[src] : 0;
          }
        } else {
          uint32_t lim = p.prefix_count - i - 1;
          non_matching_prefix_out.prefix_count = lim;
          for (uint32_t j = 0; j < lim && j < kArtMaxStoredPrefixLength; ++j) {
            non_matching_prefix_out.prefix[j] = p.prefix[i + j + 1];
          }
        }
        return CheckPrefixPessimisticResult::NoMatch;
      }
      ++level;
    }
  }
  return CheckPrefixPessimisticResult::Match;
}

// Range-walk prefix check producing a lexicographic ordering verdict
// relative to a bound key.
PCCompareResults check_prefix_compare(ArtNodeBase *n, const uint8_t *k,
                                        uint32_t key_len, uint32_t &level,
                                        ArtLoadKeyFn load_key) {
  ArtPrefix p = n->get_prefix();
  if (p.prefix_count + level < n->get_level())
    return PCCompareResults::SkippedLevel;
  if (p.prefix_count > 0) {
    uint8_t kt[kArtKeyLen] = {};
    bool kt_loaded = false;
    uint32_t start = (level + p.prefix_count) - n->get_level();
    for (uint32_t i = start; i < p.prefix_count; ++i) {
      if (i == kArtMaxStoredPrefixLength && !kt_loaded) {
        Arena *any = art_node_get_any_child_tid(n);
        if (any != nullptr && load_key != nullptr) {
          load_key(any, kt);
          kt_loaded = true;
        }
      }
      uint8_t k_level = (key_len > level) ? k[level] : 0;
      uint8_t cur_key = (i >= kArtMaxStoredPrefixLength)
                            ? (level < kArtKeyLen ? kt[level] : 0)
                            : p.prefix[i];
      if (cur_key < k_level)
        return PCCompareResults::Smaller;
      if (cur_key > k_level)
        return PCCompareResults::Bigger;
      ++level;
    }
  }
  return PCCompareResults::Equal;
}

// Range-walk prefix check against both bounds simultaneously, used at
// the top of the descent before the bound paths diverge.
PCEqualsResults check_prefix_equals(ArtNodeBase *n, uint32_t &level,
                                       const uint8_t *start_k,
                                       uint32_t start_len, const uint8_t *end_k,
                                       uint32_t end_len,
                                       ArtLoadKeyFn load_key) {
  ArtPrefix p = n->get_prefix();
  if (p.prefix_count + level < n->get_level())
    return PCEqualsResults::SkippedLevel;
  if (p.prefix_count > 0) {
    uint8_t kt[kArtKeyLen] = {};
    bool kt_loaded = false;
    uint32_t pstart = (level + p.prefix_count) - n->get_level();
    for (uint32_t i = pstart; i < p.prefix_count; ++i) {
      if (i == kArtMaxStoredPrefixLength && !kt_loaded) {
        Arena *any = art_node_get_any_child_tid(n);
        if (any != nullptr && load_key != nullptr) {
          load_key(any, kt);
          kt_loaded = true;
        }
      }
      uint8_t s_level = (start_len > level) ? start_k[level] : 0;
      uint8_t e_level = (end_len > level) ? end_k[level] : 0;
      uint8_t cur_key = (i >= kArtMaxStoredPrefixLength)
                            ? (level < kArtKeyLen ? kt[level] : 0)
                            : p.prefix[i];
      if (cur_key > s_level && cur_key < e_level)
        return PCEqualsResults::Contained;
      if (cur_key < s_level || cur_key > e_level)
        return PCEqualsResults::NoMatch;
      ++level;
    }
  }
  return PCEqualsResults::BothMatch;
}

// Validate that a leaf's full stored key matches the lookup key.
//
// Used by art_lookup when an OptimisticMatch left part of the key
// unverified — Leis ICDE 2013 §III.A requires a full-key comparison
// at the leaf if any prefix bytes were skipped optimistically.
[[nodiscard]] LIBC_INLINE Arena *check_key(Arena *leaf_tid, const uint8_t *k,
                                              uint32_t key_len,
                                              ArtLoadKeyFn load_key) {
  if (load_key == nullptr || leaf_tid == nullptr)
    return leaf_tid; // Unverifiable — defer to caller.
  uint8_t kt[kArtKeyLen] = {};
  load_key(leaf_tid, kt);
  uint32_t cmp_len = key_len < kArtKeyLen ? key_len : kArtKeyLen;
  for (uint32_t i = 0; i < cmp_len; ++i) {
    if (kt[i] != k[i])
      return nullptr;
  }
  return leaf_tid;
}

} // namespace

//===----------------------------------------------------------------------===//
//  Lookup — wait-free under Crystalline pin
//===----------------------------------------------------------------------===//

// Wait-free point lookup. Carries a Crystalline-W pin (Nikolaev &
// Ravindran, PLDI 2024 §4.2) on the currently dereferenced node and
// rotates pin slots A↔B as the descent advances. The
// optimistic_prefix_match flag drives the post-leaf check_key call so
// optimistic-tail bytes are never silently trusted (Leis ICDE 2013
// §III.A).
Arena *art_lookup(ArtTree &tree, const uint8_t *key, uint32_t key_len) {
  ArtNodeBase *node = tree.root.load(cpp::MemoryOrder::ACQUIRE);
  uint32_t level = 0;
  bool optimistic_prefix_match = false;

  // Two-slot ping-pong descent. parent_slot pins the current node via
  // the previous iteration's pinned_get_child. The first iteration's
  // node is tree.root, which is a process-lifetime sentinel
  // (allocated once at art_index_init, never retired), so
  // dereferencing it inside the first pinned_get_child is safe even
  // before any reservation is established. child_slot is the slot the
  // next pinned_get_child will advance. The rotation guarantees
  // protect()'s era-advance and drain on child_slot never touch
  // parent_slot's reservation, so node stays alive across all 16
  // fast-path iterations of pinned_get_child.
  uint32_t parent_slot = kArtPinSlotDescendA;
  uint32_t child_slot = kArtPinSlotDescendB;

  while (node != nullptr) {
    switch (check_prefix(node, key, key_len, level)) {
    case CheckPrefixResult::NoMatch:
      return nullptr;
    case CheckPrefixResult::OptimisticMatch:
      optimistic_prefix_match = true;
      [[fallthrough]];
    case CheckPrefixResult::Match: {
      if (key_len <= level)
        return nullptr;
      // Pinned child load — era-stability convergence inside
      // protect() proves the loaded child is safe to dereference
      // (Nikolaev & Ravindran, PLDI 2024 §4.2 Fig. 10, §5 Lemma 5.2).
      ArtNodeBase *child =
          pinned_get_child(node, key[level], child_slot);
      if (child == nullptr)
        return nullptr;
      if (art_is_leaf(child)) {
        // Leaves are tagged Arena pointers managed by the
        // inner-index layer — not in the ART Crystalline domain, so
        // no pin needed.
        Arena *tid = art_get_leaf(child);
        if (level < key_len - 1 || optimistic_prefix_match)
          return check_key(tid, key, key_len, tree.load_key);
        return tid;
      }
      node = child;
      // Rotate: child_slot now pins node; old parent_slot becomes
      // the next iteration's child_slot (eligible for drain/advance).
      { uint32_t tmp = parent_slot; parent_slot = child_slot; child_slot = tmp; }
      ++level;
      break;
    }
    }
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
//  Insert — ROWEX-synchronized writer
//===----------------------------------------------------------------------===//

// Insert under the ROWEX writer protocol (Leis et al. DaMoN 2016 §4).
// Restart edges:
//   - SkippedLevel from check_prefix_pessimistic (concurrent split
//     changed our node's prefix mid-walk).
//   - Failed lock_version_or_restart on \c node (version moved since
//     snapshot, or node became obsolete).
//   - Failed write_lock_or_restart on \c parent (after backing out
//     the held lock on node and any allocated new node).
//   - need_restart from insert_and_unlock (parent contention or
//     chunk-pool OOM inside a grow).
//
// Chunk-pool exhaustion at the root level returns false rather than
// restarting — the only non-restart failure path.
bool art_insert(ArtTree &tree, const uint8_t *key, uint32_t key_len,
                 Arena *leaf_arena) {
  if (LIBC_UNLIKELY(leaf_arena == nullptr))
    return false;
  ArtNodeBase *leaf = art_set_leaf(leaf_arena);
  ArtLoadKeyFn load_key = tree.load_key;

restart:
  bool need_restart = false;
  ArtNodeBase *node = nullptr;
  ArtNodeBase *next_node = tree.root.load(cpp::MemoryOrder::ACQUIRE);
  ArtNodeBase *parent = nullptr;
  uint8_t parent_key = 0;
  uint8_t node_key = 0;
  uint32_t level = 0;
  // Reset both descent slots on every restart so a contention restart
  // re-enters with both slots fresh. See art_lookup for the ping-pong
  // rationale.
  uint32_t parent_slot = kArtPinSlotDescendA;
  uint32_t child_slot = kArtPinSlotDescendB;

  while (true) {
    parent = node;
    parent_key = node_key;
    node = next_node;
    if (LIBC_UNLIKELY(node == nullptr))
      return false;

    uint64_t v = node->read_version();
    uint32_t next_level = level;
    uint8_t non_matching_key = 0;
    ArtPrefix remaining_prefix{};

    switch (check_prefix_pessimistic(node, key, key_len, next_level,
                                       non_matching_key, remaining_prefix,
                                       load_key)) {
    case CheckPrefixPessimisticResult::SkippedLevel:
      goto restart;

    case CheckPrefixPessimisticResult::NoMatch: {
      // Duplicate-key prevention: the next_level cursor must still
      // point inside the key range. Treat duplicates as failure —
      // the caller is expected not to re-insert with a key it
      // already inserted.
      if (LIBC_UNLIKELY(next_level >= key_len))
        return false;

      if (!node->lock_version_or_restart(v))
        goto restart;

      // 1) Build the new parent (an N4) carrying node's prefix
      //    truncated to the matched portion. The remaining bytes
      //    (after the diverging byte) become node's new prefix
      //    below.
      ArtPrefix prefi = node->get_prefix();
      prefi.prefix_count = next_level - level;

      auto *new_node = make_node<ArtNode4, ArtNodeType::N4>(next_level, prefi);
      if (LIBC_UNLIKELY(new_node == nullptr)) {
        node->write_unlock();
        return false;
      }

      // 2) Wire the new N4: leaf at key[next_level], existing node
      //    at the non-matching key byte.
      (void)new_node->insert(key[next_level], leaf);
      (void)new_node->insert(non_matching_key, node);

      // 3) Lock parent, redirect parent's child slot to new_node,
      //    unlock parent. The redirect is the linearisation point of
      //    the split.
      if (!parent->write_lock_or_restart()) {
        g_va_tracker_art_domain.retire(new_node);
        node->write_unlock();
        goto restart;
      }
      art_node_change(parent, parent_key, new_node);
      parent->write_unlock();

      // 4) Update node's prefix in place: drop the consumed portion
      //    (matched bytes + the diverging byte).
      uint32_t consumed = (next_level - level) + 1;
      uint32_t old_count = node->get_prefix().prefix_count;
      uint32_t new_count = (old_count > consumed) ? old_count - consumed : 0;
      node->set_prefix(remaining_prefix.prefix, new_count);

      node->write_unlock();
      return true;
    }

    case CheckPrefixPessimisticResult::Match:
      break;
    }

    if (LIBC_UNLIKELY(next_level >= key_len))
      return false;
    level = next_level;
    node_key = key[level];
    // Pinned child load. The writer path also needs the pin because
    // optimistic descent dereferences next_node's fields before any
    // lock — without a pin a concurrent retire of next_node could
    // decommit its chunk under us.
    next_node = pinned_get_child(node, node_key, child_slot);
    // Rotate slots — see art_lookup for the rationale.
    { uint32_t tmp = parent_slot; parent_slot = child_slot; child_slot = tmp; }

    if (next_node == nullptr) {
      // Empty child slot — install leaf here.
      if (!node->lock_version_or_restart(v))
        goto restart;
      art_node_insert_and_unlock(node, parent, parent_key, node_key, leaf,
                                   need_restart);
      if (need_restart)
        goto restart;
      return true;
    }

    if (art_is_leaf(next_node)) {
      // Lazy leaf expansion (Leis ICDE 2013 §III.C — leaf collision
      // creates an N4 splitting at the longest common prefix of the
      // existing and new keys, with both leaves as children under
      // their diverging bytes).
      if (!node->lock_version_or_restart(v))
        goto restart;

      Arena *existing_arena = art_get_leaf(next_node);
      if (existing_arena == leaf_arena) {
        node->write_unlock();
        return true; // Idempotent re-insert of the same arena.
      }

      uint8_t existing_key[kArtKeyLen] = {};
      if (load_key != nullptr && existing_arena != nullptr)
        load_key(existing_arena, existing_key);

      ++level;
      if (LIBC_UNLIKELY(level >= key_len)) {
        // Key is an exact prefix of an already-stored key — cannot
        // insert without making one strictly contain the other.
        node->write_unlock();
        return false;
      }

      uint32_t prefix_length = 0;
      while (level + prefix_length < key_len &&
             level + prefix_length < kArtKeyLen &&
             existing_key[level + prefix_length] ==
                 key[level + prefix_length]) {
        ++prefix_length;
      }

      auto *n4 = make_node<ArtNode4, ArtNodeType::N4>(
          level + prefix_length, &key[level], prefix_length);
      if (LIBC_UNLIKELY(n4 == nullptr)) {
        node->write_unlock();
        return false;
      }

      // Both leaves become children of n4 under their respective
      // diverging bytes. The diverging byte may be one-past-end of
      // either key (clamped to 0).
      uint8_t new_div =
          (level + prefix_length < key_len) ? key[level + prefix_length] : 0;
      uint8_t old_div = (level + prefix_length < kArtKeyLen)
                             ? existing_key[level + prefix_length]
                             : 0;
      (void)n4->insert(new_div, leaf);
      (void)n4->insert(old_div, next_node);

      art_node_change(node, key[level - 1], n4);
      node->write_unlock();
      return true;
    }

    ++level;
  }
}

//===----------------------------------------------------------------------===//
//  Remove — ROWEX-synchronized writer with path-compression collapse
//===----------------------------------------------------------------------===//

// Remove under ROWEX. Path-compression collapse via add_prefix_before
// when the parent's count would drop to 1 post-erase — handled at the
// count == 2 site BEFORE the actual erase, so the descent invariant
// "no non-root internal node has a single child" is preserved
// (Leis ICDE 2013 §III.C). Shrink dispatch (Node256 → 48 → 16 → 4) is
// hidden inside art_node_remove_and_unlock when the live count
// crosses the hysteresis thresholds.
bool art_remove(ArtTree &tree, const uint8_t *key, uint32_t key_len,
                 Arena *leaf_arena) {
  if (LIBC_UNLIKELY(leaf_arena == nullptr))
    return false;

restart:
  bool need_restart = false;
  ArtNodeBase *node = nullptr;
  ArtNodeBase *next_node = tree.root.load(cpp::MemoryOrder::ACQUIRE);
  ArtNodeBase *parent = nullptr;
  uint8_t parent_key = 0;
  uint8_t node_key = 0;
  uint32_t level = 0;
  // Two-slot ping-pong descent — see art_lookup for the rationale.
  uint32_t parent_slot = kArtPinSlotDescendA;
  uint32_t child_slot = kArtPinSlotDescendB;

  while (true) {
    parent = node;
    parent_key = node_key;
    node = next_node;
    if (LIBC_UNLIKELY(node == nullptr))
      return false;

    uint64_t v = node->read_version();

    switch (check_prefix(node, key, key_len, level)) {
    case CheckPrefixResult::NoMatch:
      // Validate the snapshot via read_unlock_or_restart so a
      // concurrent setPrefix (post-split insert at node) doesn't
      // make us return false on a key whose path was actively being
      // constructed. remove is the only ART entry that produces a
      // "not found" answer based on an optimistic multi-step read.
      if (ArtNodeBase::is_obsolete(v) || !node->read_unlock_or_restart(v))
        goto restart;
      return false;

    case CheckPrefixResult::OptimisticMatch:
      [[fallthrough]];
    case CheckPrefixResult::Match: {
      if (level >= key_len) {
        // Key consumed entirely by prefix matching — there's no
        // further byte to descend on. Validate the snapshot before
        // returning false (same rationale as NoMatch).
        if (ArtNodeBase::is_obsolete(v) || !node->read_unlock_or_restart(v))
          goto restart;
        return false;
      }
      node_key = key[level];
      next_node = pinned_get_child(node, node_key, child_slot);
      { uint32_t tmp = parent_slot; parent_slot = child_slot; child_slot = tmp; }

      if (next_node == nullptr) {
        // Slot is empty in our snapshot. Validate the version window
        // covering both the prefix check AND the get_child read.
        if (ArtNodeBase::is_obsolete(v) || !node->read_unlock_or_restart(v))
          goto restart;
        return false;
      }

      if (art_is_leaf(next_node)) {
        if (!node->lock_version_or_restart(v))
          goto restart;

        if (art_get_leaf(next_node) != leaf_arena) {
          node->write_unlock();
          return false;
        }

        // ROWEX invariant: a non-root internal node never has a
        // single child — path compression keeps the surviving
        // sibling fused into the parent's prefix. Pulling up the
        // sibling is therefore done at count == 2 (before the
        // erase), not at count == 1 (after).
        ArtNodeBase *root = tree.root.load(cpp::MemoryOrder::ACQUIRE);
        uint32_t live = node->get_count();
        if (live == 2 && parent != nullptr && node != root) {
          // Pull the surviving sibling up. Only N4 can be in this
          // state — shrink hysteresis on N16/N48/N256 prevents them
          // from observing count == 2 at this site.
          ArtNode4::SecondChild sc =
              art_node_get_second_child(node, node_key);
          if (sc.child == nullptr) {
            // Defensive: invariant violation. Caller's invocation
            // was racy in a way the version window didn't catch.
            node->write_unlock();
            return false;
          }

          if (art_is_leaf(sc.child)) {
            if (!parent->write_lock_or_restart()) {
              node->write_unlock();
              goto restart;
            }
            // Linearisation: parent's slot redirects to the
            // surviving leaf, node becomes unreachable.
            art_node_change(parent, parent_key, sc.child);
            parent->write_unlock();
            node->write_unlock_obsolete();
            g_va_tracker_art_domain.retire(node);
          } else {
            uint64_t cv = sc.child->read_version();
            if (!sc.child->lock_version_or_restart(cv)) {
              node->write_unlock();
              goto restart;
            }
            if (!parent->write_lock_or_restart()) {
              node->write_unlock();
              sc.child->write_unlock();
              goto restart;
            }
            // Surviving sibling's prefix absorbs node's prefix +
            // the connecting byte (path compression — Leis ICDE
            // 2013 §III.C).
            art_node_change(parent, parent_key, sc.child);
            sc.child->add_prefix_before(node, sc.key);
            parent->write_unlock();
            node->write_unlock_obsolete();
            g_va_tracker_art_domain.retire(node);
            sc.child->write_unlock();
          }
          return true;
        }

        // Standard erase path — shrink dispatch is hidden inside
        // art_node_remove_and_unlock when the post-erase count
        // crosses the hysteresis thresholds (Node256 ≤ ~37 → 48,
        // Node48 ≤ ~12 → 16, Node16 ≤ ~3 → 4 per Leis ICDE 2013).
        art_node_remove_and_unlock(node, node_key, parent, parent_key,
                                     need_restart);
        if (need_restart)
          goto restart;
        return true;
      }

      ++level;
      break;
    }
    }
  }
}

//===----------------------------------------------------------------------===//
//  Range walk — ordered iteration via iterative DFS
//===----------------------------------------------------------------------===//

// The reference (Tree.cpp::lookupRange) uses three mutually-recursive
// std::function lambdas — copy, findStart, findEnd. We preserve the
// same bidirectional bound-aware traversal pattern but express it as
// an explicit work-stack DFS so the per-frame ArtKV scratch[256] cost
// doesn't multiply by recursion depth.
//
// Per-frame state:
//   * node + level — the subtree being iterated.
//   * mode — one of {COPY_FULL, FIND_START, FIND_END, TOP_BOTH}.
//   * key_pos_at_push — the value of ctx.key_pos at the moment the
//     parent decided to iterate this child (before the parent
//     appended the child key byte). On pop, ctx.key_pos restores to
//     this value; that single assignment rolls back the parent's
//     appended child key, this frame's prefix bytes, and any
//     descendant key bytes in one step — replacing the three
//     separate ctx.key_pos-- rollbacks the recursive version needs.
//   * keys[256] + scratch_count + scratch_idx — the sorted child key
//     bytes observed at frame init, plus the iteration cursor.
//     Children pointers are NOT cached here (would race with
//     concurrent writers); each step re-pins via pinned_get_child.
//   * start_byte / end_byte — the relevant boundary byte for the
//     mode, consumed by derive_child_mode when assigning the next
//     mode.
//
// Modes (see derive_child_mode):
//   COPY_FULL  — emit every leaf in the subtree; all children
//                recursed as COPY_FULL.
//   FIND_START — at the lower-bound boundary; matching child
//                recursed as FIND_START, lexicographically-larger
//                children as COPY_FULL.
//   FIND_END   — symmetric for upper bound.
//   TOP_BOTH   — top of tree before paths split. If start == end we
//                collapse and recurse the matching child as
//                TOP_BOTH; else iterate [start, end] with the
//                start-key child as FIND_START, mid-keys as
//                COPY_FULL, and end-key as FIND_END.
//
// Iterative form keeps stack ≤ ~2.9 KiB total regardless of tree
// shape, making the walker robust against constrained-stack contexts
// (alternate signal stack, fiber, post-fork pre-replay).

namespace {

struct WalkCtx {
  ArtTree *tree;
  const uint8_t *lo_key;
  const uint8_t *hi_key;
  uint32_t key_len;
  ArtVisitor visitor;
  void *user_ctx;
  uint32_t visit_count;
  uint32_t budget;
  bool restart;
  // Key buffer reconstructed during descent — passed to the visitor.
  uint8_t key_buf[kArtKeyLen];
  uint32_t key_pos;
};

enum class WalkMode : uint8_t {
  COPY_FULL,
  FIND_START,
  FIND_END,
  TOP_BOTH,
};

struct WalkFrame {
  ArtNodeBase *node;
  uint32_t level;
  uint32_t key_pos_at_push;   ///< ctx.key_pos to restore on pop.
  uint16_t scratch_count;
  uint16_t scratch_idx;
  WalkMode mode;
  uint8_t init_done;
  uint8_t start_byte;          ///< Valid for FIND_START / TOP_BOTH.
  uint8_t end_byte;            ///< Valid for FIND_END / TOP_BOTH.
  // Sorted child key bytes to iterate. Children pointers are not
  // cached; each iteration step calls pinned_get_child so the pin
  // discipline covers the dereference.
  uint8_t keys[256];
};

// Stack capacity: ART internal-node depth ≤ kArtKeyLen with span = 1
// byte per level. +2 for transient root + safety margin.
inline constexpr uint32_t kWalkStackDepth = kArtKeyLen + 2;

// Apply node's prefix to ctx.key_buf at ctx.key_pos. Cap at
// kArtKeyLen to prevent buffer overflow on malformed trees.
// Optimistic-tail bytes (beyond kArtMaxStoredPrefixLength) leave the
// buffer's previous content intact — same semantics as the recursive
// reference.
LIBC_INLINE uint32_t apply_prefix_to_buf(WalkCtx &ctx, ArtNodeBase *node) {
  ArtPrefix p = node->get_prefix();
  uint32_t buf_budget =
      ctx.key_pos < kArtKeyLen ? (kArtKeyLen - ctx.key_pos) : 0;
  uint32_t advance = p.prefix_count < buf_budget ? p.prefix_count : buf_budget;
  uint32_t to_copy = advance < kArtMaxStoredPrefixLength
                          ? advance
                          : kArtMaxStoredPrefixLength;
  for (uint32_t i = 0; i < to_copy; ++i)
    ctx.key_buf[ctx.key_pos + i] = p.prefix[i];
  ctx.key_pos += advance;
  return advance;
}

// Populate out_keys[0..*out_count) with the sorted child key bytes of
// node in [lo, hi]. Discards children pointers — the iterative walker
// re-pins each child via pinned_get_child per iteration step.
LIBC_INLINE void fetch_sorted_keys(ArtNodeBase *node, uint8_t lo, uint8_t hi,
                                     uint8_t *out_keys, uint16_t &out_count) {
  ArtKV scratch[256];
  uint32_t cnt = 0;
  art_node_get_children(node, lo, hi, scratch, cnt);
  out_count = static_cast<uint16_t>(cnt);
  for (uint32_t i = 0; i < cnt; ++i)
    out_keys[i] = scratch[i].k;
}

// Per-mode initial setup. Returns:
//    1 — frame initialised; iterate.
//    0 — frame should be skipped (no work; caller pops without
//        restart).
//   -1 — restart required (SkippedLevel from prefix check).
LIBC_INLINE int init_walk_frame(WalkCtx &ctx, WalkFrame &f) {
  switch (f.mode) {
  case WalkMode::COPY_FULL: {
    // Apply prefix and iterate all 256 children.
    (void)apply_prefix_to_buf(ctx, f.node);
    fetch_sorted_keys(f.node, 0u, 255u, f.keys, f.scratch_count);
    return 1;
  }
  case WalkMode::FIND_START: {
    PCCompareResults r =
        check_prefix_compare(f.node, ctx.lo_key, ctx.key_len, f.level,
                              ctx.tree->load_key);
    switch (r) {
    case PCCompareResults::Bigger:
      // Whole subtree is above the lower bound — degrade to
      // COPY_FULL.
      f.mode = WalkMode::COPY_FULL;
      (void)apply_prefix_to_buf(ctx, f.node);
      fetch_sorted_keys(f.node, 0u, 255u, f.keys, f.scratch_count);
      return 1;
    case PCCompareResults::Smaller:
      // Subtree is entirely below the lower bound — skip.
      return 0;
    case PCCompareResults::SkippedLevel:
      ctx.restart = true;
      return -1;
    case PCCompareResults::Equal:
      (void)apply_prefix_to_buf(ctx, f.node);
      f.start_byte =
          (ctx.key_len > f.level) ? ctx.lo_key[f.level] : 0;
      fetch_sorted_keys(f.node, f.start_byte, 255u, f.keys, f.scratch_count);
      return 1;
    }
    __builtin_trap();
  }
  case WalkMode::FIND_END: {
    PCCompareResults r =
        check_prefix_compare(f.node, ctx.hi_key, ctx.key_len, f.level,
                              ctx.tree->load_key);
    switch (r) {
    case PCCompareResults::Smaller:
      // Whole subtree is below the upper bound — degrade to
      // COPY_FULL.
      f.mode = WalkMode::COPY_FULL;
      (void)apply_prefix_to_buf(ctx, f.node);
      fetch_sorted_keys(f.node, 0u, 255u, f.keys, f.scratch_count);
      return 1;
    case PCCompareResults::Bigger:
      // Subtree is entirely above the upper bound — skip.
      return 0;
    case PCCompareResults::SkippedLevel:
      ctx.restart = true;
      return -1;
    case PCCompareResults::Equal:
      (void)apply_prefix_to_buf(ctx, f.node);
      f.end_byte =
          (ctx.key_len > f.level) ? ctx.hi_key[f.level] : 255;
      fetch_sorted_keys(f.node, 0u, f.end_byte, f.keys, f.scratch_count);
      return 1;
    }
    __builtin_trap();
  }
  case WalkMode::TOP_BOTH: {
    PCEqualsResults r = check_prefix_equals(f.node, f.level, ctx.lo_key,
                                              ctx.key_len, ctx.hi_key,
                                              ctx.key_len, ctx.tree->load_key);
    switch (r) {
    case PCEqualsResults::SkippedLevel:
      ctx.restart = true;
      return -1;
    case PCEqualsResults::NoMatch:
      return 0;
    case PCEqualsResults::Contained:
      // Bounds straddle the entire subtree — emit it all.
      f.mode = WalkMode::COPY_FULL;
      (void)apply_prefix_to_buf(ctx, f.node);
      fetch_sorted_keys(f.node, 0u, 255u, f.keys, f.scratch_count);
      return 1;
    case PCEqualsResults::BothMatch:
      (void)apply_prefix_to_buf(ctx, f.node);
      f.start_byte =
          (ctx.key_len > f.level) ? ctx.lo_key[f.level] : 0;
      f.end_byte =
          (ctx.key_len > f.level) ? ctx.hi_key[f.level] : 255;
      // Includes both boundary bytes; collapsed-descent (start ==
      // end) becomes keys = [start_byte] and derive_child_mode
      // returns TOP_BOTH for the single child.
      fetch_sorted_keys(f.node, f.start_byte, f.end_byte, f.keys,
                        f.scratch_count);
      return 1;
    }
    __builtin_trap();
  }
  }
  __builtin_trap();
}

// Pick the child's recursion mode from the parent's mode and the key
// byte being iterated. Mirrors the case dispatch in the reference's
// findStart / findEnd / lookupRange::BothMatch.
LIBC_INLINE WalkMode derive_child_mode(WalkMode parent, uint8_t k,
                                          uint8_t start, uint8_t end) {
  switch (parent) {
  case WalkMode::COPY_FULL:
    return WalkMode::COPY_FULL;
  case WalkMode::FIND_START:
    return (k == start) ? WalkMode::FIND_START : WalkMode::COPY_FULL;
  case WalkMode::FIND_END:
    return (k == end) ? WalkMode::FIND_END : WalkMode::COPY_FULL;
  case WalkMode::TOP_BOTH:
    if (start == end)
      return WalkMode::TOP_BOTH; // Collapsed descent.
    if (k == start)
      return WalkMode::FIND_START;
    if (k == end)
      return WalkMode::FIND_END;
    return WalkMode::COPY_FULL;
  }
  __builtin_trap();
}

} // namespace

uint32_t art_walk_range(ArtTree &tree, const uint8_t *lo_key,
                          const uint8_t *hi_key, uint32_t key_len,
                          ArtVisitor visitor, void *ctx_user) {
  if (LIBC_UNLIKELY(visitor == nullptr || lo_key == nullptr ||
                     hi_key == nullptr))
    return 0;
  if (LIBC_UNLIKELY(key_len > kArtKeyLen))
    return 0;
  // Reject empty / inverted range up front (matches reference).
  for (uint32_t i = 0; i < key_len; ++i) {
    if (lo_key[i] > hi_key[i])
      return 0;
    if (lo_key[i] < hi_key[i])
      break;
  }

  // Stack-allocated frame stack. Total ≈ kWalkStackDepth × ~290 B ≈
  // 2.9 KiB regardless of tree shape.
  WalkFrame stack[kWalkStackDepth];

restart:
  WalkCtx ctx;
  ctx.tree = &tree;
  ctx.lo_key = lo_key;
  ctx.hi_key = hi_key;
  ctx.key_len = key_len;
  ctx.visitor = visitor;
  ctx.user_ctx = ctx_user;
  ctx.visit_count = 0;
  ctx.budget = kArtMaxNodesPerType * 4;
  ctx.restart = false;
  for (uint32_t i = 0; i < kArtKeyLen; ++i)
    ctx.key_buf[i] = 0;
  ctx.key_pos = 0;

  ArtNodeBase *root = tree.root.load(cpp::MemoryOrder::ACQUIRE);
  if (root == nullptr)
    return 0;

  uint32_t depth = 0;

  // Push root frame (TOP_BOTH; key_pos_at_push = 0).
  WalkFrame &root_frame = stack[depth++];
  root_frame.node = root;
  root_frame.level = 0;
  root_frame.key_pos_at_push = 0;
  root_frame.scratch_count = 0;
  root_frame.scratch_idx = 0;
  root_frame.mode = WalkMode::TOP_BOTH;
  root_frame.init_done = 0;
  root_frame.start_byte = 0;
  root_frame.end_byte = 0;

  while (depth > 0) {
    if (ctx.visit_count >= ctx.budget)
      break;

    WalkFrame &top = stack[depth - 1];

    if (!top.init_done) {
      int r = init_walk_frame(ctx, top);
      top.init_done = 1;
      if (r < 0) {
        // Restart from the root.
        ctx.key_pos = 0;
        depth = 0;
        goto restart;
      }
      if (r == 0) {
        // Skip frame; pop without iterating.
        ctx.key_pos = top.key_pos_at_push;
        --depth;
        continue;
      }
      // r == 1: fall through to iterate.
    }

    if (top.scratch_idx >= top.scratch_count) {
      // Iteration done — pop and restore key_pos.
      ctx.key_pos = top.key_pos_at_push;
      --depth;
      continue;
    }

    uint8_t k = top.keys[top.scratch_idx];
    ++top.scratch_idx;

    // Pin the child for k from top.node. The depth-indexed pin slot
    // gives each DFS-stack ancestor its own slot for the lifetime of
    // its frame; protect()'s era advance/drain on the deeper slot
    // can never touch a shallower frame's pin. top.node itself is
    // pinned on slot kArtPinSlotWalkBase + (depth - 2) — set when
    // the parent frame loaded it as child here — or, at depth == 1,
    // is the process-lifetime root sentinel tree.root, which never
    // retires.
    uint32_t walk_slot = kArtPinSlotWalkBase + (depth - 1);
    ArtNodeBase *child = pinned_get_child(top.node, k, walk_slot);
    if (child == nullptr)
      continue;

    // Append child key to buffer at the current key_pos. The
    // parent's post-prefix key_pos is the slot we write into. We do
    // NOT increment ctx.key_pos here — the child frame owns that
    // increment via its key_pos_at_push value. (When the child pops
    // it restores ctx.key_pos to its own key_pos_at_push, which is
    // this same value, so the byte we wrote here is rolled away
    // automatically.)
    if (ctx.key_pos < kArtKeyLen)
      ctx.key_buf[ctx.key_pos] = k;

    if (art_is_leaf(child)) {
      // Emit leaf with one key byte after our position. The
      // reference does NOT call check_key here in
      // lookupRange::copy — it relies on the bound-aware structure
      // of findStart / findEnd to only descend into the matching
      // subtree. We mirror that behaviour. art_lookup's check_key
      // call is for single-key lookup with optimisticPrefixMatch,
      // which is a different invariant.
      Arena *tid = art_get_leaf(child);
      uint32_t emit_key_pos = ctx.key_pos + 1;
      if (emit_key_pos > kArtKeyLen)
        emit_key_pos = kArtKeyLen;
      ctx.visitor(ctx.key_buf, emit_key_pos, tid, ctx.user_ctx);
      ++ctx.visit_count;
      // No key_pos rollback needed — we never advanced it.
      continue;
    }

    // Internal node: push child frame.
    if (depth >= kWalkStackDepth) {
      // Defensive cap; shouldn't trip with kArtKeyLen=8.
      continue;
    }

    WalkMode child_mode =
        derive_child_mode(top.mode, k, top.start_byte, top.end_byte);

    // The child's key_pos_at_push is the parent's current
    // ctx.key_pos — the byte we just wrote at this position. When
    // the child pops and restores ctx.key_pos to this value, the
    // parent's next iteration step will overwrite this byte with
    // the next k — correct.
    uint32_t child_key_pos_at_push = ctx.key_pos;
    // Advance ctx.key_pos past the child key byte so the child's
    // prefix application and grandchild iteration write at
    // level + 1.
    ++ctx.key_pos;

    WalkFrame &cf = stack[depth++];
    cf.node = child;
    cf.level = top.level + 1;
    cf.key_pos_at_push = child_key_pos_at_push;
    cf.scratch_count = 0;
    cf.scratch_idx = 0;
    cf.mode = child_mode;
    cf.init_done = 0;
    cf.start_byte = 0;
    cf.end_byte = 0;
  }

  return ctx.visit_count;
}

//===----------------------------------------------------------------------===//
//  Init / fork-reinit
//===----------------------------------------------------------------------===//

// Process-singleton tree definition. va_tracker.cpp pulls it via the
// extern decl in art_index.h. art_index_init owns both load_key
// installation and the root-Node256 allocation.
ArtTree g_art_tree;

void art_index_init(ArtLoadKeyFn load_key) {
  // Tier A bring-up is single-threaded; a double-call here is a bug.
  if (!g_art_init.try_begin())
    __builtin_trap();

  art_alloc_init();

  g_art_tree.load_key = load_key;

  // Process-lifetime root Node256. Leis et al. DaMoN 2016 §4 keeps
  // the root permanently allocated as the densest node type so the
  // root pointer can be loaded without a pin and is never retired —
  // every descent through pinned_get_child therefore has a known-
  // alive starting node.
  ArtPrefix empty{};
  ArtNodeBase *root = make_node<ArtNode256, ArtNodeType::N256>(0, empty);
  if (LIBC_UNLIKELY(root == nullptr))
    __builtin_trap();
  g_art_tree.root.store(root, cpp::MemoryOrder::RELEASE);

  g_art_init.publish_ready();
}

void art_index_fork_reinit() {
  art_alloc_fork_reinit();
  g_art_init.fork_reinit();
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
