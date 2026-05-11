//===- art_node.cpp - ART node operations and grow/shrink dispatch -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-variant operations (insert / change / get_child / remove / copy_to /
// get_children) for the four ROWEX ART node types (Leis et al., ICDE 2013;
// Leis et al., DaMoN 2016), plus the runtime dispatch helpers and the
// grow/shrink scaffolding called by the tree-level insert / remove.
//
// Sections:
//   1. ArtNodeBase methods (lock primitives, prefix mgmt).
//   2. ArtNode4 (no SSE; 4-element scan).
//   3. ArtNode16 (SSE2 keysearch + null-child filter).
//   4. ArtNode48 (sparse 256→48 index).
//   5. ArtNode256 (direct 256-pointer array).
//   6. Dispatch helpers (runtime type switch).
//   6.5. Crystalline-W pinned descent helper.
//   7. insert_grow / insert_compact / art_node_insert_and_unlock.
//   8. remove_and_shrink / art_node_remove_and_unlock.
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
//  Section 1 - ArtNodeBase methods
//===----------------------------------------------------------------------===//

bool ArtNodeBase::write_lock_or_restart() {
  // ROWEX writer-acquire (Leis et al., DaMoN 2016, §"Writer Protocol").
  // Spin on the locked bit with PAUSE for hyperthread yield, then CAS
  // the version+lock word from (v, unlocked) → (v + 0b10, locked). An
  // OBSOLETE observation aborts upward: caller must restart from the
  // parent so it picks up the replacement node via the parent pointer.
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
  // Promote an existing snapshot into a held lock with one strong CAS.
  // Used by the descent path that has just read the version
  // optimistically and wants to convert "I observed version v" into "I
  // hold the lock at version v + 0b10". A stale snapshot, an observed
  // lock bit, or an observed obsolete bit all force the caller to
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
  // Build the new prefix on the stack, zero-pad the unstored tail,
  // then publish via a single 8-byte RELEASE store. A concurrent
  // ACQUIRE-load observes either the pre-update or the post-update
  // (prefix_count, bytes) pair — never a torn snapshot.
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
  // Path-compression collapse: fuse `node->prefix || key || this->prefix`
  // and publish into `this->prefix` via one 8-byte RELEASE store.
  //
  // The composed prefix has length np_count + 1 + p_count, of which we
  // can store only kArtMaxStoredPrefixLength bytes inline; bytes
  // beyond that survive as optimistic-prefix (resolved against a
  // descendant leaf's key on lookup).
  ArtPrefix p = get_prefix();
  ArtPrefix np = node->get_prefix();
  uint32_t prefix_copy_count =
      (np.prefix_count + 1) < kArtMaxStoredPrefixLength
          ? (np.prefix_count + 1)
          : kArtMaxStoredPrefixLength;

  // Shift `this`'s stored bytes right by `prefix_copy_count` to make
  // room for `node->prefix` plus the discriminating key byte. The
  // iteration goes high→low so the move stays in-place safe.
  uint32_t carry =
      (p.prefix_count < (kArtMaxStoredPrefixLength - prefix_copy_count))
          ? p.prefix_count
          : (kArtMaxStoredPrefixLength - prefix_copy_count);
  for (int i = static_cast<int>(carry) - 1; i >= 0; --i)
    p.prefix[prefix_copy_count + i] = p.prefix[i];

  // Lay down the head of the new prefix: as much of node->prefix as
  // fits in the reserved slots.
  uint32_t np_copy =
      (np.prefix_count < prefix_copy_count) ? np.prefix_count : prefix_copy_count;
  for (uint32_t i = 0; i < np_copy; ++i)
    p.prefix[i] = np.prefix[i];

  // The discriminating key byte lives at slot `prefix_copy_count - 1`
  // only when node->prefix is short enough that the byte still fits
  // inside the stored prefix window.
  if (np.prefix_count < kArtMaxStoredPrefixLength)
    p.prefix[prefix_copy_count - 1] = key;

  p.prefix_count += np.prefix_count + 1;
  prefix.store(p, cpp::MemoryOrder::RELEASE);
}

//===----------------------------------------------------------------------===//
//  Section 2 - ArtNode4 (no SSE; 4-element scan)
//===----------------------------------------------------------------------===//

bool ArtNode4::insert(uint8_t key, ArtNodeBase *child) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::RELAXED);
  if (cc == 4)
    return false;
  // Append-only publish sequence: RELEASE-store key, RELEASE-store
  // child, then RELEASE-bump `compact_count`. A reader's scan masked
  // by `compact_count` sees the new entry only after the bump's
  // RELEASE synchronises with its ACQUIRE load. ACQ_REL on `count`
  // pairs with reader-side `count` consumers.
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
  // Missing key on `change` is a caller-side invariant violation —
  // every `change` is preceded by a successful `get_child`.
  __builtin_trap();
}

ArtNodeBase *ArtNode4::get_child(uint8_t key) {
  // Scan all four slots (not just `[0, count)`): `count` decreases on
  // remove but `compact_count` does not, and the append-only invariant
  // means a slot with a non-null child carries a stable key byte.
  // The null-child filter ignores slots that `remove` has cleared.
  for (uint32_t i = 0; i < 4; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr && keys[i].load(cpp::MemoryOrder::ACQUIRE) == key)
      return c;
  }
  return nullptr;
}

bool ArtNode4::remove(uint8_t key, bool /*force*/) {
  // N4 ignores the force-shrink flag: it never threshold-shrinks.
  // Single-child collapse (the N4-specific transition) is decided at
  // the caller's count==2 site through `get_second_child` plus
  // `add_prefix_before`, not in `remove`.
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  for (uint16_t i = 0; i < cc; ++i) {
    ArtNodeBase *c = children[i].load(cpp::MemoryOrder::ACQUIRE);
    if (c != nullptr && keys[i].load(cpp::MemoryOrder::ACQUIRE) == key) {
      // Order: decrement live count first, then null the slot.
      // Mirroring the reference; readers tolerate both orderings under
      // the null-child filter, but the chosen order minimises the
      // window in which `count > live_population`.
      count.fetch_sub(1, cpp::MemoryOrder::ACQ_REL);
      children[i].store(nullptr, cpp::MemoryOrder::RELEASE);
      return true;
    }
  }
  __builtin_trap();
}

ArtNodeBase *ArtNode4::get_any_child() {
  // Prefer a leaf child: `art_node_get_any_child_tid` bottoms out as
  // soon as a leaf is found, so returning the first leaf saves
  // iterations down the rightmost any-child chain.
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
  // Insertion sort: O(n²) on n ≤ 4 is the obvious choice over any
  // comparison-based sort with setup cost.
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
//  Section 3 - ArtNode16 (SSE2 keysearch + null-child filter)
//===----------------------------------------------------------------------===//

bool ArtNode16::insert(uint8_t key, ArtNodeBase *child) {
  uint16_t cc = compact_count.load(cpp::MemoryOrder::RELAXED);
  if (cc == 16)
    return false;
  // Keys are stored sign-flipped (b ^ 0x80) so signed `_mm_cmpeq_epi8`
  // orders unsigned bytes correctly for range scans (Leis et al.,
  // ICDE 2013, §3.1).
  keys[cc].store(flip_sign(key), cpp::MemoryOrder::RELEASE);
  children[cc].store(child, cpp::MemoryOrder::RELEASE);
  compact_count.store(static_cast<uint16_t>(cc + 1),
                       cpp::MemoryOrder::RELEASE);
  count.fetch_add(1, cpp::MemoryOrder::ACQ_REL);
  return true;
}

cpp::Atomic<ArtNodeBase *> *ArtNode16::get_child_pos(uint8_t k) {
  // SSE2 keysearch (Leis et al., ICDE 2013, §3.1):
  //   * `_mm_set1_epi8` broadcasts the lookup byte to all 16 lanes.
  //   * `_mm_loadu_si128` reads the 16-byte stored-keys vector. The
  //     unaligned form is correct even though `keys[]` is 16-byte
  //     aligned (alignas inheritance from ArtNodeBase), because
  //     other compilers in this libc's matrix may not see the
  //     alignment carry through `cpp::Atomic<uint8_t>`.
  //   * `_mm_cmpeq_epi8` produces a per-lane all-ones / all-zeros
  //     byte mask; `_mm_movemask_epi8` collapses it to a 16-bit hit
  //     bitmap.
  //   * Masking by `(1 << compact_count) - 1` excludes slots beyond
  //     the publish frontier.
  __m128i cmp = _mm_cmpeq_epi8(
      _mm_set1_epi8(static_cast<char>(flip_sign(k))),
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(&keys[0])));
  uint16_t cc = compact_count.load(cpp::MemoryOrder::ACQUIRE);
  unsigned bitfield = static_cast<unsigned>(_mm_movemask_epi8(cmp)) &
                       ((1u << cc) - 1u);
  // Walk hits in ascending slot order via TZCNT. A null child means
  // `remove` cleared the slot — skip and continue.
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
  // Reader form of SSE keysearch. Mask by the full 16-bit window
  // (NOT by `compact_count`): the null-child filter re-validates
  // every hit, so an in-flight insert's pre-bump write is harmless,
  // and a post-RELEASE-bump observer sees a fully populated slot
  // because of the publish ordering in `insert`.
  //
  // The second `keys[pos]` ACQUIRE compare is load-bearing for ROWEX
  // correctness (Leis et al., DaMoN 2016, §"ROWEX Node16 SSE phantom
  // matches"): an SSE compare can latch onto a stale byte if the
  // slot is mid-write, and the explicit compare catches it.
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
  // Threshold-shrink hysteresis: signal the caller to drop into the
  // 16→4 shrink path when the post-decrement count would fall to 3.
  // `force = true` skips the threshold check (used at the root and
  // mid-shrink to push the entry through unconditionally).
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
//  Section 4 - ArtNode48 (sparse 256→48 index)
//===----------------------------------------------------------------------===//

ArtNode48::ArtNode48(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
    : ArtNodeBase(ArtNodeType::N48, lvl, pfx, pfx_len) {
  // Stamp all 256 index entries to the empty marker. RELAXED suffices
  // because the node is not yet reachable from any reader.
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
  // Two-store publish (Leis et al., DaMoN 2016, §"Node48"):
  // populate the child slot first, then RELEASE-store the index byte
  // pointing at it. The second store is the linearisation point —
  // any reader that observes a non-empty `child_index[key]` is
  // guaranteed to observe the populated `children[idx]`.
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
  // Threshold-shrink hysteresis: drop into the 48→16 shrink path at
  // count == 12 (one below the threshold so the post-decrement
  // population is 11).
  uint16_t live = count.load(cpp::MemoryOrder::ACQUIRE);
  if (live == 12 && !force)
    return false;
  uint8_t idx = child_index[key].load(cpp::MemoryOrder::ACQUIRE);
  if (LIBC_UNLIKELY(idx == kArtNode48EmptyMarker))
    __builtin_trap();
  // Order matters: null the child pointer FIRST, then clear the
  // index byte. A reader that snapped the pre-clear `child_index[key]`
  // then loads the now-null child pointer and returns null — which is
  // the pre-state-correct linearised outcome. The subsequent index
  // store short-circuits later readers.
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
    // Guard against `end == 255` causing `i++` to wrap.
    if (i == 255)
      break;
  }
}

//===----------------------------------------------------------------------===//
//  Section 5 - ArtNode256 (direct 256-pointer array)
//===----------------------------------------------------------------------===//

bool ArtNode256::insert(uint8_t key, ArtNodeBase * child) {
  // Node256 is the terminal variant — single-store insert; no
  // overflow check. `count` tracks live (non-null) entries.
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
  // Threshold-shrink hysteresis: 256→48 at count == 37.
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

//===----------------------------------------------------------------------===//
//  Section 6 - Dispatch helpers (runtime type switch)
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
  // Descend until a tagged leaf is encountered or the chain bottoms
  // out (transient empty tree). The any-child preference for leaves
  // in `get_any_child` keeps this loop short — typically one or two
  // hops to a leaf for a non-trivial tree.
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
  // `getSecondChild` is a Node4-only operation — it backs the
  // single-child-collapse path that only fires on N4. Any other type
  // reaching here is a caller invariant violation.
  if (LIBC_UNLIKELY(node->node_type() != ArtNodeType::N4))
    __builtin_trap();
  return static_cast<ArtNode4 *>(node)->get_second_child(excluded_key);
}

void art_node_get_children(ArtNodeBase *node, uint8_t start, uint8_t end,
                            ArtKV *out, uint32_t &out_count) {
  // Each per-type `get_children` populates its own nested `KV` array
  // with the same `(k, child)` layout as `ArtKV`. Copying field-by-
  // field avoids reinterpret_cast and keeps the type system happy.
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
//  Section 6.5 - Crystalline-W pinned descent helper
//===----------------------------------------------------------------------===//

namespace {

// Closure passed through `CrystallineDomain::protect()`'s generalised
// overload (Nikolaev and Ravindran, PLDI 2024, §4.2 Fig. 10). The
// thunk is invoked either by the calling thread on the fast path or
// by a foreign helper thread when the slow path engages; the parent
// kept alive by the active-chain CAS scan provides the anchor.
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
//  Section 7 - Insert grow / compact dispatch
//===----------------------------------------------------------------------===//

namespace {

// Grow `cur` (CurNode) to the next-larger variant `BiggerNode`. In-
// place insert is tried first; only on structural fullness do we
// allocate a fresh bigger node, copy live children, install the new
// entry, and atomically swap the parent's child pointer.
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
    // Chunk-pool OOM is a benign condition: drop the writer lock and
    // signal the caller to restart from the root. The restart will
    // eventually surface `-ENOMEM` at the public API boundary if the
    // pool remains exhausted.
    cur->write_unlock();
    need_restart_out = true;
    return;
  }
  cur->copy_to(bigger);
  (void)bigger->insert(new_key, new_val);

  // Acquire the parent lock to publish the new child pointer.
  // Failing means another writer is restructuring the parent; retire
  // the freshly built node through Crystalline (it was never reachable
  // so no grace period is logically required, but routing through the
  // domain keeps the slot-pool accounting consistent) and restart.
  if (!parent->write_lock_or_restart()) {
    g_va_tracker_art_domain.retire(bigger);
    cur->write_unlock();
    need_restart_out = true;
    return;
  }

  // Linearisation point of the grow: parent's child pointer flips
  // from `cur` to `bigger`. Concurrent readers either see the old
  // node (still valid, will be retired) or the new one.
  art_node_change(parent, parent_key, bigger);
  parent->write_unlock();

  cur->write_unlock_obsolete();
  g_va_tracker_art_domain.retire(cur);
}

// In-place compact when `compact_count` has saturated but live
// `count` is below capacity (the append-only invariant means cleared
// slots cannot be reused without rebuilding). Same type in / out.
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
  // Per-variant dispatch. Each branch decides between in-place
  // compact (same type, fresh node) and grow (next-larger type)
  // based on the (compact_count, live count) pair. The exact
  // thresholds come from the reference implementation; the compact
  // path fires when `compact_count` saturates while live `count` has
  // headroom — i.e., many slots have been cleared by `remove`.
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
    // Node256 is terminal; in-place insert always succeeds.
    auto *n = static_cast<ArtNode256 *>(node);
    (void)n->insert(new_key, new_val);
    n->write_unlock();
    return;
  }
  }
  __builtin_trap();
}

//===----------------------------------------------------------------------===//
//  Section 8 - Remove shrink dispatch
//===----------------------------------------------------------------------===//

namespace {

// Try an in-place remove first; on threshold-shrink return false,
// allocate the next-smaller variant, copy survivors, swap into the
// parent, and retire the old node through Crystalline.
template <class CurNode, class SmallerNode, ArtNodeType SmallerTag>
void remove_and_shrink(CurNode *cur, ArtNodeBase *parent, uint8_t parent_key,
                        uint8_t rm_key, bool &need_restart_out) {
  // First attempt: in-place remove. At the root, `force = true`
  // (no parent to lock) so the call always succeeds. Below the root,
  // `force = false`; a false return means the count hit the per-type
  // shrink threshold and we must fall through to the shrink path.
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

  // Apply the erase to `cur` under the held parent lock, then copy
  // the (now post-erase) survivors into the smaller variant. Order
  // matters: erase-before-copy ensures the smaller node never carries
  // the entry being removed.
  (void)cur->remove(rm_key, true);
  cur->copy_to(smaller);
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
    // N4 always erases in place. The single-child-collapse decision
    // (the N4-specific shrink transition) is made at the caller's
    // count == 2 site via `get_second_child` plus `add_prefix_before`,
    // not here.
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
