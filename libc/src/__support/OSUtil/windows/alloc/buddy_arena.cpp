//===-- Lock-free buddy chunk broker (Layer 2) implementation -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/buddy_arena.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/alloc/pagemap.h"
#include "src/__support/OSUtil/windows/alloc/primitives/canary_seed.h"
#include "src/__support/OSUtil/windows/alloc/primitives/init_latch.h"
#include "src/__support/OSUtil/windows/alloc/primitives/occupancy_bitmap.h"
#include "src/__support/OSUtil/windows/alloc/sealed_va_publisher.h"
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/OSUtil/windows/libc_fork_registry.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/nt_pal/pal_state.h"
#include "src/__support/OSUtil/windows/nt_pal/placeholder.h"
#include "src/__support/OSUtil/windows/pcb_init_access.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/libc_assert.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/stdint_proxy.h"
#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

//===----------------------------------------------------------------------===//
// NBALLOC node status word (Marotta et al., arXiv:1804.03436, 2018, Fig. 1)
//===----------------------------------------------------------------------===//
//
// Five bits packed into a uint8_t; one independently CAS-able status word per
// tree node. Layout:
//
//   bit 4: OCC          - this node was handed out at its own level.
//   bit 3: COAL_LEFT    - FREENODE in progress in left subtree.
//   bit 2: COAL_RIGHT   - FREENODE in progress in right subtree.
//   bit 1: OCC_LEFT     - some fully-occupied descendant in left subtree.
//   bit 0: OCC_RIGHT    - some fully-occupied descendant in right subtree.
//
// COAL bits are deliberately NOT in BUSY: a coalescing node looks free to
// ancestor occupancy bookkeeping, allowing the parent to coalesce in turn.
// UNMARK detects "TRYALLOC raced through us" by observing COAL cleared on
// retry.

namespace {

constexpr uint8_t NB_OCC = 0x10;
constexpr uint8_t NB_COAL_LEFT = 0x08;
constexpr uint8_t NB_COAL_RIGHT = 0x04;
constexpr uint8_t NB_OCC_LEFT = 0x02;
constexpr uint8_t NB_OCC_RIGHT = 0x01;
constexpr uint8_t NB_BUSY = NB_OCC | NB_OCC_LEFT | NB_OCC_RIGHT;

[[nodiscard]] LIBC_INLINE constexpr bool nb_is_free(uint8_t v) {
  return (v & NB_BUSY) == 0;
}

// `child` is the array index of the node we came from on the upward walk
// (left = 2*parent, right = 2*parent+1); the low bit selects the side.
[[nodiscard]] LIBC_INLINE constexpr unsigned nb_mod2(size_t child) {
  return static_cast<unsigned>(child & 1U);
}

// TRYALLOC's per-parent action: clear the COAL bit on the side we arrived
// from, zeroing any in-flight release marker on that branch.
[[nodiscard]] LIBC_INLINE constexpr uint8_t
nb_clean_coal(uint8_t v, size_t child) {
  return static_cast<uint8_t>(v & ~(NB_COAL_LEFT >> nb_mod2(child)));
}

[[nodiscard]] LIBC_INLINE constexpr uint8_t nb_mark(uint8_t v, size_t child) {
  return static_cast<uint8_t>(v | (NB_OCC_LEFT >> nb_mod2(child)));
}

// UNMARK's per-parent action: clear OCC + COAL on our side in one mask so
// undoing the stamp and clearing the in-progress marker is a single CAS.
[[nodiscard]] LIBC_INLINE constexpr uint8_t
nb_unmark(uint8_t v, size_t child) {
  return static_cast<uint8_t>(
      v & ~((NB_OCC_LEFT | NB_COAL_LEFT) >> nb_mod2(child)));
}

[[nodiscard]] LIBC_INLINE constexpr bool nb_is_coal(uint8_t v, size_t child) {
  return (v & (NB_COAL_LEFT >> nb_mod2(child))) != 0;
}

// Sibling-occupied probe. FREENODE's upward sweep stops at the first
// non-coalescable sibling.
[[nodiscard]] LIBC_INLINE constexpr bool nb_is_occ_sibling(uint8_t v,
                                                            size_t child) {
  // child=left (mod2=0) -> sibling OCC is OCC_RIGHT (bit 0).
  // child=right (mod2=1) -> sibling OCC is OCC_LEFT (bit 1).
  return (v & (NB_OCC_RIGHT << nb_mod2(child))) != 0;
}

[[nodiscard]] LIBC_INLINE constexpr bool nb_is_coal_sibling(uint8_t v,
                                                             size_t child) {
  return (v & (NB_COAL_RIGHT << nb_mod2(child))) != 0;
}

// floor(log2(n)); root level 0 at n=1. Single defined-on-zero `lzcnt`.
[[nodiscard]] LIBC_INLINE constexpr unsigned nb_level_of(size_t n) {
  return n == 0 ? 0u : 63u - static_cast<unsigned>(__builtin_clzll(n));
}

[[nodiscard]] LIBC_INLINE constexpr size_t nb_first_at_level(unsigned level) {
  return static_cast<size_t>(1) << level;
}

// VA offset (bytes within the partition) of node `n`. Each level halves the
// per-node span, so node bytes = partition_bytes >> level.
[[nodiscard]] LIBC_INLINE size_t
nb_va_offset_of(size_t n, size_t partition_bytes) {
  unsigned L = nb_level_of(n);
  size_t off_in_level = n - nb_first_at_level(L);
  size_t node_bytes = partition_bytes >> L;
  return off_in_level * node_bytes;
}

// Crystalline retire cadence. Phase-A scan must see a retire chain longer
// than eligible-reservation slots so a slow reader can never reach a slot
// already past quiescence. 8 matches va_substrate at this libc's thread
// counts.
constexpr uint32_t kArenaRetireFreq = 8;

// 2^17 = 131 072 slots at 64 B each (8 MiB pool). Multi-pool growth deferred.
constexpr size_t kBuddyDescPoolShift = 17;
constexpr size_t kBuddyDescPoolCapacity = static_cast<size_t>(1)
                                          << kBuddyDescPoolShift;

// Immutable arena state (partition base/bytes, tree base, pool base/cap,
// canary secret) lives in PCB Zone 0; only the per-arena live counter /
// generation / init latch below are mutable, and none are lookup-redirecting.
// Value-init so the file-scope global stays constant-initialized under
// -Werror=-Wglobal-constructors (`cpp::Atomic`'s defaulted ctor leaves the
// value indeterminate).
struct alignas(64) ArenaState {
  ::LIBC_NAMESPACE::internal::alloc_primitives::InitLatch tree_init{};
  cpp::Atomic<uint64_t> live_count{};
  cpp::Atomic<uint32_t> generation{};
};

ArenaState g_first_arena_state;

::LIBC_NAMESPACE::internal::alloc_primitives::AtomicBitmap<
    kBuddyDescPoolCapacity, /*trap_on_collision=*/true>
    g_desc_pool_occupancy;
cpp::Atomic<uint32_t> g_desc_pool_hint{0};

::LIBC_NAMESPACE::internal::alloc_primitives::InitLatch g_buddy_init;

// Both the forward declaration and definition live inside the anonymous
// namespace so the linker resolves them to the same internal-linkage symbol;
// defining the function at namespace scope would trip -Wundefined-internal.
void buddy_free_chunk_descriptor(BuddyChunkDescriptor *desc);

// MaxIdx = 1 sizes the (unused) reservation-slot space minimally — the
// arena domain has no `protect()` / `anchor()` call sites.
inline constexpr uint32_t kArenaDomainMaxIdx = 1;

::LIBC_NAMESPACE::concurrent::CrystallineDomain<BuddyChunkDescriptor,
                                                 &buddy_free_chunk_descriptor,
                                                 kArenaRetireFreq,
                                                 kArenaDomainMaxIdx>
    g_arena_domain;

// Pool VA reserved + committed eagerly during Tier A; slot storage is never
// released for process lifetime because Crystalline retire batches may carry
// stale slot indices that re-resolve against fresh allocations. The
// per-allocation generation bump in the pagemap entry (not pool reuse) is
// what distinguishes generations.

[[nodiscard]] BuddyChunkDescriptor *desc_pool_alloc(uint32_t &out_slot_idx) {
  auto *base = static_cast<BuddyChunkDescriptor *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_base());
  if (LIBC_UNLIKELY(base == nullptr))
    return nullptr;

  // Per-thread randomized starting word keeps concurrent allocators from
  // pounding the same bitmap word. RELAXED — no ordering dependency between
  // allocations.
  uint32_t start_word =
      g_desc_pool_hint.fetch_add(1, cpp::MemoryOrder::RELAXED);

  using Bitmap = decltype(g_desc_pool_occupancy);
  constexpr size_t WORDS = Bitmap::word_count;

  for (size_t attempt = 0; attempt < WORDS; ++attempt) {
    size_t w = (start_word + attempt) % WORDS;
    uint64_t bits = ~g_desc_pool_occupancy
                         .word_at<cpp::MemoryOrder::RELAXED>(w);
    while (bits) {
      // tzcnt is defined-on-zero on the pinned baseline, so the outer
      // `while (bits)` is the only termination check needed.
      unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
      size_t idx = w * 64u + bit;
      if (LIBC_UNLIKELY(idx >= kBuddyDescPoolCapacity)) {
        bits &= bits - 1;
        continue;
      }
      if (g_desc_pool_occupancy.try_acquire(idx)) {
        out_slot_idx = static_cast<uint32_t>(idx);
        return &base[idx];
      }
      bits &= bits - 1;
    }
  }
  return nullptr;
}

void desc_pool_free(BuddyChunkDescriptor *desc) {
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();
  auto *base = static_cast<BuddyChunkDescriptor *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_base());
  size_t idx = static_cast<size_t>(desc - base);
  if (LIBC_UNLIKELY(idx >= kBuddyDescPoolCapacity))
    __builtin_trap();
  // Wipe the descriptor body (including the intrusive Crystalline header)
  // so a stale wait-free reader decoding through this slot before the era
  // advances sees zero-init state, not the prior owner's chunk_base / canary.
  __builtin_memset(desc, 0, sizeof(*desc));
  g_desc_pool_occupancy.mark_dead(idx);
}

// Tree backing VA was reserved during Tier A; first allocation wins the
// commit race via the InitLatch, deferring the 128 KiB commit until the
// partition is actually exercised.
[[nodiscard]] bool arena_lazy_commit_tree() {
  if (LIBC_LIKELY(g_first_arena_state.tree_init.is_ready()))
    return true;
  if (g_first_arena_state.tree_init.try_begin()) {
    NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::commit_replace(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_tree_base(),
        kBuddyTreeBytes, PAGE_READWRITE);
    if (LIBC_UNLIKELY(!NT_SUCCESS(st)))
      __builtin_trap(); // OOM during arena bring-up is unrecoverable.
    g_first_arena_state.tree_init.publish_ready();
    return true;
  }
  g_first_arena_state.tree_init.wait_ready();
  return true;
}

// NBALLOC — paper Algs. 2-4. See per-phase inline comments below.
[[nodiscard]] size_t nb_try_alloc(cpp::Atomic<uint8_t> *tree, size_t n,
                                   unsigned target_level);
void nb_free_node(cpp::Atomic<uint8_t> *tree, size_t n, unsigned bound);
void nb_unmark(cpp::Atomic<uint8_t> *tree, size_t n, unsigned upper_bound);

[[nodiscard]] size_t nb_try_alloc(cpp::Atomic<uint8_t> *tree, size_t n,
                                   unsigned target_level) {
  // (1) Leaf claim — the linearizing CAS. Success: ACQ_REL publishes BUSY
  // (release) and synchronizes with any concurrent FREENODE Phase 2 store
  // (acquire). Failure: ACQUIRE — only need what the winning CAS published.
  uint8_t expected = 0;
  if (!tree[n].compare_exchange_strong(expected, NB_BUSY,
                                        cpp::MemoryOrder::ACQ_REL,
                                        cpp::MemoryOrder::ACQUIRE))
    return n; // Node taken — caller skips and retries.

  // (2) Ancestor walk. Each parent CAS publishes OCC_LEFT/OCC_RIGHT (other
  // allocators must observe partial occupancy before descending) and clears
  // COAL on our side (TRYALLOC wins over any in-flight release on this branch).
  size_t current = n;
  while (nb_level_of(current) > 0) {
    size_t child = current;
    current = current >> 1;
    uint8_t cur_val = tree[current].load(cpp::MemoryOrder::ACQUIRE);
    for (;;) {
      if (cur_val & NB_OCC) {
        // Ancestor already occupied — roll back our leaf claim. FREENODE
        // walks back down clearing the OCC stamp we set on the path up.
        nb_free_node(tree, n, nb_level_of(current));
        return current;
      }
      uint8_t new_val = nb_clean_coal(cur_val, child);
      new_val = nb_mark(new_val, child);
      if (tree[current].compare_exchange_weak(cur_val, new_val,
                                                cpp::MemoryOrder::ACQ_REL,
                                                cpp::MemoryOrder::ACQUIRE))
        break;
      // CAS failed; `cur_val` now holds the observed value — retry same
      // parent without reloading.
    }
  }
  (void)target_level;
  return 0;
}

void nb_free_node(cpp::Atomic<uint8_t> *tree, size_t n, unsigned bound) {
  // Phase 1: walk up setting COAL on our side of each ancestor. A
  // coalescing-marked branch looks free to higher allocators (COAL is not in
  // NB_BUSY) so siblings can coalesce up in turn, but TRYALLOC observes the
  // COAL marker and treats the branch as in-flight. Termination: a
  // non-coalescing occupied sibling means we cannot merge further on this
  // path.
  size_t current = n >> 1;
  size_t runner = n;
  while (nb_level_of(current) >= bound && current != 0) {
    uint8_t or_val = static_cast<uint8_t>(NB_COAL_LEFT >> nb_mod2(runner));
    uint8_t cur_val = tree[current].load(cpp::MemoryOrder::ACQUIRE);
    uint8_t old_val;
    for (;;) {
      uint8_t new_val = static_cast<uint8_t>(cur_val | or_val);
      old_val = cur_val;
      if (tree[current].compare_exchange_weak(cur_val, new_val,
                                                cpp::MemoryOrder::ACQ_REL,
                                                cpp::MemoryOrder::ACQUIRE))
        break;
    }
    if (nb_is_occ_sibling(old_val, runner) &&
        !nb_is_coal_sibling(old_val, runner))
      break;
    runner = current;
    if (current == 1)
      break;
    current = current >> 1;
  }

  // Phase 2: zero the released node. We hold the only logical reference —
  // RELEASE pairs with the ACQUIRE in TRYALLOC's leaf CAS to publish a free
  // leaf.
  tree[n].store(0, cpp::MemoryOrder::RELEASE);

  // Phase 3: walk back down clearing the OCC + COAL bits set in Phase 1.
  // UNMARK self-aborts if a concurrent TRYALLOC has already cleared our
  // COAL marker on this side — signal that the allocator handled the
  // bookkeeping for us.
  nb_unmark(tree, n, bound);
}

void nb_unmark(cpp::Atomic<uint8_t> *tree, size_t n, unsigned upper_bound) {
  size_t current = n;
  do {
    size_t child = current;
    current = current >> 1;
    if (current == 0)
      return;
    uint8_t cur_val = tree[current].load(cpp::MemoryOrder::ACQUIRE);
    for (;;) {
      if (!nb_is_coal(cur_val, child))
        return; // Concurrent TRYALLOC won the race — bail out.
      uint8_t new_val = nb_unmark(cur_val, child);
      if (tree[current].compare_exchange_weak(cur_val, new_val,
                                                cpp::MemoryOrder::ACQ_REL,
                                                cpp::MemoryOrder::ACQUIRE))
        break;
    }
  } while (nb_level_of(current) > upper_bound);
}

// NBALLOC's CAS chain scales only when concurrent allocators scatter their
// starting node across the target level. xorshift64* (Marsaglia, Journal of
// Statistical Software 8(14), 2003) — no syscall, no shared atomic.
LIBC_INLINE size_t random_start_at_level(unsigned level) {
  static thread_local uint64_t rng_state = 0;
  if (rng_state == 0) {
    // TLS-relative address XOR per-process cookie. Both cheap, no libc deps.
    rng_state = static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(&rng_state)) ^
                static_cast<uint64_t>(
                    ::LIBC_NAMESPACE::nt_pal::process_cookie());
    if (rng_state == 0)
      rng_state = 0x9E3779B97F4A7C15ULL; // golden-ratio fallback
  }
  // xorshift64* — Marsaglia, Journal of Statistical Software 8(14), 2003.
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;
  uint64_t r = rng_state * 0x2545F4914F6CDD1DULL;

  size_t first = nb_first_at_level(level);
  size_t span = first; // level holds `first` indices: [first, 2*first)
  return first + static_cast<size_t>(r % span);
}

[[nodiscard]] void *arena_alloc_at_level(unsigned level) {
  if (LIBC_UNLIKELY(!arena_lazy_commit_tree()))
    return nullptr;
  auto *tree = static_cast<cpp::Atomic<uint8_t> *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_tree_base());
  void *partition_base =
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_base();
  size_t partition_bytes =
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_bytes();
  size_t first = nb_first_at_level(level);
  size_t start = random_start_at_level(level);

  for (size_t scanned = 0; scanned < first; ++scanned) {
    size_t i = first + ((start - first + scanned) % first);
    uint8_t v = tree[i].load(cpp::MemoryOrder::ACQUIRE);
    if (!nb_is_free(v))
      continue;
    size_t failed = nb_try_alloc(tree, i, level);
    if (failed == 0) {
      size_t off = nb_va_offset_of(i, partition_bytes);
      return static_cast<char *>(partition_base) + off;
    }
    // Subtree-skip jump (skip the entire failed-ancestor subtree, NBALLOC
    // Alg. 2 line 16) is deliberately omitted — uncontended adjacent retries
    // are cheap and the skip math adds branch complexity without a measured
    // win. Bench under contention before reintroducing.
  }
  return nullptr;
}

void arena_free_chunk(void *chunk_base, size_t chunk_bytes) {
  auto *tree = static_cast<cpp::Atomic<uint8_t> *>(
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_tree_base());
  if (LIBC_UNLIKELY(tree == nullptr))
    __builtin_trap();
  void *partition_base =
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_base();
  size_t partition_bytes =
      ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_bytes();
  size_t off =
      static_cast<size_t>(static_cast<char *>(chunk_base) -
                          static_cast<char *>(partition_base));
  if (LIBC_UNLIKELY(off >= partition_bytes))
    __builtin_trap();
  unsigned shift = 63u - static_cast<unsigned>(__builtin_clzll(chunk_bytes));
  unsigned level = kBuddyTreeDepth - (shift - kBuddyMinShift);
  size_t leaf_off = off >> shift;
  size_t node = nb_first_at_level(level) + leaf_off;
  nb_free_node(tree, node, /*bound=*/0);
}

// Repair OCC/COAL bits along a zombie chunk's tree path. The parent may
// have died mid-`nb_free_node` Phase 1 with COAL bits set, leaf not yet
// zeroed, UNMARK descent never reached. Idempotent on already-clean state
// (the `cleaned == v` early exit) and necessary repair on partial state.
// Single-threaded post-fork — RELAXED throughout.
void clean_zombie_tree_path(cpp::Atomic<uint8_t> *tree, char *part_base,
                             size_t /*part_bytes*/, void *cb, size_t cs) {
  size_t off = static_cast<size_t>(static_cast<char *>(cb) - part_base);
  unsigned shift = 63u - static_cast<unsigned>(__builtin_clzll(cs));
  unsigned level = kBuddyTreeDepth - (shift - kBuddyMinShift);
  size_t leaf_off = off >> shift;
  size_t n = (size_t(1) << level) + leaf_off;

  tree[n].store(0, cpp::MemoryOrder::RELAXED);
  while (n > 1) {
    size_t child = n;
    n >>= 1;
    uint8_t v = tree[n].load(cpp::MemoryOrder::RELAXED);
    uint8_t mask = static_cast<uint8_t>((NB_OCC_LEFT | NB_COAL_LEFT) >>
                                         nb_mod2(child));
    uint8_t cleaned = static_cast<uint8_t>(v & ~mask);
    if (cleaned == v)
      break; // Path already clean above this point.
    tree[n].store(cleaned, cpp::MemoryOrder::RELAXED);
  }
}

// Crystalline-W FreeFn for both consumers. Runs only after every concurrent
// wait-free reader has crossed the era boundary, so it is safe to issue NT
// decommit / release syscalls and return the slot here.
void buddy_free_chunk_descriptor(BuddyChunkDescriptor *desc) {
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();

  // (1) Verify canary. Key is sealed in Zone 0 — an attacker with arbitrary
  // write cannot forge by rewriting a BSS seed. Mismatch = corruption,
  // wrong-class free, or forged tagged pointer.
  uintptr_t expected_canary =
      ::LIBC_NAMESPACE::internal::alloc_primitives::derive_canary(
          ::LIBC_NAMESPACE::g_pcb.zone0.buddy_arena_secret(),
          desc->chunk_base, desc->chunk_bytes);
  if (LIBC_UNLIKELY(desc->canary != expected_canary))
    __builtin_trap();

  // (2) Pagemap unregister is a no-op — pagemap OS pages stay committed for
  // process lifetime (snmalloc notify_using_readonly, Liétar et al., ISMM
  // 2019). Kept for symmetry with the `buddy_free_sized` call site.
  ::LIBC_NAMESPACE::windows::alloc::pagemap_unregister_range(
      desc->chunk_base, desc->chunk_bytes);

  // (3) Decommit. Path asymmetry follows the reservation shape:
  //   * BuddyDirect — chunk lives inside the partition's plain
  //     `MEM_RESERVE | MEM_WRITE_WATCH` VAD; plain MEM_DECOMMIT returns
  //     physical pages while VA stays reserved inside the partition.
  //   * HugeDirect — own placeholder per allocation; `decommit_preserve`
  //     keeps the placeholder (no WW armed at commit so sub-range release
  //     is permitted — see `nt_pal::placeholder` WW caveat), then
  //     `free_placeholder` returns VA to MEM_FREE.
  if (desc->consumer_tag ==
      static_cast<uint16_t>(VaChunkConsumer::HugeDirect)) {
    (void)::LIBC_NAMESPACE::nt_pal::decommit_preserve(desc->chunk_base,
                                                       desc->chunk_bytes);
    (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(desc->chunk_base);
  } else {
    (void)::LIBC_NAMESPACE::nt_pal::decommit_uncommitted(desc->chunk_base,
                                                          desc->chunk_bytes);
  }

  // (4) Safe to return the slot — pagemap entries were zeroed in the
  // `buddy_free_sized` call site before retire, so a wait-free reader
  // decoding a stale tagged pointer sees `(0, Empty)` and rejects the
  // address before reaching this slot.
  desc_pool_free(desc);
}

} // namespace

//===----------------------------------------------------------------------===//
// Public API — buddy_alloc / buddy_free_sized
//===----------------------------------------------------------------------===//

void *buddy_alloc(size_t size) {
  if (LIBC_UNLIKELY(!g_buddy_init.is_ready()))
    return nullptr;

  uint8_t shift = buddy_class_shift_for(size);
  if (LIBC_UNLIKELY(shift > kBuddyMaxShift))
    return buddy_alloc_huge(size);

  size_t chunk_bytes = buddy_class_bytes(shift);
  unsigned level = buddy_class_tree_level(shift);

  void *chunk_base = arena_alloc_at_level(level);
  if (LIBC_UNLIKELY(chunk_base == nullptr))
    return nullptr;

  // Defensive bounds check — tree-state corruption shows up here.
  {
    auto *part_base = static_cast<char *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_base());
    size_t part_bytes =
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_bytes();
    auto *cb = static_cast<char *>(chunk_base);
    if (cb < part_base || cb + chunk_bytes > part_base + part_bytes)
      __builtin_trap();
  }

  // Partition is plain `MEM_RESERVE | MEM_WRITE_WATCH` (not a placeholder),
  // so per-chunk commit goes through the `commit_in_reservation` family
  // rather than `commit_replace*`. WW arming inherits from the partition VAD
  // — no per-chunk WW flag, no placeholder lifecycle involvement.
  NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::commit_in_reservation_no_writewatch(
      chunk_base, chunk_bytes, PAGE_READWRITE);
  if (LIBC_UNLIKELY(!NT_SUCCESS(st))) {
    // OOM on commit — release the tree slot before returning failure.
    arena_free_chunk(chunk_base, chunk_bytes);
    return nullptr;
  }

  uint32_t slot_idx = 0;
  BuddyChunkDescriptor *desc = desc_pool_alloc(slot_idx);
  if (LIBC_UNLIKELY(desc == nullptr)) {
    (void)::LIBC_NAMESPACE::nt_pal::decommit_uncommitted(chunk_base,
                                                          chunk_bytes);
    arena_free_chunk(chunk_base, chunk_bytes);
    return nullptr;
  }
  uintptr_t secret = ::LIBC_NAMESPACE::g_pcb.zone0.buddy_arena_secret();
  // Bumped per allocation. Stored low-byte for future ABA-style cross-check;
  // the live free path currently relies on chunk_base/chunk_bytes/size_class
  // plus the canary, with Crystalline gating slot reuse until quiescence.
  uint32_t gen = g_first_arena_state.generation.fetch_add(
      1, cpp::MemoryOrder::RELAXED);
  desc->chunk_base = chunk_base;
  desc->chunk_bytes = chunk_bytes;
  desc->canary =
      ::LIBC_NAMESPACE::internal::alloc_primitives::derive_canary(
          secret, chunk_base, chunk_bytes);
  desc->size_class = shift;
  desc->generation = static_cast<uint8_t>(gen & 0xFFu);
  desc->consumer_tag = static_cast<uint16_t>(VaChunkConsumer::BuddyDirect);
  // Per-descriptor cookie XORed against the Zone-0 secret and the slot
  // index so a wild write into pagemap space decodes to attacker-
  // uncontrollable (slot, tag) tuples.
  desc->page_cookie = static_cast<uint32_t>(secret) ^ slot_idx;
  desc->slot_idx = slot_idx;

  // Upgrade the chunk's pagemap OS pages from PAGE_READONLY to PAGE_READWRITE
  // before publishing. Idempotent — concurrent allocators on the same
  // pagemap page race-but-converge.
  int reg_rc = pagemap_register_range(chunk_base, chunk_bytes);
  if (LIBC_UNLIKELY(reg_rc != 0)) {
    desc_pool_free(desc);
    (void)::LIBC_NAMESPACE::nt_pal::decommit_uncommitted(chunk_base,
                                                          chunk_bytes);
    arena_free_chunk(chunk_base, chunk_bytes);
    return nullptr;
  }
  g_arena_domain.init_node(desc);

  // Linearization point — one RELEASE store per 64 KiB. After this,
  // `pagemap_load_descriptor` resolves to `desc`.
  pagemap_publish_range(chunk_base, chunk_bytes, slot_idx,
                        VaChunkConsumer::BuddyDirect);

  g_first_arena_state.live_count.fetch_add(1, cpp::MemoryOrder::RELAXED);
  return chunk_base;
}

void buddy_free_sized(void *addr, size_t size) {
  if (LIBC_UNLIKELY(addr == nullptr))
    return;
  if (LIBC_UNLIKELY(!g_buddy_init.is_ready()))
    __builtin_trap(); // Free before init means caller-side corruption.

  uint8_t shift = buddy_class_shift_for(size);
  if (shift > kBuddyMaxShift) {
    buddy_free_huge(addr, size);
    return;
  }

  size_t chunk_bytes = buddy_class_bytes(shift);

  // Wait-free typed pagemap load (bounds check + ACQUIRE + cookie XOR + tag
  // check + slot bounds). nullptr here is unambiguous corruption.
  BuddyChunkDescriptor *desc =
      pagemap_load_descriptor<VaChunkConsumer::BuddyDirect>(addr);
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();

  // Descriptor is the source of truth; pagemap entries are routing hints.
  // Wrong-class free or corrupted entry trips this before any NT call.
  if (LIBC_UNLIKELY(desc->chunk_base != addr ||
                    desc->chunk_bytes != chunk_bytes ||
                    desc->size_class != shift))
    __builtin_trap();

  // Pagemap retire (RELEASE stores of zero) MUST precede the Crystalline
  // retire below: a wait-free reader past this point decodes `(0, Empty)`
  // and rejects the address, so no new reader can find its way into the
  // descriptor while grace-period reclaim races with us.
  pagemap_retire_range(addr, chunk_bytes);

  // Release the NBALLOC tree slot back to the arena.
  arena_free_chunk(addr, chunk_bytes);
  g_first_arena_state.live_count.fetch_sub(1, cpp::MemoryOrder::RELAXED);

  g_arena_domain.retire(desc);
}

//===----------------------------------------------------------------------===//
// Huge-direct path
//===----------------------------------------------------------------------===//

void *buddy_alloc_huge(size_t size) {
  // Round up here so the descriptor's `chunk_bytes` matches what NT placed
  // — `reserve_placeholder` rounds internally, but the descriptor needs the
  // post-round value.
  size_t aligned = (size + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
  void *p = ::LIBC_NAMESPACE::nt_pal::reserve_placeholder(aligned);
  if (p == nullptr)
    return nullptr;
  // `commit_replace` (NOT `commit_replace_writewatch`) so the FreeFn's
  // `decommit_preserve` (sub-range `MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER`)
  // is accepted — a WW-armed VAD rejects every sub-range release with
  // STATUS_FREE_VM_NOT_AT_BASE (nt_pal::placeholder WW caveat).
  NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::commit_replace(
      p, aligned, PAGE_READWRITE);
  if (!NT_SUCCESS(st)) {
    (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(p);
    return nullptr;
  }

  uint32_t slot_idx = 0;
  BuddyChunkDescriptor *desc = desc_pool_alloc(slot_idx);
  if (desc == nullptr) {
    (void)::LIBC_NAMESPACE::nt_pal::decommit_preserve(p, aligned);
    (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(p);
    return nullptr;
  }
  uintptr_t secret = ::LIBC_NAMESPACE::g_pcb.zone0.buddy_arena_secret();
  uint32_t gen = g_first_arena_state.generation.fetch_add(
      1, cpp::MemoryOrder::RELAXED);
  desc->chunk_base = p;
  desc->chunk_bytes = aligned;
  desc->canary =
      ::LIBC_NAMESPACE::internal::alloc_primitives::derive_canary(
          secret, p, aligned);
  desc->size_class = kClassHuge;
  desc->generation = static_cast<uint8_t>(gen & 0xFFu);
  desc->consumer_tag = static_cast<uint16_t>(VaChunkConsumer::HugeDirect);
  desc->page_cookie = static_cast<uint32_t>(secret) ^ slot_idx;
  desc->slot_idx = slot_idx;

  if (pagemap_register_range(p, aligned) != 0) {
    desc_pool_free(desc);
    (void)::LIBC_NAMESPACE::nt_pal::decommit_preserve(p, aligned);
    (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(p);
    return nullptr;
  }
  g_arena_domain.init_node(desc);

  // Stamp every 64 KiB sub-chunk with `(slot, HugeDirect)` so the SIGSEGV
  // classifier and `is_libc_pointer` can resolve any interior address back
  // to the descriptor.
  uint64_t enc = pagemap_encode(slot_idx, VaChunkConsumer::HugeDirect);
  size_t n_entries = aligned >> kPagemapShift;
  for (size_t i = 0; i < n_entries; ++i) {
    void *entry_addr = static_cast<char *>(p) + (i * kPagemapChunkBytes);
    pagemap_store_encoded(entry_addr, enc);
  }
  return p;
}

void buddy_free_huge(void *addr, size_t size) {
  if (addr == nullptr)
    return;
  size_t aligned = (size + 0xFFFF) & ~static_cast<size_t>(0xFFFF);

  // Tag mismatch = corruption or wrong-path free (huge address fed to
  // sized-free, or vice versa).
  BuddyChunkDescriptor *desc =
      pagemap_load_descriptor<VaChunkConsumer::HugeDirect>(addr);
  if (LIBC_UNLIKELY(desc == nullptr))
    __builtin_trap();
  if (LIBC_UNLIKELY(desc->size_class != kClassHuge ||
                    desc->chunk_base != addr ||
                    desc->chunk_bytes != aligned))
    __builtin_trap();

  pagemap_retire_range(addr, aligned);
  // FreeFn dispatches on `consumer_tag == HugeDirect` to release the
  // placeholder VA back to MEM_FREE; no per-allocation VA leak.
  g_arena_domain.retire(desc);
}

//===----------------------------------------------------------------------===//
// Tier-A init
//===----------------------------------------------------------------------===//

uint32_t buddy_arena_init_fn(::LIBC_NAMESPACE::internal::Receipt *out,
                              uint32_t cap) {
  if (!g_buddy_init.try_begin())
    __builtin_trap(); // Tier A is single-threaded; double-init is a bug.

  // Per-process buddy canary key via ProcessPrng, sealed in Zone 0.
  // Fail-closed on PRNG failure / zero draw (see canary_seed).
  ::LIBC_NAMESPACE::internal::alloc_primitives::SingleCanarySeed seed{};
  ::LIBC_NAMESPACE::internal::alloc_primitives::init_seed_or_trap(seed);
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_buddy_arena_secret(
      seed.seed);

  // Partition constrained below `max_address()` so every chunk's pagemap
  // entry lands inside the tracked window — without the constraint NT can
  // hand back a reservation just past pagemap_end and the first decode
  // resolves out-of-bounds. WW armed once at the VAD level.
  void *partition =
      ::LIBC_NAMESPACE::nt_pal::reserve_uncommitted_writewatch_below(
          ::LIBC_NAMESPACE::g_pcb.zone0.max_address(), kBuddyArenaBytes);
  if (partition == nullptr)
    __builtin_trap();
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_buddy_partition_base(
      partition);
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_buddy_partition_bytes(
      kBuddyArenaBytes);
  publish_sealed_va_range(SealedKind::BuddyPartition, partition,
                           kBuddyArenaBytes);

  // Tree backing as its own placeholder so the partition stays maximally
  // usable for chunks. Commit deferred to `arena_lazy_commit_tree`.
  void *tree_storage =
      ::LIBC_NAMESPACE::nt_pal::reserve_placeholder(kBuddyTreeBytes);
  if (tree_storage == nullptr)
    __builtin_trap();
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_buddy_tree_base(
      tree_storage);
  publish_sealed_va_range(SealedKind::BuddyTree, tree_storage,
                           kBuddyTreeBytes);

  // Descriptor pool — eager commit; Crystalline `retire()` cannot tolerate
  // a lazy pool.
  size_t pool_bytes =
      kBuddyDescPoolCapacity * sizeof(BuddyChunkDescriptor);
  void *pool_va =
      ::LIBC_NAMESPACE::nt_pal::reserve_placeholder(pool_bytes);
  if (pool_va == nullptr)
    __builtin_trap();
  NTSTATUS st = ::LIBC_NAMESPACE::nt_pal::commit_replace(
      pool_va, pool_bytes, PAGE_READWRITE);
  if (!NT_SUCCESS(st))
    __builtin_trap();
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_buddy_desc_pool_base(
      pool_va);
  ::LIBC_NAMESPACE::internal::PcbInitAccess::set_buddy_desc_pool_capacity(
      kBuddyDescPoolCapacity);
  publish_sealed_va_range(SealedKind::BuddyDescPool, pool_va, pool_bytes);

  g_arena_domain.init_registration();
  g_buddy_init.publish_ready();

  // Receipts so the substrate registry stamps these reservations as
  // libc-internal during the bootstrap pass.
  uint32_t emitted = 0;
  if (cap >= 3) {
    out[0] = {partition, kBuddyArenaBytes,
              ::LIBC_NAMESPACE::internal::InternalKind::BuddyArena};
    out[1] = {tree_storage, kBuddyTreeBytes,
              ::LIBC_NAMESPACE::internal::InternalKind::BuddyArena};
    out[2] = {pool_va, pool_bytes,
              ::LIBC_NAMESPACE::internal::InternalKind::BuddyArena};
    emitted = 3;
  }
  return emitted;
}

// Arena-level fork-reinit. Crystalline's own hook zeroes eras / discards
// retire batches; pagemap entries / cookie are fork-stable (Zone 0 sealed).
// Here we drop CoW-shared pages from "zombies" (chunks the parent freed but
// whose Crystalline FreeFn never ran) and reclaim the slots they hold.
//
// Descriptor-pool-driven (not tree-walk-driven). For each occupied bitmap
// slot:
//   (a) Canary mismatch = orphan from a parent killed between
//       `desc_pool_alloc` and the canary fill — reclaim the slot, trust
//       nothing else.
//   (b) Probe `pagemap_load_descriptor<Tag>(chunk_base)`:
//       * via == d              — live; preserve verbatim.
//       * via != d, != nullptr  — VA reused by another live chunk after
//                                 retire; reclaim our slot only — DO NOT
//                                 decommit (would corrupt the successor).
//       * via == nullptr        — genuine zombie. BuddyDirect:
//                                 `decommit_uncommitted` +
//                                 `clean_zombie_tree_path` (parent may
//                                 have died mid-`nb_free_node` Phase 1).
//                                 HugeDirect: `decommit_preserve` +
//                                 `free_placeholder`.
//
// A tree walk would issue one syscall per FREE subtree (O(N) mostly
// no-ops) and structurally miss HugeDirect zombies and slot reclamation.
//
// Residual gap: parent killed between the `nb_try_alloc` leaf CAS (NB_OCC
// stamped) and `desc_pool_alloc` leaves a leaf in NB_OCC with no
// descriptor — invisible from the pool walk. Damage: one tree slot per
// occurrence permanently un-allocatable; bound `<= T-1` per fork. If the
// kill landed after `commit_in_reservation_no_writewatch`, that chunk's
// physical pages are also stranded until process exit (no slot, no free
// path); strictly before it, no RAM leak.
//
// TODO(buddy-fork-reinit): optional tree post-scan for NB_OCC orphans —
// linear pass over `kBuddyTreeBytes`. Wire if telemetry shows long-running
// fork-without-exec accumulating un-allocatable leaves.
void buddy_arena_fork_reinit() {
  if (g_buddy_init.is_ready()) {
    auto *pool_base = static_cast<BuddyChunkDescriptor *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_desc_pool_base());
    auto *tree = static_cast<cpp::Atomic<uint8_t> *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_tree_base());
    char *part_base = static_cast<char *>(
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_base());
    size_t part_bytes =
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_partition_bytes();
    uintptr_t secret =
        ::LIBC_NAMESPACE::g_pcb.zone0.buddy_arena_secret();

    if (pool_base != nullptr && tree != nullptr && part_base != nullptr) {
      using Bitmap = decltype(g_desc_pool_occupancy);
      for (size_t w = 0; w < Bitmap::word_count; ++w) {
        uint64_t bits =
            g_desc_pool_occupancy.word_at<cpp::MemoryOrder::RELAXED>(w);
        while (bits) {
          unsigned bit = static_cast<unsigned>(__builtin_ctzll(bits));
          bits &= bits - 1;
          size_t slot = w * 64u + bit;
          if (LIBC_UNLIKELY(slot >= kBuddyDescPoolCapacity))
            break;
          BuddyChunkDescriptor *d = &pool_base[slot];

          uintptr_t expected =
              ::LIBC_NAMESPACE::internal::alloc_primitives::derive_canary(
                  secret, d->chunk_base, d->chunk_bytes);
          if (LIBC_UNLIKELY(d->canary != expected)) {
            desc_pool_free(d); // Orphan/corruption — case (a).
            continue;
          }

          bool is_huge =
              d->consumer_tag ==
              static_cast<uint16_t>(VaChunkConsumer::HugeDirect);
          // Snapshot before `desc_pool_free` wipes the descriptor.
          void *cb = d->chunk_base;
          size_t cs = d->chunk_bytes;

          BuddyChunkDescriptor *via =
              is_huge
                  ? pagemap_load_descriptor<VaChunkConsumer::HugeDirect>(cb)
                  : pagemap_load_descriptor<VaChunkConsumer::BuddyDirect>(
                        cb);

          if (via == d)
            continue; // Live.

          if (via != nullptr) {
            desc_pool_free(d); // VA reused by successor — reclaim slot only.
            continue;
          }

          // Genuine zombie.
          if (is_huge) {
            (void)::LIBC_NAMESPACE::nt_pal::decommit_preserve(cb, cs);
            (void)::LIBC_NAMESPACE::nt_pal::free_placeholder(cb);
          } else {
            (void)::LIBC_NAMESPACE::nt_pal::decommit_uncommitted(cb, cs);
            clean_zombie_tree_path(tree, part_base, part_bytes, cb, cs);
          }
          desc_pool_free(d);
        }
      }
    }
  }

  // Latch resets last. `fork_reinit` only flips INITIALIZING -> UNINIT; a
  // READY arena stays READY in the child, so the descriptor walk above ran
  // against the same Zone-0 state the parent observed.
  g_first_arena_state.tree_init.fork_reinit();
  g_buddy_init.fork_reinit();
}

BuddyStats buddy_stats_snapshot() {
  BuddyStats s = {};
  s.total_arenas = 1;
  s.total_chunks_live = static_cast<size_t>(
      g_first_arena_state.live_count.load(cpp::MemoryOrder::RELAXED));
  s.total_committed_bytes = 0; // Not tracked at this layer.
  s.descriptor_pool_used = g_desc_pool_occupancy.popcount();
  return s;
}

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

//===----------------------------------------------------------------------===//
// Section registry hooks (must expand at namespace scope)
//===----------------------------------------------------------------------===//

LIBC_REGISTER_MEMORY_PRIMITIVE(
    buddy_arena, 4,
    &::LIBC_NAMESPACE::windows::alloc::buddy_arena_init_fn)

LIBC_REGISTER_FORK_REINIT(
    buddy_arena, ::LIBC_NAMESPACE::internal::kForkPrioAlloc,
    &::LIBC_NAMESPACE::windows::alloc::buddy_arena_fork_reinit)
