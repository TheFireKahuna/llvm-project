//===- alloc/pagemap.h - Cookie-XOR'd VA-to-descriptor index ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Out-of-band metadata index for every chunk of user VA owned by the libc
/// allocator. One 8-byte atomic entry per 64 KiB chunk, addressed by
/// `addr >> 16`; each entry encodes a 32-bit slot index into the owning
/// consumer's descriptor pool together with an 8-bit \c VaChunkConsumer tag,
/// XOR'd with a per-process cookie sealed in PCB Zone 0:
///
/// \code
///   encoded = ((slot_idx << 8) | uint8_t(tag)) ^ pagemap_cookie
/// \endcode
///
/// The cookie's low byte is constrained to zero at init so that a freshly
/// zero-filled (or shared-zero) entry decodes to `(slot_idx=0, tag=Empty)`.
/// "Unregistered" is therefore the natural value of an untouched entry, not
/// a special case. The `slot_idx` of such an entry is `cookie >> 8` and is
/// meaningless; callers gate on \c tag first.
///
/// The design follows snmalloc (Liétar et al., ISMM 2019, §2.3): a globally
/// shared, lazily populated pagemap chunked at a fixed granularity, with
/// every entry safe to load from any thread without synchronisation. The
/// cookie XOR is a hardening layer absent from snmalloc — a wild write into
/// the pagemap range produces either an out-of-band tag (rejected by
/// dispatch) or a slot index past the consumer's pool capacity (rejected by
/// the bounds check in \c pagemap_load_descriptor), so pagemap corruption
/// cannot be escalated to a wrong-descriptor dereference without the
/// attacker also defeating the Zone-0 cookie.
///
/// The backing array is reserved + committed `PAGE_READONLY` for the
/// process lifetime in one \c NtAllocateVirtualMemoryEx call; NT binds every
/// PTE to the kernel-owned shared zero page so the reservation costs zero
/// physical RAM until a writer upgrades touched OS pages to
/// `PAGE_READWRITE` via \c nt_pal::protect. Pages never decommit. Every
/// load is therefore wait-free and non-faulting from any context — SIGSEGV
/// handler, debugger probe, fault classifier, \c __cxa_finalize teardown.
///
/// Reader contract (the three load surfaces):
///
///   * \c pagemap_load_decoded(addr) — one ACQUIRE load, cookie XOR, bounds
///     check. Returns the raw `(slot_idx, tag)` routing tuple, or
///     `(0, Empty)` for out-of-bounds / untouched addresses. Never faults.
///
///   * \c pagemap_load_descriptor<Tag>(addr) — composes the decode with a
///     tag-validity check and a slot bounds check against the consumer's
///     pool capacity, then indexes into the pool's sealed Zone-0 base.
///     Each consumer specialises \c PagemapTraits<Tag> in its own header
///     to provide the descriptor type and sealed accessors; the pagemap
///     layer itself includes no consumer headers.
///
///   * \c pagemap_is_tracked(addr) — membership probe; true iff the entry
///     decodes to anything other than \c Empty.
///
/// The decoded tuple is a routing hint, not the source of truth. The
/// descriptor carries the canary, generation, size class, and hardening
/// seed; readers cross-check the canary under a Crystalline-W pin before
/// touching descriptor payload (Nikolaev/Ravindran, PLDI 2024 — the SMR
/// reclamation scheme covering descriptor lifetime).
///
/// Single-publisher per chunk-aligned address: the chunk's owner is the
/// sole writer of the entry. Stores are RELEASE; readers synchronise via
/// the matching ACQUIRE on load. No CAS retry is required for publish or
/// retire.
///
/// Retire protocol (the chunk-owning layer's obligation):
///
///   1. \c pagemap_retire_range(chunk_base, chunk_bytes) zeroes every
///      covered entry. Zero decodes to `(0, Empty)` because of the
///      cookie's low-byte constraint.
///   2. The chunk owner returns the VA to its arena. Once the pagemap
///      entries are zeroed any racing reader decodes `(0, Empty)` and
///      rejects the address before it reaches the descriptor slot, so
///      returning VA here is safe.
///   3. The chunk owner retires the descriptor through the consumer's
///      Crystalline-W domain (canary check → descriptor cleanup → slot
///      back to pool). The grace period gates descriptor-pool reuse
///      against any wait-free reader that pinned the descriptor
///      before step 1. A reader past step 1 cannot reach the
///      descriptor; a reader before step 1 holds it pinned through
///      Crystalline. The descriptor slot may therefore return to the
///      pool after the VA has already been recycled — the two pools
///      are independent.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_ALLOC_PAGEMAP_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/memory/legacy/memory_primitives_bootstrap.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace alloc {

//===----------------------------------------------------------------------===//
// Geometry
//===----------------------------------------------------------------------===//

/// log2 of the chunk granularity. 16 → 64 KiB per pagemap entry, matching
/// the slab-page granularity used by mimalloc and snmalloc.
inline constexpr size_t kPagemapShift = 16;
inline constexpr size_t kPagemapChunkBytes = static_cast<size_t>(1)
                                              << kPagemapShift;

/// OS page size at which the pagemap reservation is protect()-upgraded.
/// One OS page holds 4096 / 8 = 512 entries, covering 32 MiB of tracked
/// user VA.
inline constexpr size_t kPagemapOsPageSize = 4096;
inline constexpr size_t kPagemapEntriesPerOsPage = kPagemapOsPageSize / 8;
inline constexpr size_t kPagemapTrackedVaPerOsPage =
    kPagemapEntriesPerOsPage * kPagemapChunkBytes;

//===----------------------------------------------------------------------===//
// VaChunkConsumer
//===----------------------------------------------------------------------===//

/// Low-byte tag of a pagemap entry; identifies which consumer owns the
/// chunk and therefore which descriptor pool the slot index addresses.
///
/// The numeric layout is significant — \c pagemap_classifier and
/// \c pagemap_cordon both use band-comparison range tests
/// (`tag >= kCordonBandLo && tag <= kCordonBandHi`) to dispatch in O(1)
/// without enumerating individual values. Do not reorder enumerators;
/// new tags are added at the end of the appropriate band.
///
/// Tag bands:
///
///   * `0x00`        — \c Empty (untouched / retired / out-of-bounds).
///   * `0x01..0x0F`  — Layer-2 / Layer-3 chunk allocators
///                     (\c BuddyDirect, \c HugeDirect; further slots
///                     reserved for the segment / slab-page tracks).
///   * `0x10..0x1F`  — Sealed Tier-A / hardening kinds
///                     (\c PartitionGuard, \c GwpAsan, \c LibcSealed).
///   * `0x20..0x2F`  — Layer-1 va_tracker internals
///                     (\c VaTrackerVaChunk, \c VaTrackerArtChunk).
///   * `0x30..0x3F`  — Cordon band (\c Image, \c Kernel, \c Foreign,
///                     \c ForeignStale). Stamped via
///                     `alloc/pagemap_cordon.h`; never enters the
///                     va_tracker skiplist.
///   * `0x40..0x4F`  — Libc-internal facade tags
///                     (\c LibcSignalMailbox, \c LibcFifoChannel, etc).
///   * `0x60..0x6F`  — Reserved-empty. Accidental future stamps in
///                     this band (e.g. a wild write that flips an
///                     existing entry's tag byte) fail the
///                     classifier's `is_libc_owned` predicate, which
///                     is a positive allow-list over the libc-stamped
///                     bands above and below. The cookie XOR rejects
///                     most full-word forgeries; this band gives the
///                     classifier a defence-in-depth fallback against
///                     bit-flip primitives that survive the XOR.
///   * `0xFF`        — Misc / catch-all libc-stamped chunk.
///
/// POSIX-visible VA carries NO pagemap entry. The va_tracker (ART +
/// interval skiplist) is the source of truth for POSIX mappings; a
/// pagemap probe on POSIX VA decodes to \c Empty and the classifier
/// falls through to \c va_tracker::resolve. Composing the two indices
/// avoids a per-64-KiB publish loop on every POSIX \c mmap.
enum class VaChunkConsumer : uint8_t {
  Empty = 0,
  BuddyDirect = 1,
  HugeDirect = 2,
  // Reserved for the segment / slab-page tracks: Segment = 3, SlabPage = 4,
  // up through 0x0F.
  PartitionGuard = 0x10,
  GwpAsan = 0x11,
  /// Sealed Tier-A libc-internal VA range (pagemap, descriptor pools,
  /// etc.). The fine-grained kind is encoded in the entry's \c slot_idx
  /// via \c windows::alloc::SealedKind (see \c sealed_va_publisher.h).
  /// Membership-only consumers compare against \c LibcSealed; diagnostic
  /// paths decode \c slot_idx for the kind.
  LibcSealed = 0x12,
  /// va_tracker chunk covering skiplist nodes, RegionDesc, and Arena
  /// chunks. \c slot_idx indexes the shared \c g_va_chunk_desc_pool.
  /// Backing descriptor type: \c VaChunkDesc.
  VaTrackerVaChunk = 0x20,
  /// va_tracker chunk covering ART node chunks across all four node-type
  /// sub-pools. \c slot_idx encodes `(node_type << 8) | chunk_id`; the
  /// four-pool layout doesn't fit the single-base-pointer shape required
  /// by \c PagemapTraits, so resolution does not go through
  /// \c pagemap_load_descriptor — \c art_node_alloc.cpp dispatches
  /// directly per node type against its own per-pool chunk tables.
  VaTrackerArtChunk = 0x21,
  /// Cordon: image (executable / DLL ranges).
  Image = 0x30,
  /// Cordon: kernel-loaned regions (PEB / TEB / KUSER_SHARED_DATA /
  /// ALPC shared sections / Win32 client shared section / heap / ApiSet
  /// schema).
  Kernel = 0x31,
  /// Cordon: foreign VA captured by \c NtPssCaptureVaSpaceBulk that is
  /// neither POSIX-visible, libc-internal, image, nor kernel.
  Foreign = 0x32,
  /// Cordon: \c Foreign that has been observed torn down. Reachable
  /// only via \c set_cordon_stale / \c clear_cordon_stale; the public
  /// stamp surface never produces this tag directly.
  ForeignStale = 0x33,
  /// Libc-internal facade tags. Each names a libc-internal subsystem
  /// that owns VA outside the chunk-allocator's descriptor pools.
  /// Stamped by \c alloc/internal_va_facade.cpp; the facade retires
  /// these entries before unmap so a load on freed VA decodes to
  /// \c Empty.
  LibcSignalMailbox = 0x40,
  LibcFifoChannel = 0x41,
  LibcPtyRing = 0x42,
  LibcSysVSemHeader = 0x43,
  LibcSubstrateArena = 0x44,
  /// Catch-all for libc-stamped chunks without a dedicated tag.
  Misc = 0xFF,
};

//===----------------------------------------------------------------------===//
// PagemapEntry — single 8 B atomic word
//===----------------------------------------------------------------------===//

/// One pagemap slot. The encoded word is atomic to support single-CAS
/// toggles (see \c pagemap_cordon::toggle_cordon_band) and to give every
/// reader a torn-free 8 B load on x86-64 and AArch64. Non-copyable: the
/// pagemap array is addressed by physical position, never moved.
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

//===----------------------------------------------------------------------===//
// PagemapDecoded — XOR-decoded routing tuple
//===----------------------------------------------------------------------===//

/// Decoded pagemap entry. \c slot_idx is a back-pointer into the owning
/// consumer's descriptor pool; \c tag identifies which pool to consult.
/// For \c tag == \c VaChunkConsumer::Empty the \c slot_idx field is
/// meaningless and consumers must not consult it.
struct PagemapDecoded {
  uint32_t slot_idx;
  VaChunkConsumer tag;
};

//===----------------------------------------------------------------------===//
// Encode / decode helpers
//===----------------------------------------------------------------------===//

/// Pack `(slot_idx, tag)` into the wire form and XOR with the Zone-0
/// cookie. The XOR is symmetric — \c pagemap_decode applies the same
/// cookie to invert.
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

//===----------------------------------------------------------------------===//
// Internal helpers
//===----------------------------------------------------------------------===//

namespace internal {

/// Compute the entry pointer for \p addr without bounds checking. Reads
/// the pagemap base from sealed PCB Zone 0 rather than a BSS cache: an
/// arbitrary-write attacker who could flip a mutable base pointer would
/// otherwise redirect every hot-path lookup to attacker-controlled
/// memory with a single store.
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

//===----------------------------------------------------------------------===//
// Public API — load
//===----------------------------------------------------------------------===//

/// Wait-free, non-faulting decode of the entry covering \p addr.
///
/// Performs one ACQUIRE 8 B load against the pagemap reservation,
/// applies the cookie XOR, and bounds-checks the address against the
/// Zone-0 user-VA window. Out-of-bounds and untouched (shared-zero)
/// entries decode to `(0, Empty)`. The load itself can never fault: the
/// pagemap reservation is committed \c PAGE_READONLY for process
/// lifetime.
///
/// \returns the routing tuple. Consumers gate on \c tag first.
[[nodiscard]] LIBC_INLINE PagemapDecoded pagemap_load_decoded(const void *addr) {
  if (LIBC_UNLIKELY(!internal::pagemap_addr_in_bounds(addr)))
    return PagemapDecoded{0, VaChunkConsumer::Empty};
  PagemapEntry *slot = internal::pagemap_slot_unchecked(addr);
  uint64_t enc = slot->encoded.load(cpp::MemoryOrder::ACQUIRE);
  return pagemap_decode(enc);
}

/// Wait-free membership probe. \returns \c true iff \p addr resolves to
/// a chunk currently tagged by some consumer (i.e. the decode is not
/// \c Empty). False for out-of-bounds, untouched, and retired entries.
[[nodiscard]] LIBC_INLINE bool pagemap_is_tracked(const void *addr) {
  PagemapDecoded d = pagemap_load_decoded(addr);
  return d.tag != VaChunkConsumer::Empty;
}

//===----------------------------------------------------------------------===//
// Public API — store
//===----------------------------------------------------------------------===//

/// Owner-side publish of a single chunk's routing tuple. One 8 B RELEASE
/// store; readers ACQUIRE-load and synchronise with this store. The
/// single-publisher invariant means no CAS is needed.
///
/// \pre The chunk owner has called \c pagemap_register_range covering
///      this entry's OS page; without it the page is still
///      \c PAGE_READONLY-shared-zero and the store will fault.
LIBC_INLINE void pagemap_store(void *chunk_aligned_addr, uint32_t slot_idx,
                                VaChunkConsumer tag) {
  PagemapEntry *slot = internal::pagemap_slot_unchecked(chunk_aligned_addr);
  uint64_t enc = pagemap_encode(slot_idx, tag);
  slot->encoded.store(enc, cpp::MemoryOrder::RELEASE);
}

/// Publish a pre-computed encoded word into a single entry. Useful when
/// callers stamp the same encoded word across multiple chunks of a
/// multi-chunk allocation — \c pagemap_encode runs once outside the
/// loop.
LIBC_INLINE void pagemap_store_encoded(void *chunk_aligned_addr,
                                       uint64_t encoded) {
  PagemapEntry *slot = internal::pagemap_slot_unchecked(chunk_aligned_addr);
  slot->encoded.store(encoded, cpp::MemoryOrder::RELEASE);
}

/// Stamp the same `(slot_idx, tag)` into every 64 KiB sub-entry covering
/// `[chunk_base, chunk_base + chunk_bytes)`. The encode runs once
/// outside the loop.
///
/// \pre Single-writer ownership of every entry in the range.
/// \pre \p chunk_bytes is a positive multiple of \c kPagemapChunkBytes.
/// \pre \p chunk_base is \c kPagemapChunkBytes-aligned.
/// \pre \c pagemap_register_range over the same range has succeeded;
///      otherwise the backing OS pages are still \c PAGE_READONLY.
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

/// Retire every entry in `[chunk_base, chunk_base + chunk_bytes)` by
/// storing literal zero. Zero decodes to `(0, Empty)` thanks to the
/// cookie's low-byte constraint; this is identical to the shared-zero
/// state of an untouched OS page.
///
/// \pre Single-writer ownership of every entry in the range.
/// \pre \p chunk_bytes is a multiple of \c kPagemapChunkBytes.
/// \pre \p chunk_base is \c kPagemapChunkBytes-aligned.
LIBC_INLINE void pagemap_retire_range(void *chunk_base, size_t chunk_bytes) {
  size_t n = chunk_bytes >> kPagemapShift;
  if (LIBC_UNLIKELY(n == 0))
    return;
  // Storing literal zero matches the untouched / shared-zero state of a
  // fresh OS page, and `0 ^ cookie ^ cookie == 0` keeps decode symmetric.
  // Encoding `(0, Empty)` explicitly would produce `cookie` itself, which
  // is wrong — readers expect the natural zero state.
  for (size_t i = 0; i < n; ++i) {
    void *entry_addr =
        static_cast<char *>(chunk_base) + (i * kPagemapChunkBytes);
    PagemapEntry *slot = internal::pagemap_slot_unchecked(entry_addr);
    slot->encoded.store(0, cpp::MemoryOrder::RELEASE);
  }
}

//===----------------------------------------------------------------------===//
// Public API — register / unregister
//===----------------------------------------------------------------------===//

/// Upgrade the pagemap OS pages covering
/// `[chunk_base, chunk_base + chunk_bytes)` from \c PAGE_READONLY (the
/// initial shared-zero state) to \c PAGE_READWRITE via
/// \c nt_pal::protect.
///
/// Idempotent: a per-OS-page atomic byte elides redundant syscalls after
/// the first successful upgrade. The byte is a performance optimisation
/// only — \c nt_pal::protect is idempotent at the OS level and a
/// redundant call is harmless.
///
/// Must be called by the chunk's owner BEFORE any \c pagemap_store on
/// the affected entries; a publish to a still-RO page would fault.
///
/// \returns 0 on success; \c -ENOMEM if \c protect fails; \c -EINVAL on
///          out-of-bounds or malformed range.
[[nodiscard]] int pagemap_register_range(void *chunk_base, size_t chunk_bytes);

/// Symmetry-only no-op kept at the consumer call site (\c buddy_arena's
/// FreeFn). Pagemap pages stay committed for the process lifetime — no
/// decommit path, no downgrade back to RO. Layer-3 sub-slab decommit
/// operates on chunk VA, not on pagemap VA. The snmalloc
/// \c notify_using_readonly PAL adopts the same register-only,
/// no-symmetric-unregister discipline.
LIBC_INLINE void pagemap_unregister_range(void * /*chunk_base*/,
                                           size_t /*chunk_bytes*/) {
}

//===----------------------------------------------------------------------===//
// Public API — typed descriptor load
//===----------------------------------------------------------------------===//

/// Per-consumer traits for typed descriptor lookup.
///
/// Each consumer specialises this template in its own header to declare
/// its descriptor record type and the sealed Zone-0 accessors for its
/// pool base and capacity. The pagemap layer never includes consumer
/// headers; the primary template is intentionally left undefined so a
/// consumer that forgets its specialisation gets an
/// "incomplete type" diagnostic at the typed-load call site, not a
/// silent fallback to a wrong pool.
///
/// A specialisation MUST expose:
///
/// \code{.cpp}
///   using Descriptor = ...;
///   static size_t pool_capacity();        // Zone-0-sealed slot count
///   static Descriptor *pool_base();       // Zone-0-sealed pool VA
/// \endcode
///
/// \c pool_capacity and \c pool_base MUST read from PCB Zone 0 (sealed
/// \c PAGE_READONLY for process lifetime). Backing them on a mutable BSS
/// pointer would let an arbitrary-write attacker redirect every typed
/// load with one store — exactly the hazard the pagemap's cookie XOR
/// already defends against.
template <VaChunkConsumer Tag> struct PagemapTraits;

/// Wait-free typed descriptor lookup.
///
/// Decodes the entry covering \p addr, verifies the tag matches \c Tag,
/// bounds-checks \c slot_idx against the consumer's pool capacity, and
/// indexes into the pool. Non-faulting on any address in the user-VA
/// window — both the pagemap reservation and the consumer's pool VA are
/// sealed at Tier A. Safe from SIGSEGV, debugger probe, and
/// \c __cxa_finalize contexts.
///
/// \returns the typed descriptor pointer, or \c nullptr when the entry
///          is out of bounds, untouched, tagged for a different
///          consumer, or carries a slot index past the pool capacity
///          (corruption or a forged pagemap word).
///
/// The caller still owns the consumer-specific cross-check (canary,
/// geometry, generation) before dereferencing payload fields.
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

//===----------------------------------------------------------------------===//
// Tier-A bootstrap
//===----------------------------------------------------------------------===//

/// Tier-A init entry point. Registered via \c LIBC_REGISTER_MEMORY_PRIMITIVE
/// at phase 3 (after PAL = 0, va_substrate = 1, mapping_table = 2).
/// Reserves the backing array as \c PAGE_READONLY-stay-committed, draws
/// the pagemap cookie from \c ProcessPrng (redrawing until the low byte
/// is zero, with expected ~1.004 draws), stamps PCB Zone 0, and emits
/// Receipts so Pass 2 marks the reservations as libc-internal.
///
/// \param[out] out  Receipt array sized to at least \p cap entries.
/// \param      cap  Receipt array capacity.
/// \returns the number of Receipts written.
uint32_t pagemap_init_fn(::LIBC_NAMESPACE::internal::Receipt *out, uint32_t cap);

/// Fork-child reinit hook. The pagemap reservation survives via CoW:
/// shared-zero RO PTEs inherit verbatim and the touched RW pages CoW on
/// first child write. The cookie is fork-stable through the sealed
/// Zone 0 inheritance. Registered for completeness; current
/// implementation is an intentional no-op.
void pagemap_fork_reinit();

} // namespace alloc
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif
