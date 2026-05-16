//===- art_node.cpp - ART node operations and grow/shrink dispatch -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-variant operations for the four ROWEX ART node types, runtime
// dispatch, the Crystalline-W pinned descent helper, and grow/shrink
// scaffolding called by the tree-level insert / remove.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/memory/art_node.h"
#include "src/__support/OSUtil/windows/memory/art_node_alloc.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/stdint_proxy.h"
#include <emmintrin.h>
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

//===----------------------------------------------------------------------===//
//  ArtNodeBase
//===----------------------------------------------------------------------===//

bool ArtNodeBase::write_lock_or_restart() {
  // ROWEX writer-acquire (paper §"Writer Protocol"). Observed OBSOLETE
  // aborts upward — caller restarts from the parent so it picks up the
  // replacement node via the parent's child pointer.
  for (;;) {
    uint64_t v = typeVersionLockObsolete.load(cpp::MemoryOrder::ACQUIRE);
    while (is_locked(v)) {
      _mm_pause();
      v = typeVersionLockObsolete.load(cpp::MemoryOrder::ACQUIRE);
    }
    if (LIBC_UNLIKELY(is_obsolete(v)))
      return false;
    if (typeVersionLockObsolete.compare_exchange_weak(
            v, v + 0b10ULL, cpp::MemoryOrder::ACQ_REL,
            cpp::MemoryOrder::ACQUIRE))
      return true;
  }
}

bool ArtNodeBase::lock_version_or_restart(uint64_t &version_inout) {
  // Stale snapshot, observed lock, or observed obsolete all force
  // restart from the parent.
  if (is_locked(version_inout) || is_obsolete(version_inout))
    return false;
  uint64_t expected = version_inout;
  if (typeVersionLockObsolete.compare_exchange_strong(
          expected, version_inout + 0b10ULL, cpp::MemoryOrder::ACQ_REL,
          cpp::MemoryOrder::ACQUIRE)) {
    version_inout = version_inout + 0b10ULL;
    return true;
  }
  return false;
}

void ArtNodeBase::set_prefix(const uint8_t *bytes, uint32_t length) {
  // Build on the stack, zero-pad the unstored tail, publish via one
  // 8-byte RELEASE store. A concurrent ACQUIRE-load observes either
  // pre- or post-update — never a torn `(prefix_count, bytes)` pair.
  ArtPrefix p;
  p.prefix_count = length;
  uint32_t copy_len = length < kArtMaxStoredPrefixLength
                          ? length
                          : kArtMaxStoredPrefixLength;
  for (uint32_t i = 0; i < copy_len; ++i)
    p.prefix[i] = bytes[i];
  for (uint32_t i = copy_len; i < kArtMaxStoredPrefixLength; ++i)
    p.prefix[i] = 0;
  prefix.store(p, cpp::MemoryOrder::RELEASE);
}

void ArtNodeBase::add_prefix_before(ArtNodeBase *node, uint8_t key) {
  // Fuse `node->prefix || key || this->prefix` into `this->prefix` via
  // one 8-byte RELEASE store. Composed length is `np + 1 + p`; only
  // `kArtMaxStoredPrefixLength` bytes fit inline, the rest survive as
  // optimistic-prefix (resolved against a descendant leaf's key on
  // lookup).
  ArtPrefix p = get_prefix();
  ArtPrefix np = node->get_prefix();
  uint32_t prefix_copy_count =
      (np.prefix_count + 1) < kArtMaxStoredPrefixLength
          ? (np.prefix_count + 1)
          : kArtMaxStoredPrefixLength;

  // Shift this->prefix right by `prefix_copy_count` to make room.
  // High→low iteration keeps the move in-place safe.
  uint32_t carry =
      (p.prefix_count < (kArtMaxStoredPrefixLength - prefix_copy_count))
          ? p.prefix_count
          : (kArtMaxStoredPrefixLength - prefix_copy_count);
  for (int i = static_cast<int>(carry) - 1; i >= 0; --i)
    p.prefix[prefix_copy_count + i] = p.prefix[i];

  uint32_t np_copy =
      (np.prefix_count < prefix_copy_count) ? np.prefix_count : prefix_copy_count;
  for (uint32_t i = 0; i < np_copy; ++i)
    p.prefix[i] = np.prefix[i];

  // The key byte lands at the last reserved slot only when
  // `node->prefix` is short enough for the byte to still fit inside
  // the inline window.
  if (np.prefix_count < kArtMaxStoredPrefixLength)
    p.prefix[prefix_copy_count - 1] = key;

  p.prefix_count += np.prefix_count + 1;
  prefix.store(p, cpp::MemoryOrder::RELEASE);
}

//===----------------------------------------------------------------------===//
//  ArtNode4 (no SSE; 4-element scan)
//===----------------------------------------------------------------------===//

bool ArtNode4::insert(uint8_t key, ArtNodeBase *child) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::RELAXED);
  if (cc == 4)
    return false;
  // Append-only publish: key, child, then RELEASE-bump compact_count.
  // A reader's scan masked by compact_count sees the new entry only
  // after the bump's RELEASE pairs with its ACQUIRE.
  keys[cc].store(key, cpp::MemoryOrder::RELEASE);
  children[cc].store(child, cpp::MemoryOrder::RELEASE);
  compact_count.store(static_cast<uint16_t>(cc + 1),
                       cpp::MemoryOrder::RELEASE);
  count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

void ArtNode4::change(uint8_t key, ArtNodeBase *new_val) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint16_t i = 0; i < cc; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr && keys[i].load(cpp::MemoryOrder::ACQUIRE) == key) {
      children[i].store(new_val, cpp::MemoryOrder::RELEASE);
      return;
    }
  }
  // Every `change` is preceded by a successful `get_child` — a miss
  // here is a caller invariant violation.
  __builtin_trap();
}

ArtNodeBase *ArtNode4::get_child(uint8_t key) {
  // Scan all 4 slots, not [0, count): `count` decreases on remove but
  // `compact_count` doesn't, and the append-only invariant guarantees
  // any non-null child carries a stable key byte.
  for (uint32_t i = 0; i < 4; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr && keys[i].load(cpp::MemoryOrder::ACQUIRE) == key)
      return c;
  }
  return nullptr;
}

bool ArtNode4::remove(uint8_t key, bool /*force*/) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint16_t i = 0; i < cc; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr && keys[i].load(cpp::MemoryOrder::ACQUIRE) == key) {
      // Decrement count before nulling the slot: readers tolerate
      // either order under the null-child filter, but this order
      // minimises the window where count > live population.
      count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
      children[i].store(nullptr, cpp::MemoryOrder::RELEASE);
      return true;
    }
  }
  __builtin_trap();
}

ArtNodeBase *ArtNode4::get_any_child() {
  // Leaf-preference shortens the any-child chain that
  // `art_node_get_any_child_tid` descends.
  ArtNodeBase *any = nullptr;
  for (uint32_t i = 0; i < 4; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr) {
      if (art_is_leaf(c))
        return c;
      any = c;
    }
  }
  return any;
}

ArtNode4::SecondChild ArtNode4::get_second_child(uint8_t excluded_key) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint16_t i = 0; i < cc; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr) {
      uint8_t k = keys[i].load(cpp::MemoryOrder::ACQUIRE);
      if (k != excluded_key)
        return {c, k};
    }
  }
  return {nullptr, 0};
}

// Caller holds `cur`'s writer lock (insert_grow / insert_compact /
// remove_and_shrink), so no concurrent writer mutates `cur`. The ACQUIRE
// loads off `cur` pair with the RELEASEs that previously published each
// (key, child); `bigger` is not yet parent-reachable, so `bigger->insert`'s
// own RELEASE stores are paid only against the CAS-publish that follows in
// insert_grow / insert_compact.
template <class NODE> void ArtNode4::copy_to(NODE *bigger) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint16_t i = 0; i < cc; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr) {
      uint8_t k = keys[i].load(cpp::MemoryOrder::ACQUIRE);
      (void)bigger->insert(k, c);
    }
  }
}

void ArtNode4::get_children(uint8_t start, uint8_t end, KV *out_kv,
                              uint32_t &out_count) {
  out_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c == nullptr)
      continue;
    uint8_t k = keys[i].load(cpp::MemoryOrder::ACQUIRE);
    if (k >= start && k <= end) {
      out_kv[out_count].k = k;
      out_kv[out_count].child = c;
      ++out_count;
    }
  }
  // Insertion sort: at n ≤ 4 any comparison-based sort loses on setup.
  for (uint32_t i = 1; i < out_count; ++i) {
    KV cur = out_kv[i];
    uint32_t j = i;
    while (j > 0 && out_kv[j - 1].k > cur.k) {
      out_kv[j] = out_kv[j - 1];
      --j;
    }
    out_kv[j] = cur;
  }
}

//===----------------------------------------------------------------------===//
//  ArtNode16 (SSE2 keysearch + null-child filter)
//===----------------------------------------------------------------------===//

bool ArtNode16::insert(uint8_t key, ArtNodeBase *child) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::RELAXED);
  if (cc == 16)
    return false;
  // `flip_sign` re-encodes for the signed SSE compare; see ArtNode16
  // class doc.
  keys[cc].store(flip_sign(key), cpp::MemoryOrder::RELEASE);
  children[cc].store(child, cpp::MemoryOrder::RELEASE);
  compact_count.store(static_cast<uint16_t>(cc + 1),
                       cpp::MemoryOrder::RELEASE);
  count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

cpp::Atomic<ArtNodeBase *> *ArtNode16::get_child_pos(uint8_t k) {
  // Caller holds the writer lock (callers are `change` / `remove`) — the
  // returned slot handle is safe to mutate.
  // `_mm_loadu_si128` rather than the aligned variant: `cpp::Atomic`
  // does not propagate alignment through `Atomic<uint8_t>` on every
  // compiler in this libc's matrix.
  __m128i cmp = _mm_cmpeq_epi8(
      _mm_set1_epi8(static_cast<char>(flip_sign(k))),
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(&keys[0])));
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  // Mask out slots beyond the publish frontier.
  unsigned bitfield = static_cast<unsigned>(_mm_movemask_epi8(cmp)) &
                       ((1u << cc) - 1u);
  while (bitfield) {
    unsigned pos = ctz(static_cast<uint16_t>(bitfield));
    if (children[pos].load(cpp::MemoryOrder::ACQUIRE) != nullptr)
      return &children[pos];
    bitfield = bitfield ^ (1u << pos);
  }
  return nullptr;
}

void ArtNode16::change(uint8_t key, ArtNodeBase *new_val) {
  cpp::Atomic<ArtNodeBase *> *slot = get_child_pos(key);
  if (LIBC_UNLIKELY(slot == nullptr))
    __builtin_trap();
  slot->store(new_val, cpp::MemoryOrder::RELEASE);
}

ArtNodeBase *ArtNode16::get_child(uint8_t key) {
  // Mask by the full 16-bit window — NOT by compact_count — because
  // the null-child filter re-validates every hit and the publish
  // ordering in `insert` guarantees that a hit on a slot ≥ compact_count
  // is either harmless (pre-bump in-flight insert) or fully populated.
  //
  // The second `keys[pos]` ACQUIRE compare catches phantom SSE matches
  // on a mid-write slot (paper §"ROWEX Node16 SSE phantom matches").
  __m128i cmp = _mm_cmpeq_epi8(
      _mm_set1_epi8(static_cast<char>(flip_sign(key))),
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(&keys[0])));
  unsigned bitfield = static_cast<unsigned>(_mm_movemask_epi8(cmp)) &
                       ((1u << 16) - 1u);
  while (bitfield) {
    unsigned pos = ctz(static_cast<uint16_t>(bitfield));
    ArtNodeBase *c = children[pos].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr &&
        keys[pos].load(cpp::MemoryOrder::ACQUIRE) == flip_sign(key))
      return c;
    bitfield = bitfield ^ (1u << pos);
  }
  return nullptr;
}

bool ArtNode16::remove(uint8_t key, bool force) {
  // Hysteresis: signal shrink at `live == 3` so post-decrement is 2.
  // `force` is used at the root (no parent to relock) and mid-shrink.
  uint16_t live = count.load(cpp::MemoryOrder::ACQUIRE);
  if (live == 3 && !force)
    return false;
  cpp::Atomic<ArtNodeBase *> *slot = get_child_pos(key);
  if (LIBC_UNLIKELY(slot == nullptr))
    __builtin_trap();
  slot->store(nullptr, cpp::MemoryOrder::RELEASE);
  count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

ArtNodeBase *ArtNode16::get_any_child() {
  ArtNodeBase *any = nullptr;
  for (int i = 0; i < 16; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr) {
      if (art_is_leaf(c))
        return c;
      any = c;
    }
  }
  return any;
}

template <class NODE> void ArtNode16::copy_to(NODE *bigger) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint16_t i = 0; i < cc; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr) {
      uint8_t k = flip_sign(keys[i].load(cpp::MemoryOrder::ACQUIRE));
      (void)bigger->insert(k, c);
    }
  }
}

void ArtNode16::get_children(uint8_t start, uint8_t end, KV *out_kv,
                                uint32_t &out_count) {
  out_count = 0;
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint32_t i = 0; i < cc; ++i) {
    uint8_t k = flip_sign(keys[i].load(cpp::MemoryOrder::ACQUIRE));
    if (k < start || k > end)
      continue;
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c == nullptr)
      continue;
    out_kv[out_count].k = k;
    out_kv[out_count].child = c;
    ++out_count;
  }
  for (uint32_t i = 1; i < out_count; ++i) {
    KV cur = out_kv[i];
    uint32_t j = i;
    while (j > 0 && out_kv[j - 1].k > cur.k) {
      out_kv[j] = out_kv[j - 1];
      --j;
    }
    out_kv[j] = cur;
  }
}

//===----------------------------------------------------------------------===//
//  ArtNode48 (sparse 256→48 index)
//===----------------------------------------------------------------------===//

ArtNode48::ArtNode48(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
    : ArtNodeBase(ArtNodeType::N48, lvl, pfx, pfx_len) {
  // RELAXED is sufficient — the node is not yet reader-reachable.
  for (uint32_t i = 0; i < 256; ++i)
    child_index[i].store(kArtNode48EmptyMarker, cpp::MemoryOrder::RELAXED);
}

ArtNode48::ArtNode48(uint32_t lvl, const ArtPrefix &pfx)
    : ArtNodeBase(ArtNodeType::N48, lvl, pfx) {
  for (uint32_t i = 0; i < 256; ++i)
    child_index[i].store(kArtNode48EmptyMarker, cpp::MemoryOrder::RELAXED);
}

bool ArtNode48::insert(uint8_t key, ArtNodeBase *child) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::RELAXED);
  if (cc == 48)
    return false;
  // Populate the child slot then RELEASE-publish via child_index — the
  // index store is the linearisation point (see ArtNode48 class doc).
  children[cc].store(child, cpp::MemoryOrder::RELEASE);
  child_index[key].store(static_cast<uint8_t>(cc), cpp::MemoryOrder::RELEASE);
  compact_count.store(static_cast<uint16_t>(cc + 1),
                       cpp::MemoryOrder::RELEASE);
  count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

void ArtNode48::change(uint8_t key, ArtNodeBase *new_val) {
  uint8_t idx = child_index[key].load(cpp::MemoryOrder::ACQUIRE);
  if (LIBC_UNLIKELY(idx == kArtNode48EmptyMarker))
    __builtin_trap();
  children[idx].store(new_val, cpp::MemoryOrder::RELEASE);
}

ArtNodeBase *ArtNode48::get_child(uint8_t key) {
  uint8_t idx = child_index[key].load(cpp::MemoryOrder::ACQUIRE);
  if (idx == kArtNode48EmptyMarker)
    return nullptr;
  return children[idx].load(cpp::MemoryOrder::ACQUIRE);
}

bool ArtNode48::remove(uint8_t key, bool force) {
  // Hysteresis: shrink at live == 12; post-decrement is 11.
  uint16_t live = count.load(cpp::MemoryOrder::ACQUIRE);
  if (live == 12 && !force)
    return false;
  uint8_t idx = child_index[key].load(cpp::MemoryOrder::ACQUIRE);
  if (LIBC_UNLIKELY(idx == kArtNode48EmptyMarker))
    __builtin_trap();
  // Null the child pointer before clearing the index byte: a reader
  // that snapped the pre-clear index then loads the now-null pointer
  // and returns null — the pre-state-correct linearised outcome.
  children[idx].store(nullptr, cpp::MemoryOrder::RELEASE);
  child_index[key].store(kArtNode48EmptyMarker, cpp::MemoryOrder::RELEASE);
  count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

ArtNodeBase *ArtNode48::get_any_child() {
  ArtNodeBase *any = nullptr;
  for (unsigned i = 0; i < 48; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr) {
      if (art_is_leaf(c))
        return c;
      any = c;
    }
  }
  return any;
}

template <class NODE> void ArtNode48::copy_to(NODE *bigger) {
  for (uint32_t i = 0; i < 256; ++i) {
    uint8_t idx = child_index[i].load(cpp::MemoryOrder::ACQUIRE);
    if (idx != kArtNode48EmptyMarker) {
      ArtNodeBase *c = children[idx].load(cpp::MemoryOrder::ACQUIRE);
      if (c != nullptr)
        (void)bigger->insert(static_cast<uint8_t>(i), c);
    }
  }
}

void ArtNode48::get_children(uint8_t start, uint8_t end, KV *out_kv,
                                uint32_t &out_count) {
  out_count = 0;
  for (uint32_t i = start; i <= end; ++i) {
    uint8_t idx = child_index[i].load(cpp::MemoryOrder::ACQUIRE);
    if (idx == kArtNode48EmptyMarker)
      continue;
    ArtNodeBase *c = children[idx].load(cpp::MemoryOrder::ACQUIRE);
    if (c == nullptr)
      continue;
    out_kv[out_count].k = static_cast<uint8_t>(i);
    out_kv[out_count].child = c;
    ++out_count;
    // Avoid uint8_t wrap when end == 255.
    if (i == 255)
      break;
  }
}

//===----------------------------------------------------------------------===//
//  ArtNode256 (direct 256-pointer array)
//===----------------------------------------------------------------------===//

bool ArtNode256::insert(uint8_t key, ArtNodeBase * child) {
  children[key].store(child, cpp::MemoryOrder::RELEASE);
  count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

void ArtNode256::change(uint8_t key, ArtNodeBase *new_val) {
  children[key].store(new_val, cpp::MemoryOrder::RELEASE);
}

ArtNodeBase *ArtNode256::get_child(uint8_t key) {
  return children[key].load(cpp::MemoryOrder::ACQUIRE);
}

bool ArtNode256::remove(uint8_t key, bool force) {
  // Hysteresis: shrink at live == 37.
  uint16_t live = count.load(cpp::MemoryOrder::ACQUIRE);
  if (live == 37 && !force)
    return false;
  children[key].store(nullptr, cpp::MemoryOrder::RELEASE);
  count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

ArtNodeBase *ArtNode256::get_any_child() {
  ArtNodeBase *any = nullptr;
  for (uint32_t i = 0; i < 256; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr) {
      if (art_is_leaf(c))
        return c;
      any = c;
    }
  }
  return any;
}

template <class NODE> void ArtNode256::copy_to(NODE *bigger) {
  for (uint32_t i = 0; i < 256; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr)
      (void)bigger->insert(static_cast<uint8_t>(i), c);
  }
}

void ArtNode256::get_children(uint8_t start, uint8_t end, KV *out_kv,
                                 uint32_t &out_count) {
  out_count = 0;
  for (uint32_t i = start; i <= end; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c == nullptr)
      continue;
    out_kv[out_count].k = static_cast<uint8_t>(i);
    out_kv[out_count].child = c;
    ++out_count;
    if (i == 255)
      break;
  }
}

// Without explicit instantiation the compiler folds `copy_to` into
// `insert_grow<>` and emits no standalone symbol, breaking any external
// caller (notably the per-node-type direct-grow unit tests).
template void ArtNode4::copy_to<ArtNode16>(ArtNode16 *);
template void ArtNode16::copy_to<ArtNode48>(ArtNode48 *);
template void ArtNode48::copy_to<ArtNode256>(ArtNode256 *);

//===----------------------------------------------------------------------===//
//  Runtime-type-dispatched free functions
//===----------------------------------------------------------------------===//

ArtNodeBase *art_node_get_child(ArtNodeBase *node, uint8_t key) {
  switch (node->node_type()) {
  case ArtNodeType::N4:
    return static_cast<ArtNode4 *>(node)->get_child(key);
  case ArtNodeType::N16:
    return static_cast<ArtNode16 *>(node)->get_child(key);
  case ArtNodeType::N48:
    return static_cast<ArtNode48 *>(node)->get_child(key);
  case ArtNodeType::N256:
    return static_cast<ArtNode256 *>(node)->get_child(key);
  }
  __builtin_trap();
}

void art_node_change(ArtNodeBase *node, uint8_t key, ArtNodeBase *new_val) {
  switch (node->node_type()) {
  case ArtNodeType::N4:
    static_cast<ArtNode4 *>(node)->change(key, new_val);
    return;
  case ArtNodeType::N16:
    static_cast<ArtNode16 *>(node)->change(key, new_val);
    return;
  case ArtNodeType::N48:
    static_cast<ArtNode48 *>(node)->change(key, new_val);
    return;
  case ArtNodeType::N256:
    static_cast<ArtNode256 *>(node)->change(key, new_val);
    return;
  }
  __builtin_trap();
}

ArtNodeBase *art_node_get_any_child(ArtNodeBase *node) {
  switch (node->node_type()) {
  case ArtNodeType::N4:
    return static_cast<ArtNode4 *>(node)->get_any_child();
  case ArtNodeType::N16:
    return static_cast<ArtNode16 *>(node)->get_any_child();
  case ArtNodeType::N48:
    return static_cast<ArtNode48 *>(node)->get_any_child();
  case ArtNodeType::N256:
    return static_cast<ArtNode256 *>(node)->get_any_child();
  }
  __builtin_trap();
}

Arena *art_node_get_any_child_tid(ArtNodeBase *node) {
  ArtNodeBase *cur = node;
  while (cur != nullptr) {
    ArtNodeBase *next = art_node_get_any_child(cur);
    if (LIBC_UNLIKELY(next == nullptr))
      return nullptr;
    if (art_is_leaf(next))
      return art_get_leaf(next);
    cur = next;
  }
  return nullptr;
}

ArtNode4::SecondChild art_node_get_second_child(ArtNodeBase *node,
                                                  uint8_t excluded_key) {
  // Single-child-collapse only fires on N4 — anything else is a caller
  // invariant violation.
  if (LIBC_UNLIKELY(node->node_type() != ArtNodeType::N4))
    __builtin_trap();
  return static_cast<ArtNode4 *>(node)->get_second_child(excluded_key);
}

void art_node_get_children(ArtNodeBase *node, uint8_t start, uint8_t end,
                            ArtKV *out, uint32_t &out_count) {
  // Field-by-field copy from each per-type nested `KV` into `ArtKV`
  // avoids reinterpret_cast across the unrelated nested types.
  switch (node->node_type()) {
  case ArtNodeType::N4: {
    ArtNode4::KV scratch[4];
    static_cast<ArtNode4 *>(node)->get_children(start, end, scratch,
                                                   out_count);
    for (uint32_t i = 0; i < out_count; ++i) {
      out[i].k = scratch[i].k;
      out[i].child = scratch[i].child;
    }
    return;
  }
  case ArtNodeType::N16: {
    ArtNode16::KV scratch[16];
    static_cast<ArtNode16 *>(node)->get_children(start, end, scratch,
                                                    out_count);
    for (uint32_t i = 0; i < out_count; ++i) {
      out[i].k = scratch[i].k;
      out[i].child = scratch[i].child;
    }
    return;
  }
  case ArtNodeType::N48: {
    ArtNode48::KV scratch[256];
    static_cast<ArtNode48 *>(node)->get_children(start, end, scratch,
                                                    out_count);
    for (uint32_t i = 0; i < out_count; ++i) {
      out[i].k = scratch[i].k;
      out[i].child = scratch[i].child;
    }
    return;
  }
  case ArtNodeType::N256: {
    ArtNode256::KV scratch[256];
    static_cast<ArtNode256 *>(node)->get_children(start, end, scratch,
                                                     out_count);
    for (uint32_t i = 0; i < out_count; ++i) {
      out[i].k = scratch[i].k;
      out[i].child = scratch[i].child;
    }
    return;
  }
  }
  __builtin_trap();
}

//===----------------------------------------------------------------------===//
//  Crystalline-W pinned descent helper
//===----------------------------------------------------------------------===//

namespace {

// Closure for `CrystallineDomain::protect()`'s generalised overload
// (paper §4.2 Fig. 10). May be invoked by the calling thread on the
// fast path or by a foreign helper thread on slow-path handoff.
struct ArtChildLoadCtx {
  ArtNodeBase *parent;
  uint8_t      key_byte;
};

ArtNodeBase *art_load_child_thunk(void *ctx_p) {
  auto *c = static_cast<ArtChildLoadCtx *>(ctx_p);
  return art_node_get_child(c->parent, c->key_byte);
}

} // namespace

ArtNodeBase *pinned_get_child(ArtNodeBase *parent, uint8_t key_byte,
                               uint32_t hr_idx) {
  ArtChildLoadCtx ctx{parent, key_byte};
  return g_va_tracker_art_domain.protect<&art_load_child_thunk>(
      ctx, hr_idx, parent);
}

//===----------------------------------------------------------------------===//
//  Insert grow / compact dispatch
//===----------------------------------------------------------------------===//

namespace {

// Grow to the next-larger variant. In-place insert is tried first;
// only on structural fullness do we allocate a fresh bigger node, copy
// live children, install the new entry, and CAS the parent's child
// pointer to flip the linearisation point from `cur` to `bigger`.
template <class CurNode, class BiggerNode, ArtNodeType BiggerTag>
void insert_grow(CurNode *cur, ArtNodeBase *parent, uint8_t parent_key,
                  uint8_t new_key, ArtNodeBase *new_val,
                  bool &need_restart_out) {
  if (cur->insert(new_key, new_val)) {
    cur->write_unlock();
    return;
  }
  auto *bigger =
      make_node<BiggerNode, BiggerTag>(cur->level, cur->get_prefix());
  if (LIBC_UNLIKELY(bigger == nullptr)) {
    // Chunk-pool OOM: restart from root; persistent exhaustion will
    // surface as `-ENOMEM` at the public API boundary.
    cur->write_unlock();
    need_restart_out = true;
    return;
  }
  cur->copy_to(bigger);
  (void)bigger->insert(new_key, new_val);

  if (!parent->write_lock_or_restart()) {
    // Route the unreachable fresh node through Crystalline retire even
    // though no grace period is logically required — keeps slot-pool
    // accounting consistent.
    g_va_tracker_art_domain.retire(bigger);
    cur->write_unlock();
    need_restart_out = true;
    return;
  }

  // Linearisation point of the grow: the parent's child pointer flips from
  // `cur` to `bigger`. Pre-flip readers still see `cur` (kept alive by
  // Crystalline until grace closes); post-flip readers see `bigger`.
  art_node_change(parent, parent_key, bigger);
  parent->write_unlock();

  cur->write_unlock_obsolete();
  g_va_tracker_art_domain.retire(cur);
}

// Same type in / out: the append-only invariant means cleared slots
// can only be reclaimed by rebuilding the node.
template <class CurNode, ArtNodeType CurTag>
void insert_compact(CurNode *cur, ArtNodeBase *parent, uint8_t parent_key,
                     uint8_t new_key, ArtNodeBase *new_val,
                     bool &need_restart_out) {
  auto *fresh = make_node<CurNode, CurTag>(cur->level, cur->get_prefix());
  if (LIBC_UNLIKELY(fresh == nullptr)) {
    cur->write_unlock();
    need_restart_out = true;
    return;
  }
  cur->copy_to(fresh);
  (void)fresh->insert(new_key, new_val);

  if (!parent->write_lock_or_restart()) {
    g_va_tracker_art_domain.retire(fresh);
    cur->write_unlock();
    need_restart_out = true;
    return;
  }
  // Linearisation point of the compact: parent's child pointer flips from
  // `cur` to `fresh`. Pre-flip readers see `cur`; post-flip readers see
  // `fresh`. `cur` survives the flip via Crystalline grace.
  art_node_change(parent, parent_key, fresh);
  parent->write_unlock();

  cur->write_unlock_obsolete();
  g_va_tracker_art_domain.retire(cur);
}

} // namespace

void art_node_insert_and_unlock(ArtNodeBase *node, ArtNodeBase *parent,
                                  uint8_t parent_key, uint8_t new_key,
                                  ArtNodeBase *new_val,
                                  bool &need_restart_out) {
  // Compact when compact_count has saturated but live count has
  // headroom (cleared slots can't be reused — see `insert_compact`);
  // grow otherwise. Thresholds match the reference.
  switch (node->node_type()) {
  case ArtNodeType::N4: {
    auto *n = static_cast<ArtNode4 *>(node);
    uint16_t cc = n->compact_count.load(cpp::MemoryOrder::RELAXED);
    uint16_t live = n->count.load(cpp::MemoryOrder::RELAXED);
    if (cc == 4 && live <= 3) {
      insert_compact<ArtNode4, ArtNodeType::N4>(n, parent, parent_key, new_key,
                                                  new_val, need_restart_out);
      return;
    }
    insert_grow<ArtNode4, ArtNode16, ArtNodeType::N16>(
        n, parent, parent_key, new_key, new_val, need_restart_out);
    return;
  }
  case ArtNodeType::N16: {
    auto *n = static_cast<ArtNode16 *>(node);
    uint16_t cc = n->compact_count.load(cpp::MemoryOrder::RELAXED);
    uint16_t live = n->count.load(cpp::MemoryOrder::RELAXED);
    if (cc == 16 && live <= 14) {
      insert_compact<ArtNode16, ArtNodeType::N16>(n, parent, parent_key,
                                                    new_key, new_val,
                                                    need_restart_out);
      return;
    }
    insert_grow<ArtNode16, ArtNode48, ArtNodeType::N48>(
        n, parent, parent_key, new_key, new_val, need_restart_out);
    return;
  }
  case ArtNodeType::N48: {
    auto *n = static_cast<ArtNode48 *>(node);
    uint16_t cc = n->compact_count.load(cpp::MemoryOrder::RELAXED);
    uint16_t live = n->count.load(cpp::MemoryOrder::RELAXED);
    if (cc == 48 && live != 48) {
      insert_compact<ArtNode48, ArtNodeType::N48>(n, parent, parent_key,
                                                    new_key, new_val,
                                                    need_restart_out);
      return;
    }
    insert_grow<ArtNode48, ArtNode256, ArtNodeType::N256>(
        n, parent, parent_key, new_key, new_val, need_restart_out);
    return;
  }
  case ArtNodeType::N256: {
    // Terminal variant: in-place insert always succeeds.
    auto *n = static_cast<ArtNode256 *>(node);
    (void)n->insert(new_key, new_val);
    n->write_unlock();
    return;
  }
  }
  __builtin_trap();
}

//===----------------------------------------------------------------------===//
//  Remove shrink dispatch
//===----------------------------------------------------------------------===//

namespace {

template <class CurNode, class SmallerNode, ArtNodeType SmallerTag>
void remove_and_shrink(CurNode *cur, ArtNodeBase *parent, uint8_t parent_key,
                        uint8_t rm_key, bool &need_restart_out) {
  // At the root `force = true` (no parent to relock); below the root
  // `force = false`, and `false` return means the per-type shrink
  // threshold tripped — fall through to the shrink path.
  if (cur->remove(rm_key, parent == nullptr)) {
    cur->write_unlock();
    return;
  }

  auto *smaller =
      make_node<SmallerNode, SmallerTag>(cur->level, cur->get_prefix());
  if (LIBC_UNLIKELY(smaller == nullptr)) {
    cur->write_unlock();
    need_restart_out = true;
    return;
  }

  if (!parent->write_lock_or_restart()) {
    g_va_tracker_art_domain.retire(smaller);
    cur->write_unlock();
    need_restart_out = true;
    return;
  }

  // Erase from `cur` BEFORE copying so the smaller node never carries
  // the entry being removed.
  (void)cur->remove(rm_key, true);
  cur->copy_to(smaller);
  // Linearisation point of the shrink: parent's child pointer flips from
  // `cur` to `smaller`. Pre-flip readers see `cur` (Crystalline-pinned
  // until grace); post-flip readers see `smaller`.
  art_node_change(parent, parent_key, smaller);

  parent->write_unlock();
  cur->write_unlock_obsolete();
  g_va_tracker_art_domain.retire(cur);
}

} // namespace

void art_node_remove_and_unlock(ArtNodeBase *node, uint8_t rm_key,
                                  ArtNodeBase *parent, uint8_t parent_key,
                                  bool &need_restart_out) {
  switch (node->node_type()) {
  case ArtNodeType::N4: {
    // N4 always erases in place; single-child collapse is the caller's
    // count == 2 responsibility (via `get_second_child` +
    // `add_prefix_before`).
    auto *n = static_cast<ArtNode4 *>(node);
    (void)n->remove(rm_key, false);
    n->write_unlock();
    return;
  }
  case ArtNodeType::N16: {
    remove_and_shrink<ArtNode16, ArtNode4, ArtNodeType::N4>(
        static_cast<ArtNode16 *>(node), parent, parent_key, rm_key,
        need_restart_out);
    return;
  }
  case ArtNodeType::N48: {
    remove_and_shrink<ArtNode48, ArtNode16, ArtNodeType::N16>(
        static_cast<ArtNode48 *>(node), parent, parent_key, rm_key,
        need_restart_out);
    return;
  }
  case ArtNodeType::N256: {
    remove_and_shrink<ArtNode256, ArtNode48, ArtNodeType::N48>(
        static_cast<ArtNode256 *>(node), parent, parent_key, rm_key,
        need_restart_out);
    return;
  }
  }
  __builtin_trap();
}

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL
