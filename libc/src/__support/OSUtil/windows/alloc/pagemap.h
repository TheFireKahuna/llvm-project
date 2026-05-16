//===- alloc/pagemap.h - Cookie-XOR'd VA-to-descriptor index ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Out-of-band 8 B atomic index, one entry per 64 KiB chunk, addressed by
// `addr >> 16`. Each entry encodes a 32-bit consumer slot index plus an
// 8-bit VaChunkConsumer tag, XOR'd with a Zone-0-sealed cookie:
//
//   encoded = ((slot_idx << 8) | uint8_t(tag)) ^ pagemap_cookie
//
// The cookie's low byte is forced to zero at init so a freshly
// zero-filled (or shared-zero) entry decodes with tag = Empty (the
// slot_idx field of such a decode is `cookie >> 8` — meaningless, and
// callers MUST gate on tag first). "Unregistered" is the natural state.
//
// Snmalloc design (Liétar et al., ISMM 2019, §2.3): globally shared,
// lazily populated, fixed-granularity pagemap whose entries are safe to
// load without synchronisation. The cookie XOR is a hardening layer
// absent from snmalloc — a wild write produces either an out-of-band
// tag (rejected by dispatch) or an out-of-pool slot index (rejected by
// pagemap_load_descriptor's bounds check). Pagemap corruption alone
// cannot escalate to a wrong-descriptor dereference without also
// defeating the Zone-0 cookie.
//
// Backing array: reserved + committed PAGE_READONLY for process lifetime
// in one NtAllocateVirtualMemoryEx call; NT binds every PTE to the
// kernel-owned shared zero page so the reservation costs zero physical
// RAM until a writer upgrades pages to PAGE_READWRITE via nt_pal::protect.
// Pages never decommit, so every load is wait-free and non-faulting from
// any context — SIGSEGV handler, debugger probe, fault classifier,
// __cxa_finalize teardown.
//
// The decoded tuple is a routing hint, not the source of truth. The
// descriptor carries canary / generation / size class / hardening seed;
// readers cross-check the canary under a Crystalline-W pin before
// touching descriptor payload (Nikolaev/Ravindran, PLDI 2024 — the SMR
// scheme covering descriptor lifetime).
//
// Single-publisher per chunk-aligned address. Stores RELEASE; readers
// ACQUIRE. No CAS for publish/retire; pagemap_cordon's cross-tag toggles
// are the one CAS site (single 8 B word).
//
// Retire protocol (chunk-owning layer's obligation):
//
//   1. pagemap_retire_range zeroes covered entries. Zero decodes to
//      (0, Empty) by the cookie low-byte invariant.
//   2. Owner returns VA to its arena. After step 1, a racing reader
//      decodes Empty and rejects the address before reaching the
//      descriptor, so VA-return here is safe.
//   3. Owner retires the descriptor through its Crystalline-W domain
//      (canary check, cleanup, slot back to pool). Crystalline gates
//      pool reuse against any wait-free reader that pinned before
//      step 1; a reader past step 1 cannot reach the descriptor. The
//      descriptor slot may therefore return to its pool after the VA
//      has already recycled — the two pools are independent.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

// 16 → 64 KiB per entry, matching the slab-page granularity used by
// mimalloc and snmalloc.
inline constexpr size_t kPagemapShift = 16;
inline constexpr size_t kPagemapChunkBytes = static_cast<size_t>(1)
                                              << kPagemapShift;

// One OS page = 4096 / 8 = 512 entries, covering 32 MiB of tracked VA.
inline constexpr size_t kPagemapOsPageSize = 4096;
inline constexpr size_t kPagemapEntriesPerOsPage = kPagemapOsPageSize / 8;
inline constexpr size_t kPagemapTrackedVaPerOsPage =
    kPagemapEntriesPerOsPage * kPagemapChunkBytes;

// Low-byte tag of a pagemap entry. The numeric layout is significant:
// pagemap_classifier and pagemap_cordon dispatch in O(1) via
// band-comparison range tests (`tag >= kCordonBandLo && tag <=
// kCordonBandHi`). Do not reorder; add new tags at the end of the
// appropriate band.
//
// Bands:
//   0x00          Empty (untouched / retired / out-of-bounds / POSIX VA).
//   0x01..0x0F    Layer-2 / Layer-3 chunk allocators.
//   0x10..0x1F    Sealed Tier-A / hardening kinds.
//   0x20..0x2F    Layer-1 va_tracker internals.
//   0x30..0x3F    Cordons (Image / Kernel / Foreign / ForeignStale).
//   0x40..0x4F    Libc-internal facade tags.
//   0x60..0x6F    Reserved-empty. is_libc_owned is a positive allow-list,
//                 so a stamp landing here (e.g. a wild write that flips
//                 an existing entry's tag byte) classifies as not
//                 libc-owned — defence-in-depth against bit-flip
//                 primitives that survive the cookie XOR.
//   0xFF          Misc catch-all.
//
// POSIX-visible VA carries NO pagemap entry. va_tracker (ART + interval
// skiplist) is the source of truth for POSIX mappings; a pagemap probe
// on POSIX VA decodes to Empty and the classifier falls through to
// va_tracker::resolve. Composing the two indices avoids a per-64-KiB
// publish loop on every POSIX mmap.
enum class VaChunkConsumer : uint8_t {
  Empty = 0,
  BuddyDirect = 1,
  HugeDirect = 2,
  // Reserved for segment / slab-page tracks: Segment = 3, SlabPage = 4,
  // up through 0x0F.
  PartitionGuard = 0x10,
  GwpAsan = 0x11,
  // Fine-grained sealed kind lives in slot_idx via SealedKind (see
  // sealed_va_publisher.h); membership consumers compare LibcSealed,
  // diagnostics decode slot_idx.
  LibcSealed = 0x12,
  // slot_idx indexes the shared g_va_chunk_desc_pool.
  VaTrackerVaChunk = 0x20,
  // slot_idx encodes (node_type << 8) | chunk_id; the four-pool layout
  // doesn't fit PagemapTraits' single-base-pointer shape, so resolution
  // bypasses pagemap_load_descriptor — art_node_alloc.cpp dispatches per
  // node type against its own per-pool chunk tables.
  VaTrackerArtChunk = 0x21,
  Image = 0x30,
  Kernel = 0x31,
  Foreign = 0x32,
  // ForeignStale is reachable only via set_cordon_stale /
  // clear_cordon_stale; the public stamp surface never produces this
  // tag directly.
  ForeignStale = 0x33,
  // Facade band: subsystems that own VA outside the chunk allocator's
  // descriptor pools. Stamped by alloc/internal_va_facade.cpp; the
  // facade retires before unmap so a load on freed VA decodes to Empty.
  LibcSignalMailbox = 0x40,
  LibcFifoChannel = 0x41,
  LibcPtyRing = 0x42,
  LibcSysVSemHeader = 0x43,
  LibcSubstrateArena = 0x44,
  Misc = 0xFF,
};

// One pagemap slot. Atomic to support single-CAS toggles
// (pagemap_cordon::toggle_cordon_band) and to give every reader a
// torn-free 8 B load on x86-64 and AArch64. Non-copyable: the array is
// addressed by physical position.
struct alignas(8) PagemapEntry {
  cpp::Atomic<uint64_t> encoded;

  LIBC_INLINE constexpr PagemapEntry() : encoded{} {}

  PagemapEntry(const PagemapEntry &) = delete;
  PagemapEntry &operator=(const PagemapEntry &) = delete;
};

static_assert(sizeof(PagemapEntry) == 8,
              "PagemapEntry must be exactly 8 bytes");
static_assert(alignof(PagemapEntry) == 8,
              "PagemapEntry must be 8-byte aligned");

// XOR-decoded routing tuple. slot_idx is a back-pointer into the owning
// consumer's descriptor pool; tag identifies which pool. For tag ==
// Empty slot_idx is meaningless — callers MUST gate on tag first.
struct PagemapDecoded {
  uint32_t slot_idx;
  VaChunkConsumer tag;
};

// XOR is symmetric — pagemap_decode applies the same cookie to invert.
LIBC_INLINE uint64_t pagemap_encode(uint32_t slot_idx, VaChunkConsumer tag) {
  return ((static_cast<uint64_t>(slot_idx) << 8) |
          static_cast<uint64_t>(tag)) ^
         g_pcb.zone0.pagemap_cookie();
}

LIBC_INLINE PagemapDecoded pagemap_decode(uint64_t encoded) {
  uint64_t plain = encoded ^ g_pcb.zone0.pagemap_cookie();
  PagemapDecoded d;
  d.slot_idx = static_cast<uint32_t>(plain >> 8);
  d.tag = static_cast<VaChunkConsumer>(plain & 0xFFu);
  return d;
}

namespace internal {

// Reads the base from sealed PCB Zone 0 rather than a BSS cache: an
// arbitrary-write attacker who could flip a mutable base pointer would
// otherwise redirect every hot-path lookup to attacker-controlled memory
// with a single store.
[[nodiscard]] LIBC_INLINE PagemapEntry *
pagemap_slot_unchecked(const void *addr) {
  uintptr_t idx = reinterpret_cast<uintptr_t>(addr) >> kPagemapShift;
  auto *base = static_cast<PagemapEntry *>(g_pcb.zone0.pagemap_base());
  return base + idx;
}

[[nodiscard]] LIBC_INLINE bool pagemap_addr_in_bounds(const void *addr) {
  uintptr_t idx = reinterpret_cast<uintptr_t>(addr) >> kPagemapShift;
  auto *base = static_cast<PagemapEntry *>(g_pcb.zone0.pagemap_base());
  auto *end = static_cast<PagemapEntry *>(g_pcb.zone0.pagemap_end());
  return base != nullptr && (base + idx) < end;
}

} // namespace internal

// Wait-free, non-faulting decode of the entry covering addr. Out-of-bounds
// and untouched (shared-zero) entries return (0, Empty). The load itself
// can never fault — the reservation is committed PAGE_READONLY for
// process lifetime. ACQUIRE pairs with the owner's RELEASE in
// pagemap_store / pagemap_store_encoded.
[[nodiscard]] LIBC_INLINE PagemapDecoded pagemap_load_decoded(const void *addr) {
  if (LIBC_UNLIKELY(!internal::pagemap_addr_in_bounds(addr)))
    return PagemapDecoded{0, VaChunkConsumer::Empty};
  PagemapEntry *slot = internal::pagemap_slot_unchecked(addr);
  uint64_t enc = slot->encoded.load(cpp::MemoryOrder::ACQUIRE);
  return pagemap_decode(enc);
}

// Wait-free membership probe. False for out-of-bounds, untouched, and
// retired entries.
[[nodiscard]] LIBC_INLINE bool pagemap_is_tracked(const void *addr) {
  PagemapDecoded d = pagemap_load_decoded(addr);
  return d.tag != VaChunkConsumer::Empty;
}

// One 8 B RELEASE store; readers ACQUIRE-load. Single-publisher invariant
// means no CAS. Precondition: pagemap_register_range covering this
// entry's OS page has succeeded; otherwise the page is still
// PAGE_READONLY-shared-zero and the store will fault.
LIBC_INLINE void pagemap_store(void *chunk_aligned_addr, uint32_t slot_idx,
                                VaChunkConsumer tag) {
  PagemapEntry *slot = internal::pagemap_slot_unchecked(chunk_aligned_addr);
  uint64_t enc = pagemap_encode(slot_idx, tag);
  slot->encoded.store(enc, cpp::MemoryOrder::RELEASE);
}

// Pre-encoded variant: callers stamping the same word across multiple
// chunks run pagemap_encode once outside the loop.
LIBC_INLINE void pagemap_store_encoded(void *chunk_aligned_addr,
                                       uint64_t encoded) {
  PagemapEntry *slot = internal::pagemap_slot_unchecked(chunk_aligned_addr);
  slot->encoded.store(encoded, cpp::MemoryOrder::RELEASE);
}

// Stamp the same (slot_idx, tag) into every 64 KiB sub-entry covering
// the range. Preconditions: single-writer ownership, successful
// pagemap_register_range over the same range, chunk-aligned inputs.
LIBC_INLINE void pagemap_publish_range(void *chunk_base, size_t chunk_bytes,
                                        uint32_t slot_idx,
                                        VaChunkConsumer tag) {
  uint64_t encoded = pagemap_encode(slot_idx, tag);
  size_t n_entries = chunk_bytes >> kPagemapShift;
  for (size_t i = 0; i < n_entries; ++i) {
    void *entry_addr =
        static_cast<char *>(chunk_base) + (i * kPagemapChunkBytes);
    pagemap_store_encoded(entry_addr, encoded);
  }
}

// Retire by storing literal zero. Zero decodes to tag = Empty by the
// cookie's low-byte invariant (slot_idx of an Empty entry is meaningless
// — callers gate on tag), matching the shared-zero state of an untouched
// OS page. Encoding (0, Empty) explicitly would produce `cookie` itself,
// which is the wrong word — readers expect the natural zero.
// Preconditions: single-writer ownership, chunk-aligned inputs.
LIBC_INLINE void pagemap_retire_range(void *chunk_base, size_t chunk_bytes) {
  size_t n = chunk_bytes >> kPagemapShift;
  if (LIBC_UNLIKELY(n == 0))
    return;
  for (size_t i = 0; i < n; ++i) {
    void *entry_addr =
        static_cast<char *>(chunk_base) + (i * kPagemapChunkBytes);
    PagemapEntry *slot = internal::pagemap_slot_unchecked(entry_addr);
    slot->encoded.store(0, cpp::MemoryOrder::RELEASE);
  }
}

// Upgrade pagemap OS pages covering the range from PAGE_READONLY (the
// initial shared-zero state) to PAGE_READWRITE. Must run before any
// pagemap_store on the affected entries; a publish to a still-RO page
// would fault. Idempotent: a per-OS-page atomic byte elides redundant
// syscalls (performance only — protect() is idempotent at the OS level).
// Returns 0; -ENOMEM if protect fails; -EINVAL on malformed range.
[[nodiscard]] int pagemap_register_range(void *chunk_base, size_t chunk_bytes);

// Symmetry-only no-op kept at the consumer call site (buddy_arena's
// FreeFn). Pagemap pages stay committed for process lifetime; Layer-3
// sub-slab decommit operates on chunk VA, not pagemap VA. Same
// discipline as snmalloc's notify_using_readonly PAL.
LIBC_INLINE void pagemap_unregister_range(void * /*chunk_base*/,
                                           size_t /*chunk_bytes*/) {
}

// Per-consumer traits. Each consumer specialises this in its own header
// to declare its descriptor type and the sealed Zone-0 accessors for
// pool base and capacity. The pagemap layer never includes consumer
// headers; the primary template is intentionally undefined so a
// consumer that forgets to specialise gets an "incomplete type"
// diagnostic at the typed-load call site, not a silent fallback to a
// wrong pool.
//
// A specialisation MUST expose:
//   using Descriptor = ...;
//   static size_t pool_capacity();   // Zone-0-sealed slot count.
//   static Descriptor *pool_base();  // Zone-0-sealed pool VA.
//
// Both accessors MUST read from PCB Zone 0. A mutable BSS pointer would
// let an arbitrary-write attacker redirect every typed load with one
// store — the same hazard the cookie XOR defends against.
template <VaChunkConsumer Tag> struct PagemapTraits;

// Wait-free typed descriptor lookup. Decodes, tag-matches against Tag,
// bounds-checks slot_idx, indexes the pool. Non-faulting on any user-VA
// address — pagemap and consumer pool are both Tier-A sealed; safe from
// SIGSEGV, debugger probe, and __cxa_finalize contexts. Returns nullptr
// for out-of-bounds, untouched, wrong-tag, or slot-past-capacity entries
// (corruption or forged word). Caller still owns the consumer-specific
// cross-check (canary, geometry, generation) before dereferencing.
template <VaChunkConsumer Tag>
[[nodiscard]] LIBC_INLINE typename PagemapTraits<Tag>::Descriptor *
pagemap_load_descriptor(const void *addr) {
  PagemapDecoded d = pagemap_load_decoded(addr);
  if (LIBC_UNLIKELY(d.tag != Tag))
    return nullptr;
  using Traits = PagemapTraits<Tag>;
  if (LIBC_UNLIKELY(d.slot_idx >= Traits::pool_capacity()))
    return nullptr;
  return Traits::pool_base() + d.slot_idx;
}

// Tier-A init, registered via LIBC_REGISTER_MEMORY_PRIMITIVE at phase 3
// (after PAL = 0, va_substrate = 1, mapping_table = 2). Reserves the
// backing array PAGE_READONLY-stay-committed, draws the cookie from
// ProcessPrng (redrawing until the low byte is zero; acceptance rate
// 1/256 so expected ~256 draws), stamps PCB Zone 0, and emits Receipts
// so Pass 2 marks the reservations libc-internal. Returns number of
// Receipts written.
uint32_t pagemap_init_fn(::LIBC_NAMESPACE::internal::Receipt *out, uint32_t cap);

// Intentional no-op. Reservation survives via CoW; cookie fork-stable
// through sealed Zone 0. Registered for completeness.
void pagemap_fork_reinit();

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
