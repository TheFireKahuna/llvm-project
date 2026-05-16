//===- art_node.h - ART node types for ROWEX adaptive radix tree -*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Layouts for the four ART internal-node variants and the shared base
// (Leis et al., ICDE 2013; DaMoN 2016). Synchronisation is ROWEX:
// readers issue single ACQUIRE loads and never restart; writers
// serialise via the per-node lock word and acquire parent-then-child.
//
// Append-only slot discipline (N4 / N16): a slot, once written, retains
// its (key, child) pair for the life of the node; `remove` nulls the
// child pointer but leaves the key byte. This keeps the SSE keysearch
// vector coherent under concurrent writers; the matching null-child
// filter in every `get_child` catches stale SSE hits.
//
// Internal nodes inherit `CrystallineNode` and are reclaimed via
// `g_va_tracker_art_domain` (Nikolaev and Ravindran, PLDI 2024) so a
// retired node's body remains observable to pinned readers across the
// grace period.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_NODE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ART_NODE_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

// ART traffics in `Arena *` only as an opaque tagged pointer.
struct Arena;

// Encoded in the high two bits of the lock word; paper §3 ordering.
enum class ArtNodeType : uint8_t {
  N4 = 0,
  N16 = 1,
  N48 = 2,
  N256 = 3,
};

// Cap for the inline path-compression fragment; longer shared paths are
// validated optimistically against a descendant leaf's full key (paper
// §"Path Compression").
inline constexpr uint32_t kArtMaxStoredPrefixLength = 4;

// Sized + aligned to exactly 8 bytes so `set_prefix` /
// `add_prefix_before` publish via one atomic store — a reader never
// sees a torn `(count, bytes)` pair. `cpp::Atomic<T>` in this libc
// inherits T's alignment rather than the platform's natural atomic
// alignment, so the explicit `alignas(8)` is load-bearing — without it
// the 8-byte load trips `-Watomic-alignment` or falls back to an
// out-of-line `cmpxchg` on some build configs.
struct alignas(8) ArtPrefix {
  uint32_t prefix_count = 0;
  uint8_t prefix[kArtMaxStoredPrefixLength] = {};
};

static_assert(sizeof(ArtPrefix) == 8, "atomic single-store precondition");
static_assert(alignof(ArtPrefix) == 8, "atomic load/store alignment");

// Common header for every ART internal node. Exactly one cache line.
//
// Lock word `typeVersionLockObsolete` packs:
//   [63:62] node-type tag.
//   [61:2]  60-bit version counter, monotonically increasing.
//   [1]     locked (writer holds the node).
//   [0]     obsolete (node has been replaced by grow/shrink).
//
// Lock = unlock = `fetch_add(0b10)` (toggles locked, bumps version);
// `write_unlock_obsolete` = `fetch_add(0b11)` (clears lock, sets
// obsolete, bumps version). The reader path tests both bits from a
// plain ACQUIRE load — no RMW required (paper §"Writer Protocol").
struct alignas(64) ArtNodeBase
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // Intrusive Crystalline-W fields emitted inline to keep ArtNodeBase
  // standard-layout. Occupies [0..19]; [20..23] is natural pad before
  // the 8-aligned `typeVersionLockObsolete` at offset 24.
  LIBC_CRYSTALLINE_NODE_FIELDS(ArtNodeBase);

protected:
  ArtNodeBase(ArtNodeType t, uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
      : level(lvl) {
    set_type(t);
    set_prefix(pfx, pfx_len);
  }
  ArtNodeBase(ArtNodeType t, uint32_t lvl, const ArtPrefix &pfx)
      : prefix(pfx), level(lvl) {
    set_type(t);
  }
  ArtNodeBase(const ArtNodeBase &) = delete;
  ArtNodeBase(ArtNodeBase &&) = delete;

public:
  // Initial state 0b100 = version 1, unlocked, not obsolete; `set_type`
  // then folds the type tag into the high bits.
  cpp::Atomic<uint64_t> typeVersionLockObsolete{0b100ULL};

  cpp::Atomic<ArtPrefix> prefix{};

  // Not `const`: the slot is memset-cleared on Crystalline retire and
  // re-stamped on the next `make_node`; `const` would let the compiler
  // cache values across the recycle boundary.
  uint32_t level;

  // Live (non-null) child count. 16-bit loads are single-copy atomic
  // on x86-64.
  cpp::Atomic<uint16_t> count{0};

  // Append-only publish frontier on N4 / N16: insert reserves the next
  // slot, RELEASE-stores (key, child), then RELEASE-bumps this. Never
  // decremented — shrink copies live children into a fresh node and
  // retires the old one through Crystalline.
  cpp::Atomic<uint16_t> compact_count{0};

  // Heap-spray detector derived at allocation from
  // (partition_secret, class_id, chunk_id, slot_idx); validated in
  // `art_node_free` before any chunk-descriptor deref. Plain
  // `uint64_t` because writer (allocator under bitmap-acquire) and
  // reader (FreeFn post grace) never overlap. Re-stamped on fork when
  // `partition_secret` rotates.
  uint64_t node_canary = 0;

  [[nodiscard]] LIBC_INLINE static constexpr bool is_locked(uint64_t v) {
    return (v & 0b10ULL) != 0;
  }
  [[nodiscard]] LIBC_INLINE static constexpr bool is_obsolete(uint64_t v) {
    return (v & 0b01ULL) != 0;
  }
  [[nodiscard]] LIBC_INLINE static constexpr ArtNodeType decode_type(uint64_t v) {
    return static_cast<ArtNodeType>(v >> 62);
  }
  [[nodiscard]] LIBC_INLINE static constexpr uint64_t
  type_to_version_bits(ArtNodeType t) {
    return static_cast<uint64_t>(t) << 62;
  }

  // Reader-side entry point for the ROWEX validate-after-read protocol;
  // pairs with `write_unlock`'s RELEASE-store on the writer side. Not
  // `const` because `cpp::Atomic::load` is non-const in this libc — a
  // `const ArtNodeBase *` caller needs `const_cast`, which is safe
  // since the atomic words are designed for concurrent reads.
  [[nodiscard]] LIBC_INLINE uint64_t read_version() {
    return typeVersionLockObsolete.load(cpp::MemoryOrder::ACQUIRE);
  }

  [[nodiscard]] LIBC_INLINE ArtNodeType node_type() {
    return decode_type(typeVersionLockObsolete.load(cpp::MemoryOrder::ACQUIRE));
  }

  [[nodiscard]] LIBC_INLINE uint32_t get_level() const { return level; }

  [[nodiscard]] LIBC_INLINE uint32_t get_count() {
    return count.load(cpp::MemoryOrder::ACQUIRE);
  }

  [[nodiscard]] LIBC_INLINE ArtPrefix get_prefix() {
    return prefix.load(cpp::MemoryOrder::ACQUIRE);
  }

  // Spin-acquire with `_mm_pause` backoff. Returns false if the node
  // became obsolete during the spin — caller restarts from the parent.
  [[nodiscard]] bool write_lock_or_restart();

  // Promote a previously-read version snapshot into a held writer lock
  // via one strong CAS. `version_inout` is updated to the post-lock
  // version on success. Returns false on stale snapshot, observed lock,
  // or observed obsolete.
  [[nodiscard]] bool lock_version_or_restart(uint64_t &version_inout);

  // Validate the lock word is unchanged since `start_read`. Used by
  // `art_remove` to commit a "not-found" answer derived from a
  // multi-step optimistic read (prefix check + `get_child`). `lookup`
  // is pure wait-free and `insert` validates at its lock points, so
  // neither needs this path.
  [[nodiscard]] LIBC_INLINE bool read_unlock_or_restart(uint64_t start_read) {
    return start_read ==
           typeVersionLockObsolete.load(cpp::MemoryOrder::ACQUIRE);
  }

  // Releases the writer lock and bumps the version; preserves type and
  // obsolete bits.
  LIBC_INLINE void write_unlock() {
    typeVersionLockObsolete.fetch_add(0b10ULL, cpp::MemoryOrder::ACQ_REL);
  }

  // Releases the writer lock and stamps obsolete in one atomic publish.
  LIBC_INLINE void write_unlock_obsolete() {
    typeVersionLockObsolete.fetch_add(0b11ULL, cpp::MemoryOrder::ACQ_REL);
  }

  // Stores `min(length, kArtMaxStoredPrefixLength)` bytes inline;
  // `prefix_count` records the true length for optimistic-prefix
  // resolution against a descendant leaf's key.
  void set_prefix(const uint8_t *bytes, uint32_t length);

  // Single-child-collapse helper. Fuses `node->prefix || key ||
  // this->prefix` into `this->prefix` via one 8-byte atomic store.
  // Caller must hold the writer lock on both nodes.
  void add_prefix_before(ArtNodeBase *node, uint8_t key);

private:
  // RELAXED is sufficient — the new node is not yet reader-reachable.
  LIBC_INLINE void set_type(ArtNodeType t) {
    typeVersionLockObsolete.fetch_add(type_to_version_bits(t),
                                       cpp::MemoryOrder::RELAXED);
  }
};

static_assert(sizeof(ArtNodeBase) == 64, "one cache line");
static_assert(alignof(ArtNodeBase) == 64, "cache-line aligned");

// Bit 63 distinguishes tagged `Arena *` leaves from internal-node
// pointers. x86-64 user VAs occupy only the low 47 bits, so a valid
// `Arena *` always has bit 63 clear.
inline constexpr uint64_t kArtLeafTagBit = 1ULL << 63;

[[nodiscard]] LIBC_INLINE bool art_is_leaf(const ArtNodeBase *p) {
  return (reinterpret_cast<uintptr_t>(p) & kArtLeafTagBit) != 0;
}

[[nodiscard]] LIBC_INLINE ArtNodeBase *art_set_leaf(Arena *arena) {
  // A null `Arena *` is a legal tagged leaf — the remove path checks
  // tagged-but-null after decode and treats it as a miss.
  uintptr_t bits = reinterpret_cast<uintptr_t>(arena);
  return reinterpret_cast<ArtNodeBase *>(bits | kArtLeafTagBit);
}

[[nodiscard]] LIBC_INLINE Arena *art_get_leaf(const ArtNodeBase *p) {
  LIBC_ASSERT(art_is_leaf(p) && "art_get_leaf: not a leaf");
  return reinterpret_cast<Arena *>(reinterpret_cast<uintptr_t>(p) &
                                    ~kArtLeafTagBit);
}

// Smallest variant: 4 keys + 4 child pointers. Lookup is a 4-element
// scan; SSE doesn't pay at this fan-out. `compact_count` ∈ [count, 4];
// `insert` returns false when it saturates and the caller must grow or
// compact.
struct alignas(64) ArtNode4 : public ArtNodeBase {
  cpp::Atomic<uint8_t> keys[4]{};
  cpp::Atomic<ArtNodeBase *> children[4]{};

  ArtNode4(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
      : ArtNodeBase(ArtNodeType::N4, lvl, pfx, pfx_len) {}
  ArtNode4(uint32_t lvl, const ArtPrefix &pfx)
      : ArtNodeBase(ArtNodeType::N4, lvl, pfx) {}

  // Returns false when full; caller grows to N16.
  bool insert(uint8_t key, ArtNodeBase *child);

  // Caller must hold the writer lock.
  void change(uint8_t key, ArtNodeBase *new_val);

  [[nodiscard]] ArtNodeBase *get_child(uint8_t key);

  // `force` is the reference's force-shrink flag; ignored for N4 (N4
  // never threshold-shrinks — single-child collapse is decided at the
  // caller's count==2 site).
  bool remove(uint8_t key, bool force);

  // Prefers leaves so `art_node_get_any_child_tid` bottoms out faster.
  [[nodiscard]] ArtNodeBase *get_any_child();

  // Surviving (child, key) pair after a remove leaves a single entry.
  struct SecondChild {
    ArtNodeBase *child;
    uint8_t key;
  };
  [[nodiscard]] SecondChild get_second_child(uint8_t excluded_key);

  template <class NODE> void copy_to(NODE *bigger);

  struct KV {
    uint8_t k;
    ArtNodeBase *child;
  };

  // Populates `out_kv[0..out_count)` with children whose key byte lies
  // in `[start, end]`, sorted ascending.
  void get_children(uint8_t start, uint8_t end, KV *out_kv,
                     uint32_t &out_count);
};

// 16 keys + 16 child pointers; SSE2 keysearch (paper §3.1):
// `_mm_cmpeq_epi8` produces a per-lane byte mask, `_mm_movemask_epi8`
// collapses it to a 16-bit hit vector. Stored bytes are sign-flipped
// (`b ^ 0x80`) so signed `_mm_cmpeq_epi8` orders unsigned bytes
// correctly for range scans; the flip is invisible above `flip_sign`.
struct alignas(64) ArtNode16 : public ArtNodeBase {
  cpp::Atomic<uint8_t> keys[16]{};
  cpp::Atomic<ArtNodeBase *> children[16]{};

  ArtNode16(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
      : ArtNodeBase(ArtNodeType::N16, lvl, pfx, pfx_len) {}
  ArtNode16(uint32_t lvl, const ArtPrefix &pfx)
      : ArtNodeBase(ArtNodeType::N16, lvl, pfx) {}

  [[nodiscard]] LIBC_INLINE static constexpr uint8_t flip_sign(uint8_t b) {
    return static_cast<uint8_t>(b ^ 0x80U);
  }

  // Single TZCNT on the x86-64-v3 baseline. Caller gates on x > 0.
  [[nodiscard]] LIBC_INLINE static unsigned ctz(uint16_t x) {
    return static_cast<unsigned>(__builtin_ctz(static_cast<uint32_t>(x)));
  }

  bool insert(uint8_t key, ArtNodeBase *child);
  void change(uint8_t key, ArtNodeBase *new_val);
  [[nodiscard]] ArtNodeBase *get_child(uint8_t key);
  bool remove(uint8_t key, bool force);
  [[nodiscard]] ArtNodeBase *get_any_child();

  template <class NODE> void copy_to(NODE *bigger);

  struct KV {
    uint8_t k;
    ArtNodeBase *child;
  };
  void get_children(uint8_t start, uint8_t end, KV *out_kv,
                     uint32_t &out_count);

  // Locates the storage slot for `change` / `remove`. Returns null on
  // miss. Caller holds the writer lock.
  [[nodiscard]] cpp::Atomic<ArtNodeBase *> *get_child_pos(uint8_t k);
};

// 48 valid slot indices live in [0, 48); 48 itself is the empty marker.
inline constexpr uint8_t kArtNode48EmptyMarker = 48;

// 256-byte direct index → 48 child slots. Lookup is one byte
// indirection plus a pointer load.
//
// Two-store publish (paper §"Node48"): populate
// `children[compact_count]`, then RELEASE-store `child_index[key]`. The
// second store is the linearisation point; a reader observing a
// non-empty index entry is guaranteed to observe the populated child.
struct alignas(64) ArtNode48 : public ArtNodeBase {
  cpp::Atomic<uint8_t> child_index[256]{};
  cpp::Atomic<ArtNodeBase *> children[48]{};

  ArtNode48(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len);
  ArtNode48(uint32_t lvl, const ArtPrefix &pfx);

  bool insert(uint8_t key, ArtNodeBase *child);
  void change(uint8_t key, ArtNodeBase *new_val);
  [[nodiscard]] ArtNodeBase *get_child(uint8_t key);
  bool remove(uint8_t key, bool force);
  [[nodiscard]] ArtNodeBase *get_any_child();

  template <class NODE> void copy_to(NODE *bigger);

  struct KV {
    uint8_t k;
    ArtNodeBase *child;
  };
  void get_children(uint8_t start, uint8_t end, KV *out_kv,
                     uint32_t &out_count);
};

// Terminal variant: index is the key byte. All ops are a single atomic
// store / load on `children[byte]`; `insert` never overflows.
struct alignas(64) ArtNode256 : public ArtNodeBase {
  cpp::Atomic<ArtNodeBase *> children[256]{};

  ArtNode256(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
      : ArtNodeBase(ArtNodeType::N256, lvl, pfx, pfx_len) {}
  ArtNode256(uint32_t lvl, const ArtPrefix &pfx)
      : ArtNodeBase(ArtNodeType::N256, lvl, pfx) {}

  bool insert(uint8_t key, ArtNodeBase *child);
  void change(uint8_t key, ArtNodeBase *new_val);
  [[nodiscard]] ArtNodeBase *get_child(uint8_t key);
  bool remove(uint8_t key, bool force);
  [[nodiscard]] ArtNodeBase *get_any_child();

  template <class NODE> void copy_to(NODE *bigger);

  struct KV {
    uint8_t k;
    ArtNodeBase *child;
  };
  void get_children(uint8_t start, uint8_t end, KV *out_kv,
                     uint32_t &out_count);
};

//===----------------------------------------------------------------------===//
// Runtime-type-dispatched free functions
//===----------------------------------------------------------------------===//

// Wait-free under an outer Crystalline pin. Dispatches by `node_type()`.
[[nodiscard]] ArtNodeBase *art_node_get_child(ArtNodeBase *node, uint8_t key);

// Caller must hold the writer lock on `node`.
void art_node_change(ArtNodeBase *node, uint8_t key, ArtNodeBase *new_val);

[[nodiscard]] ArtNodeBase *art_node_get_any_child(ArtNodeBase *node);

// Descend through any-child pointers until a tagged leaf is reached.
// Used by the loadKey path to recover optimistic-prefix tail bytes from
// a real stored key.
[[nodiscard]] Arena *art_node_get_any_child_tid(ArtNodeBase *node);

// Node4-only — other node types trap. Drives path-compression collapse
// in `art_remove`.
[[nodiscard]] ArtNode4::SecondChild
art_node_get_second_child(ArtNodeBase *node, uint8_t excluded_key);

// Mirrors each node's nested `KV` so the dispatch helper can copy
// without per-type plumbing.
struct ArtKV {
  uint8_t k;
  ArtNodeBase *child;
};

// `out` capacity must be at least 256.
void art_node_get_children(ArtNodeBase *node, uint8_t start, uint8_t end,
                            ArtKV *out, uint32_t &out_count);

// Linear descent ping-pongs between slots A and B so the slot being
// advanced on the next `pinned_get_child` is never the slot pinning the
// current parent. Closes the iteration-2 drain-vs-parent UAF window in
// the Crystalline-W fast path.
inline constexpr uint32_t kArtPinSlotDescendA = 0;
inline constexpr uint32_t kArtPinSlotDescendB = 1;

// `art_walk_range`'s DFS pins frame at depth `d` on
// `kArtPinSlotWalkBase + (d - 1)`. Overlap with the descent A/B slots
// is intentional — the two operations never co-occur on one thread.
inline constexpr uint32_t kArtPinSlotWalkBase = 0;

// Routes a single child fetch through Crystalline-W `protect()` (paper
// §4.2 Fig. 10). The tree root is a process-lifetime sentinel never
// retired, so the unpinned `tree.root` load is safe; every retirable
// internal node must be reached through this helper. Without it a plain
// `art_node_get_child` would not advance the reservation and a foreign
// thread's `live_count → 0` could free a node a stale-era reader is
// still dereferencing.
//
// `parent` is the caller's currently-pinned anchor; the slow path uses
// it as the Crystalline parent for PROTECT2 / active-chain scan.
// `hr_idx` is the reservation slot to advance on this step.
[[nodiscard]] ArtNodeBase *pinned_get_child(ArtNodeBase *parent,
                                             uint8_t key_byte,
                                             uint32_t hr_idx);

// Handles grow (N4→N16→N48→N256) or in-place compact when
// `compact_count` saturates while live `count` has headroom. Writer
// lock held on entry; released before return (plain `write_unlock` on
// in-place success, `write_unlock_obsolete` + Crystalline retire after
// a successful grow / compact). `need_restart_out` is set on parent
// lock contention or chunk-pool OOM.
void art_node_insert_and_unlock(ArtNodeBase *node, ArtNodeBase *parent,
                                 uint8_t parent_key, uint8_t new_key,
                                 ArtNodeBase *new_val, bool &need_restart_out);

// Handles per-type hysteresis shrink (N256→N48 at 37, N48→N16 at 12,
// N16→N4 at 3). Writer lock held on entry; released before return.
void art_node_remove_and_unlock(ArtNodeBase *node, uint8_t rm_key,
                                 ArtNodeBase *parent, uint8_t parent_key,
                                 bool &need_restart_out);

// Crystalline-W FreeFn. Validates `node_canary`, scrubs the body, and
// returns the slot to the per-type chunk pool. Metadata-only — never
// invokes any `nt_pal::*` syscall.
void art_node_free(ArtNodeBase *node);

// Retire frequency for the ART domain. ART retires cluster around
// grow / shrink boundaries rather than streaming, so 4 amortises the
// retire-batch cost without inflating steady-state hold (paper §3).
inline constexpr uint32_t kArtRetireFreq = 4;

// MaxIdx covers two reservation consumers sharing one slot space:
//   * Linear descent: slots 0 and 1 (DescendA / DescendB ping-pong).
//   * `art_walk_range` DFS: depth ∈ [1, kArtKeyLen + 2] = [1, 10], so
//     stack slots span [0, 9].
// Audited max index = 9 ⇒ MaxIdx = 10.
inline constexpr uint32_t kArtMaxIdx = 10;

extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    ArtNodeBase, &art_node_free, kArtRetireFreq, kArtMaxIdx>
    g_va_tracker_art_domain;

} // namespace va_tracker
} // namespace windows

namespace concurrent {

// Bodies live in `art_node_alloc.cpp` so they can reach the
// anonymous-namespace `state_for_type` helper; the forward declaration
// here is what every `CrystallineDomain<ArtNodeBase>` instantiation
// sees.
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::ArtNodeBase> {
  static uint32_t encode(CrystallineNode *n) noexcept;
  static CrystallineNode *decode(uint32_t code) noexcept;
};

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif
