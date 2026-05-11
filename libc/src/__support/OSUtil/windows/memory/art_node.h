//===- art_node.h - ART node types for ROWEX adaptive radix tree -*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal layouts for the four ART internal-node variants (Leis et al.,
/// ICDE 2013): `ArtNode4`, `ArtNode16`, `ArtNode48`, `ArtNode256`, plus
/// the shared `ArtNodeBase` carrying the lock+version word and the
/// path-compression prefix.
///
/// Synchronisation is ROWEX (Leis et al., DaMoN 2016). Readers issue
/// single ACQUIRE loads and never restart; writers serialise via the
/// per-node lock word and acquire parent-then-child. Append-only slot
/// discipline on Node4 / Node16 keeps SSE keysearch correct in the
/// presence of concurrent writers: a slot, once written, retains its
/// (key, child) pair for the life of the node, and a child cleared by
/// `remove` leaves its key byte intact. The matching null-child filter
/// in `get_child` catches stale SSE bitmap hits.
///
/// Reclamation is Crystalline-W (Nikolaev and Ravindran, PLDI 2024) via
/// `g_va_tracker_art_domain`: every internal node inherits
/// `CrystallineNode` so a retired node's body remains observable to
/// pinned readers until the domain's grace period elapses.
///
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

// Forward declaration of the inner-index leaf type. ART traffics in
// `Arena *` only as opaque tagged pointers and never dereferences one.
struct Arena;

/// Node-type tag encoded in the high two bits of the lock word.
/// Values match the paper's per-variant ordering (Leis et al., ICDE
/// 2013, §3): N4 (≤4 children), N16 (5..16), N48 (17..48), N256
/// (≥37 by grow / ≥17 by shrink hysteresis).
enum class ArtNodeType : uint8_t {
  N4 = 0,
  N16 = 1,
  N48 = 2,
  N256 = 3,
};

/// Stored-prefix capacity for ART's hybrid path compression. Four
/// bytes is the pessimistic fragment held inline on every node;
/// longer shared paths are validated optimistically against a
/// descendant leaf's full key (Leis et al., ICDE 2013, §"Path
/// Compression").
inline constexpr uint32_t kArtMaxStoredPrefixLength = 4;

/// Path-compression prefix carried by every internal node.
///
/// Sized to exactly 8 bytes so the publish point of
/// `ArtNodeBase::set_prefix` / `add_prefix_before` is a single atomic
/// store — a reader sees either the pre-store or post-store snapshot,
/// never a torn `(count, bytes)` pair.
///
/// The `alignas(8)` is load-bearing for the atomic. `cpp::Atomic<T>`
/// in this libc inherits the underlying type's alignment rather than
/// the platform's natural atomic alignment, so without the explicit
/// `alignas` the 8-byte load would either trip `-Watomic-alignment`
/// or fall back to an out-of-line `cmpxchg` on some build configs.
struct alignas(8) ArtPrefix {
  uint32_t prefix_count = 0;
  uint8_t prefix[kArtMaxStoredPrefixLength] = {};
};

static_assert(sizeof(ArtPrefix) == 8,
              "ArtPrefix must be 8 bytes (atomic prefix update precondition)");
static_assert(alignof(ArtPrefix) == 8,
              "ArtPrefix must be 8-byte aligned for atomic load/store");

/// Common header for every ART internal node.
///
/// Layout (64 bytes, one cache line):
///
/// \code
///   offset  field                          size
///   ------  -----------------------------  ----
///        0  CrystallineNode (base)          24    // retire link + era
///       24  typeVersionLockObsolete          8    // ROWEX lock word
///       32  prefix                           8    // path compression
///       40  level                            4    // key-byte depth
///       44  count                            2    // live children
///       46  compact_count                    2    // append frontier
///       48  node_canary                      8    // heap-spray detector
///       56  (padding)                        8
/// \endcode
///
/// Lock word `typeVersionLockObsolete` packs four fields:
///   * bits [63:62] node type tag (N4=0, N16=1, N48=2, N256=3).
///   * bits [61:2]  60-bit version counter, monotonically increasing.
///   * bit  [1]     locked (writer holds the node).
///   * bit  [0]     obsolete (node has been replaced by grow/shrink).
///
/// Lock is `fetch_add(0b10)` (sets the lock bit, bumps version);
/// unlock is also `fetch_add(0b10)` (clears the lock bit, bumps
/// version); `write_unlock_obsolete` does `fetch_add(0b11)` (clears
/// lock, sets obsolete, bumps version). The lock and obsolete bits
/// are testable from a plain ACQUIRE load — no RMW required on the
/// reader path (Leis et al., DaMoN 2016).
///
/// The class enforces these invariants:
///   * `count` is the number of live (non-null) children; updated
///     under the writer lock with ACQ_REL.
///   * `compact_count` is the append-only frontier on Node4 / Node16:
///     once a slot is written it stays written for the node's life,
///     so a concurrent reader either sees the slot empty or fully
///     populated. Decreasing it is never legal.
///   * `level` is logically immutable but not `const` — the slot is
///     memset-cleared on Crystalline retire and re-stamped on the
///     next `make_node`, and a `const` qualifier would let the
///     compiler cache values across the recycle boundary.
struct alignas(64) ArtNodeBase
    : public ::LIBC_NAMESPACE::concurrent::CrystallineNode {
  // [0..19] Intrusive Crystalline-W runtime fields emitted directly so
  // ArtNodeBase is standard-layout. The 4-byte slot at [20..23] is
  // unused — `typeVersionLockObsolete` is 8-aligned and lands at
  // offset 24, leaving a natural 4-byte pad after batch_link.
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
  /// ROWEX lock + version word. Initialised to `0b100` (version 1,
  /// unlocked, not obsolete); `set_type` then folds the type tag in
  /// via `fetch_add`.
  cpp::Atomic<uint64_t> typeVersionLockObsolete{0b100ULL};

  /// Path-compression prefix; updated via single 8-byte atomic store.
  cpp::Atomic<ArtPrefix> prefix{};

  /// Key-byte depth at which this node sits in the tree. See class
  /// doc for why this is not `const`.
  uint32_t level;

  /// Number of live (non-null) child entries. Decremented on `remove`.
  /// 16-bit atomic loads are single-copy atomic on x86-64.
  cpp::Atomic<uint16_t> count{0};

  /// Append-only slot population frontier. `insert` reserves the
  /// next slot, RELEASE-stores `(key, child)`, then RELEASE-bumps
  /// `compact_count`. Never decremented; shrink copies live children
  /// into a fresh node and retires the old one through Crystalline.
  cpp::Atomic<uint16_t> compact_count{0};

  /// Per-slot heap-spray detector. Derived at allocation from
  /// `(partition_secret, class_id, chunk_id, slot_idx)` and validated
  /// in `art_node_free` before any chunk-descriptor dereference.
  ///
  /// Plain (non-atomic) `uint64_t`: written exactly once after
  /// construction by the allocator under the bitmap-acquire single-
  /// writer guarantee, re-read by the FreeFn after the Crystalline
  /// grace period elapses, so reader and writer never overlap.
  /// Re-stamped during fork reinit when `partition_secret` rotates
  /// (Zone 0b publish at priority 36 → ART at priority 39).
  uint64_t node_canary = 0;

  // ---- ROWEX lock-word helpers ----

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

  /// ACQUIRE-load of the lock word. Reader-side entry point for the
  /// ROWEX validate-after-read protocol; pairs with `write_unlock`'s
  /// RELEASE-store on the writer side.
  ///
  /// Not `const`: `cpp::Atomic::load` is non-const in this libc, so
  /// every method that issues an atomic load takes a mutable
  /// receiver. The receiver is logically read-only and callers
  /// holding a `const ArtNodeBase *` need `const_cast` — safe because
  /// the underlying atomic words are designed for concurrent reads.
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

  /// Spin-acquire the writer lock with `_mm_pause` backoff.
  ///
  /// \returns true on success with the lock held; false if the node
  ///          became obsolete during the spin (caller restarts from
  ///          the parent).
  [[nodiscard]] bool write_lock_or_restart();

  /// Promote a previously-read version snapshot into a held writer
  /// lock via a single strong CAS.
  ///
  /// \param[in,out] version_inout snapshot on entry; updated to the
  ///                              post-lock version on success.
  /// \returns true on success; false if the version moved or the
  ///          node became obsolete since the snapshot.
  [[nodiscard]] bool lock_version_or_restart(uint64_t &version_inout);

  /// Validate that the lock word has not changed since the snapshot
  /// `start_read` was taken.
  ///
  /// Used by `art_remove` to commit a "not-found" answer derived from
  /// a multi-step optimistic read of a writer-mutable node (prefix
  /// check plus `get_child`). `art_lookup` is pure ROWEX wait-free
  /// and `art_insert` validates via `lock_version_or_restart` at its
  /// lock points, so neither needs this path.
  ///
  /// \returns true if it is safe to commit the read; false if a
  ///          writer touched the node during the optimistic window.
  [[nodiscard]] LIBC_INLINE bool read_unlock_or_restart(uint64_t start_read) {
    return start_read ==
           typeVersionLockObsolete.load(cpp::MemoryOrder::ACQUIRE);
  }

  /// Release the writer lock and bump the version counter. Preserves
  /// the type and obsolete bits.
  LIBC_INLINE void write_unlock() {
    typeVersionLockObsolete.fetch_add(0b10ULL, cpp::MemoryOrder::ACQ_REL);
  }

  /// Release the writer lock and stamp the node obsolete in a single
  /// atomic publish. Next reader to ACQUIRE-load the word observes
  /// the obsolete bit and unwinds.
  LIBC_INLINE void write_unlock_obsolete() {
    typeVersionLockObsolete.fetch_add(0b11ULL, cpp::MemoryOrder::ACQ_REL);
  }

  /// Replace the prefix bytes via a single 8-byte atomic store.
  /// Stores the first `min(length, kArtMaxStoredPrefixLength)` bytes
  /// in-line; `prefix_count` records the true length so longer paths
  /// are resolved optimistically against a descendant leaf's key.
  void set_prefix(const uint8_t *bytes, uint32_t length);

  /// Single-child-collapse helper (Leis et al., ICDE 2013, §"Path
  /// Compression"). Fuses `node`'s prefix, the discriminating `key`
  /// byte, and `this`'s existing prefix into `this`'s prefix via one
  /// 8-byte atomic store.
  ///
  /// \pre Caller holds the writer lock on both `this` and \p node.
  void add_prefix_before(ArtNodeBase *node, uint8_t key);

private:
  /// Compose the type tag into the initial `0b100` lock-word state at
  /// construction. RELAXED is sufficient — the new node is not yet
  /// reachable from any reader.
  LIBC_INLINE void set_type(ArtNodeType t) {
    typeVersionLockObsolete.fetch_add(type_to_version_bits(t),
                                       cpp::MemoryOrder::RELAXED);
  }
};

static_assert(sizeof(ArtNodeBase) == 64,
              "ArtNodeBase must occupy exactly one cache line");
static_assert(alignof(ArtNodeBase) == 64,
              "ArtNodeBase must be cache-line aligned");

/// Leaf-pointer tag bit. An `ArtNodeBase *` with bit 63 set encodes
/// a tagged `Arena *` leaf; with bit 63 clear it points at an
/// internal node. x86-64 user VAs occupy only the low 47 bits, so a
/// valid `Arena *` always has bit 63 clear and the tag is
/// unambiguous.
inline constexpr uint64_t kArtLeafTagBit = 1ULL << 63;

[[nodiscard]] LIBC_INLINE bool art_is_leaf(const ArtNodeBase *p) {
  return (reinterpret_cast<uintptr_t>(p) & kArtLeafTagBit) != 0;
}

[[nodiscard]] LIBC_INLINE ArtNodeBase *art_set_leaf(Arena *arena) {
  // A null `Arena *` is still a legal tagged leaf — the remove path
  // checks tagged-but-null after decode and treats it as a miss.
  uintptr_t bits = reinterpret_cast<uintptr_t>(arena);
  return reinterpret_cast<ArtNodeBase *>(bits | kArtLeafTagBit);
}

[[nodiscard]] LIBC_INLINE Arena *art_get_leaf(const ArtNodeBase *p) {
  LIBC_ASSERT(art_is_leaf(p) && "art_get_leaf: not a leaf");
  return reinterpret_cast<Arena *>(reinterpret_cast<uintptr_t>(p) &
                                    ~kArtLeafTagBit);
}

/// Smallest node variant: header plus 4 keys and 4 child pointers.
///
/// Layout:
/// \code
///   [ ArtNodeBase header (64 B) ][ keys[4] ][ children[4] ]
/// \endcode
///
/// Lookup is a 4-element scan; SSE would not pay off at this fan-out
/// and the reference avoids it. Append-only slot discipline (Leis
/// et al., DaMoN 2016, §"ROWEX"): inserts populate the slot at index
/// `compact_count`, RELEASE-store key and child, then RELEASE-bump
/// `compact_count`. `remove` nulls the child pointer in place but
/// leaves the key byte, so the post-removal slot remains scannable
/// and the null-child filter in `get_child` ignores it.
///
/// Invariants:
///   * `compact_count` ∈ [count, 4]; equality holds when no slot
///     has been nulled by `remove`.
///   * Once `compact_count` reaches 4 the node is structurally
///     full; `insert` returns false and the caller routes through
///     grow (N4 to N16) or in-place compact.
struct alignas(64) ArtNode4 : public ArtNodeBase {
  cpp::Atomic<uint8_t> keys[4]{};
  cpp::Atomic<ArtNodeBase *> children[4]{};

  ArtNode4(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
      : ArtNodeBase(ArtNodeType::N4, lvl, pfx, pfx_len) {}
  ArtNode4(uint32_t lvl, const ArtPrefix &pfx)
      : ArtNodeBase(ArtNodeType::N4, lvl, pfx) {}

  /// Append-only insert under writer lock.
  ///
  /// \returns true on success; false when the node is structurally
  ///          full (`compact_count == 4`) and the caller must grow.
  bool insert(uint8_t key, ArtNodeBase *child);

  /// In-place replace of an existing key's child via a single RELEASE
  /// store. \pre Caller holds the writer lock.
  void change(uint8_t key, ArtNodeBase *new_val);

  /// ROWEX point lookup. The null-child filter is mandatory because
  /// `remove` clears the child pointer but leaves the key byte (the
  /// append-only invariant).
  [[nodiscard]] ArtNodeBase *get_child(uint8_t key);

  /// Erase the entry for \p key. The \p force parameter is the
  /// reference's force-shrink flag and is ignored for N4 — N4 never
  /// shrinks via threshold; single-child collapse decisions are made
  /// at the caller's count==2 site (Leis et al., ICDE 2013).
  bool remove(uint8_t key, bool force);

  /// Return any non-null child, preferring leaves so
  /// `art_node_get_any_child_tid` bottoms out faster.
  [[nodiscard]] ArtNodeBase *get_any_child();

  /// Surviving (child, key) pair when only one entry remains after a
  /// remove. \p excluded_key is the key just removed.
  struct SecondChild {
    ArtNodeBase *child;
    uint8_t key;
  };
  [[nodiscard]] SecondChild get_second_child(uint8_t excluded_key);

  /// Copy live children into a different node-type instance (called
  /// during grow N4 -> N16 or in-place compact).
  template <class NODE> void copy_to(NODE *bigger);

  /// (key, child) tuple yielded by `get_children`.
  struct KV {
    uint8_t k;
    ArtNodeBase *child;
  };

  /// Populate `out_kv[0..*out_count)` with the children whose key
  /// byte lies in `[start, end]`, sorted ascending.
  void get_children(uint8_t start, uint8_t end, KV *out_kv,
                     uint32_t &out_count);
};

/// Mid-fan-out variant: header plus 16 keys and 16 child pointers.
///
/// Layout:
/// \code
///   [ ArtNodeBase header (64 B) ][ keys[16] ][ children[16] ]
/// \endcode
///
/// Point lookup uses SSE2 keysearch (Leis et al., ICDE 2013, §3.1):
/// `_mm_cmpeq_epi8` broadcasts the lookup byte across an XMM register
/// and compares against the 16 stored keys in one instruction;
/// `_mm_movemask_epi8` collapses the 128-bit byte mask into a 16-bit
/// hit vector; `__builtin_ctz` extracts the first hit. The mask is
/// constrained to populated slots via `(1 << compact_count) - 1`.
///
/// Two invariants make SSE keysearch correct under ROWEX
/// (Leis et al., DaMoN 2016):
///   * Append-only slots: a (key, child) pair, once written, never
///     moves — so the SSE compare's "byte view" of `keys[]` is
///     coherent regardless of concurrent writers.
///   * Null-child filter: `remove` clears the child pointer but
///     leaves the key byte. A stale SSE hit can therefore match a
///     freed slot; every hit is re-validated by `child != nullptr`
///     and a second `keys[pos]` comparison.
///
/// Stored bytes are sign-flipped (`b ^ 0x80`) so the signed
/// `_mm_cmpeq_epi8` correctly orders unsigned bytes for range scans;
/// the flip is invisible above `flip_sign`.
struct alignas(64) ArtNode16 : public ArtNodeBase {
  cpp::Atomic<uint8_t> keys[16]{};
  cpp::Atomic<ArtNodeBase *> children[16]{};

  ArtNode16(uint32_t lvl, const uint8_t *pfx, uint32_t pfx_len)
      : ArtNodeBase(ArtNodeType::N16, lvl, pfx, pfx_len) {}
  ArtNode16(uint32_t lvl, const ArtPrefix &pfx)
      : ArtNodeBase(ArtNodeType::N16, lvl, pfx) {}

  /// Sign-flip a byte for signed SSE comparison ordering.
  [[nodiscard]] LIBC_INLINE static constexpr uint8_t flip_sign(uint8_t b) {
    return static_cast<uint8_t>(b ^ 0x80U);
  }

  /// Count trailing zeros in a 16-bit hit mask. Single TZCNT on
  /// Haswell+ (the x86-64-v3 baseline). Defined for x > 0; callers
  /// gate on a non-zero mask.
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

  /// Locate the storage slot for key \p k. Used by `change` /
  /// `remove` to overwrite the child pointer in place under the
  /// writer lock. Returns null if no live slot matches.
  [[nodiscard]] cpp::Atomic<ArtNodeBase *> *get_child_pos(uint8_t k);
};

/// Empty-slot sentinel for the Node48 256-byte sparse index. The
/// 48 valid slot indices live in `[0, 48)`; the marker `48` is the
/// first out-of-range value.
inline constexpr uint8_t kArtNode48EmptyMarker = 48;

/// Sparse-fanout variant: 256-byte direct index into 48 child slots.
///
/// Layout:
/// \code
///   [ ArtNodeBase header (64 B) ][ child_index[256] ][ children[48] ]
/// \endcode
///
/// `child_index[byte] == kArtNode48EmptyMarker` means no child for
/// that byte; any other value is the slot in `children[]` holding
/// the child pointer. Lookup is a single byte indirection plus a
/// pointer load; no comparison, no SIMD.
///
/// Insert publishes in two RELEASE stores (Leis et al., DaMoN 2016,
/// §"Node48"):
///   1. Write `children[compact_count]` with the new child.
///   2. Publish via `child_index[byte] = compact_count`.
/// A concurrent reader either sees the pre-state index value
/// (empty marker or older mapping) or the post-state index value
/// pointing at a fully-populated slot. The second store is the
/// linearisation point.
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

/// Largest variant: direct 256-element child pointer array, where
/// the index *is* the key byte.
///
/// Layout:
/// \code
///   [ ArtNodeBase header (64 B) ][ children[256] ]
/// \endcode
///
/// Insert / lookup / remove are a single atomic store or load on
/// `children[byte]`; no separate index, no SIMD. Node256 is the
/// terminal variant — `insert` never overflows.
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

// ---- Free-function dispatch over the runtime node type ----

/// Reader-side child lookup. Wait-free under an outer Crystalline
/// pin. Implementations dispatch by `node_type()` to the per-variant
/// `get_child`.
[[nodiscard]] ArtNodeBase *art_node_get_child(ArtNodeBase *node, uint8_t key);

/// Writer-side in-place child replacement. \pre Caller holds the
/// writer lock on \p node.
void art_node_change(ArtNodeBase *node, uint8_t key, ArtNodeBase *new_val);

/// Return any non-null child of \p node, preferring leaves so that
/// `art_node_get_any_child_tid` bottoms out faster.
[[nodiscard]] ArtNodeBase *art_node_get_any_child(ArtNodeBase *node);

/// Descend through any-child pointers until a tagged leaf is reached.
/// Used by the loadKey path to recover the optimistic-prefix tail
/// bytes from a real stored key.
[[nodiscard]] Arena *art_node_get_any_child_tid(ArtNodeBase *node);

/// Surviving (child, key) pair for the post-erase single-child case.
/// Defined on Node4 only; other node types trap. Used by the path-
/// compression collapse in `art_remove`.
[[nodiscard]] ArtNode4::SecondChild
art_node_get_second_child(ArtNodeBase *node, uint8_t excluded_key);

/// (key, child) tuple uniformly carried across node types. Mirrors
/// each node's nested `KV` struct so the dispatch helper can copy
/// without per-type plumbing.
struct ArtKV {
  uint8_t k;
  ArtNodeBase *child;
};

/// Sorted-by-key children in `[start, end]` from \p node into \p out.
/// \p out capacity must be at least 256.
void art_node_get_children(ArtNodeBase *node, uint8_t start, uint8_t end,
                            ArtKV *out, uint32_t &out_count);

/// Reservation slot used by linear descents (lookup / insert /
/// remove). Callers ping-pong between this and `kArtPinSlotDescendB`
/// across iterations so the slot being advanced on the next
/// `pinned_get_child` is never the slot pinning the current parent.
/// This eliminates the iteration-2 drain-vs-parent UAF window in the
/// Crystalline-W fast path.
inline constexpr uint32_t kArtPinSlotDescendA = 0;
inline constexpr uint32_t kArtPinSlotDescendB = 1;

/// Base index of the depth-indexed reservation slots used by
/// `art_walk_range`'s DFS. The stack frame at depth `d` pins its
/// node on `kArtPinSlotWalkBase + (d - 1)`. The linear-descent slots
/// A/B and the walk slots overlap by design; the two operations
/// never co-occur on the same thread.
inline constexpr uint32_t kArtPinSlotWalkBase = 0;

/// Wait-free pinned child fetch.
///
/// Routes a single `art_node_get_child` step through Crystalline-W's
/// `protect()` generalised overload (Nikolaev and Ravindran, PLDI
/// 2024, §4.2 Fig. 10). The thunk performs the dispatch over the
/// four node variants — N4 scan, N16 SSE bitmap, N48 index byte,
/// N256 direct array — and the era-stability protocol inside
/// `protect()` proves the loaded child is safe to dereference. The
/// fast path is at most 16 CAS attempts; the bounded `slow_path` is
/// the wait-free fallback (PLDI 2024, §5 Lemma 5.2 / 5.3).
///
/// The ART tree root is a process-lifetime sentinel allocated once
/// at `art_index_init` and never retired, so an unpinned load of
/// `tree.root` is safe. Every retirable internal node must be
/// reached through `pinned_get_child`, which establishes the
/// reservation on \p hr_idx.
///
/// \param parent  current node; kept alive by the caller's existing
///                pin (the slow-path handoff uses it as the
///                Crystalline parent for PROTECT2 / active-chain
///                scan when fast-path convergence fails).
/// \param hr_idx  hazard-reservation slot to advance on this step.
///
/// Without this helper a plain `art_node_get_child` would not
/// advance the reservation and a foreign thread's `live_count → 0`
/// could free a node that a stale-era reader is still dereferencing.
[[nodiscard]] ArtNodeBase *pinned_get_child(ArtNodeBase *parent,
                                             uint8_t key_byte,
                                             uint32_t hr_idx);

/// Insert \p new_val under \p new_key into \p node, handling grow
/// to the next variant (N4→N16→N48→N256) or in-place compact when
/// `compact_count` has saturated but live `count` is below capacity.
///
/// \pre Writer lock held on \p node at entry.
/// \post Writer lock released before return (either via plain
///       `write_unlock` on success or via `write_unlock_obsolete`
///       and Crystalline retire after grow).
/// \param[out] need_restart_out set when the caller must restart
///                              from the root (parent lock
///                              contention or chunk-pool OOM).
void art_node_insert_and_unlock(ArtNodeBase *node, ArtNodeBase *parent,
                                 uint8_t parent_key, uint8_t new_key,
                                 ArtNodeBase *new_val, bool &need_restart_out);

/// Remove the entry under \p rm_key from \p node, handling per-type
/// hysteresis shrink (Node256→Node48 at count 37, Node48→Node16 at
/// 12, Node16→Node4 at 3).
///
/// \pre Writer lock held on \p node at entry.
/// \post Writer lock released before return.
/// \param[out] need_restart_out set when the caller must restart
///                              from the root.
void art_node_remove_and_unlock(ArtNodeBase *node, uint8_t rm_key,
                                 ArtNodeBase *parent, uint8_t parent_key,
                                 bool &need_restart_out);

/// Crystalline-W FreeFn for retired ART nodes. Validates
/// `node_canary`, scrubs the body, and returns the slot to the
/// per-type chunk pool. Per the reclamation discipline, the FreeFn
/// is metadata-only: it never invokes any `nt_pal::*` syscall.
void art_node_free(ArtNodeBase *node);

/// Per-domain retire frequency for ART nodes (Nikolaev and
/// Ravindran, PLDI 2024, §3). Lower values bound metadata leakage at
/// the cost of higher retire-batch overhead; higher values amortise
/// retire cost but raise steady-state hold. Four is the operating
/// point chosen for the ART traversal shape, where retires cluster
/// around grow/shrink boundaries rather than streaming.
inline constexpr uint32_t kArtRetireFreq = 4;

/// The Crystalline-W domain governing ART internal-node body
/// reclamation. Each ART node inherits `CrystallineNode` and rides
/// this domain through retire / grace / FreeFn.
extern ::LIBC_NAMESPACE::concurrent::CrystallineDomain<
    ArtNodeBase, &art_node_free, kArtRetireFreq>
    g_va_tracker_art_domain;

} // namespace va_tracker
} // namespace windows

namespace concurrent {

/// BatchLinkCodec specialisation routing `ArtNodeBase` retire-link
/// codes through the chunk allocator's per-type slot encoding. Method
/// bodies live in `art_node_alloc.cpp` so they can reach the
/// anonymous-namespace `state_for_type` helper; the declaration here
/// lets every TU instantiating `CrystallineDomain<ArtNodeBase>` see
/// the codec.
template <>
struct BatchLinkCodec<::LIBC_NAMESPACE::windows::va_tracker::ArtNodeBase> {
  static uint32_t encode(CrystallineNode *n) noexcept;
  static CrystallineNode *decode(uint32_t code) noexcept;
};

} // namespace concurrent
} // namespace LIBC_NAMESPACE_DECL

#endif
