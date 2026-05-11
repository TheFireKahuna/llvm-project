//===-- .libcmem section registry — unified memory-primitive bootstrap ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each libc memory-primitive subsystem (VaSubstrate, MappingTable, future
// per-CPU arenas, ...) installs one MemPrimitive record into
// `.libcmem$P<phase>`. Tier A Phase 0c.5 sweeps the section via
// `memory_primitives_startup_init()`:
//
//   Pass 1: for each handler in phase order ($P0 → $P9), `init_fn`
//           brings its subsystem up and may emit zero or more Receipts
//           into the walker's stack-local buffer. A handler at phase N
//           may consume any subsystem whose handler ran in phase < N.
//   Pass 2: every emitted Receipt is registered into the now-live mapping
//           table as LIBC_INTERNAL.
//
// Current phase assignment (unified-memory-bootstrap.md §Tiers):
//
//   P1: va_substrate  — reserves the three class arenas via raw NT,
//                       seeds Zone 0 substrate secrets, emits one Receipt
//                       per arena.
//   P2: mapping_table — ensure_init(): allocates L1 / remap-guard /
//                       RegionPool sentinels, flips the table's public
//                       readiness flag. Emits zero Receipts (the table's
//                       own backing is internal bookkeeping, not an
//                       observable mmap range).
//   P3: (reserved)    — placeholder for future primitives that need to
//                       run after the mapping table is up.
//
// Why the phased shape (see `unified-memory-bootstrap.md` for the full
// rationale):
//
// - Link-time order enforcement. lld-link merges `$P1 $P2 $P3 ...`
//   alphabetically, so a handler in phase N cannot silently drift ahead
//   of a dependency in phase M<N even if contributor TUs are reordered
//   or rebuilt. Ordering violations would require editing phase tags
//   and become visible at code-review time.
//
// - Receipts are stack-local to the walker — no cross-function state
//   sharing, no shared globals bridging Pass 1 and Pass 2.
//
// - The mapping table is atomic-complete at its first observable moment:
//   all Pass 1 handlers return before any Pass 2 registration fires, so
//   the table goes from "empty" to "all internal regions stamped" in one
//   `for` loop with no intermediate observable state.
//
// - `.libcmem$*` is hardware-sealed `PAGE_READONLY` after Tier A (same
//   protocol as `.libcveh`) — the dispatch table cannot be repointed
//   post-seal by an arbitrary-write primitive.
//
// - Reverse-phase teardown is free: when a unified memory fini lands, it
//   walks `libc_libcmem_registry().reverse()` the same way `.libcfin`
//   does. Paired init+fini registration with matching phase tags keeps
//   the three primitives in lockstep.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MEMORY_PRIMITIVES_BOOTSTRAP_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MEMORY_PRIMITIVES_BOOTSTRAP_H

#include "hdr/stdint_proxy.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/section_registry.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

#include <stddef.h>

namespace LIBC_NAMESPACE_DECL {
namespace internal {

// Tag for diagnostics and for the mapping table's per-region kind field.
// Keep narrow (uint8_t) — stored inline in each Receipt. Extend as new
// primitives register.
enum class InternalKind : uint8_t {
  Unknown = 0,
  SubstrateSeedSmall,
  SubstrateSeedMedium,
  SubstrateSeedLarge,
  SubstrateSeedXLarge,
  SubstrateSeedHuge,
  // ThreadScratch's bootstrap-tier (first-thread) scratch arena. The
  // arena VA is owned by ThreadScratch (raw NT reservation, freed at
  // thread exit), but the receipt is stamped LIBC_INTERNAL by the
  // walker's Pass 2 so /proc-style introspection treats it consistently
  // with all other libc-internal VA. See thread_scratch.cpp's bootstrap
  // path for the queueing protocol.
  BootstrapScratch,
  // Substrate-served arena reserved while the mapping table was still
  // INIT_IN_PROGRESS (so inline `register_mapping_internal` would have
  // self-deadlocked on `ensure_init`). The substrate's `reserve_new_arena`
  // auto-enqueues the receipt; the walker's `mapping_table_init_fn`
  // harvests every queued receipt after `ensure_init` returns INIT_READY.
  SubstrateArena,
  // Layer 2 flat 16 B / 16 KiB chunkmap backing reservation. One large
  // `MEM_RESERVE | MEM_WRITE_WATCH` range covering the user VA window
  // (e.g. ~128 GiB on a 47-bit address space). Per-page commit happens
  // on demand via `pagemap_register_range` from the allocator hot path;
  // physical backing is reclaimed on empty via the write-watch driven
  // `pagemap_unregister_range`. Stamped LIBC_INTERNAL by Pass 2.
  Pagemap,
  // Layer 2 lock-free NBALLOC chunk broker. Three reservations per
  // partition: (1) the 4 GiB compact-pointer partition VA itself (eager
  // VA, lazy commit per-chunk via `commit_replace`); (2) the per-arena
  // NBALLOC tree storage (~512 KiB; lazy commit on first allocation);
  // (3) the BuddyChunkDescriptor pool (~8 MiB eager commit). All three
  // stamped LIBC_INTERNAL by Pass 2 so the SIGSEGV classifier and
  // `is_libc_pointer` route correctly. See alloc/buddy_arena.{h,cpp}.
  BuddyArena,
  // Layer 7 partition layer reservations. Three reservation kinds emit
  // this tag from `partition_init_fn`: (1) the 256 KiB CoarsePagemap
  // backing — flat partition-granularity (4 GiB stride) lookup index
  // covering the full user-VA range, lazy-committed per touched OS page;
  // (2) the 32 KiB descriptor pool — fixed array of 256 PartitionDescriptor
  // (128 B cache-line-pair); (3) each demand-reserved 4 GiB partition
  // (eager-reserved at Tier A for the 12 core libc-internal classes;
  // demand-reserved per (class, numa_node) for user-facing allocator
  // classes). Stamped LIBC_INTERNAL by Pass 2 so the SIGSEGV classifier
  // and `is_libc_pointer` route correctly. See alloc/partition.{h,cpp}.
  Partition,
  // Layer 1 va_tracker — Phase 1 P1.G. Receipt kinds emitted from
  // `va_tracker_init_fn`: (1) the eager 128-arena head storage (one
  // bucket-3 chunk pre-committed in the SkiplistNode partition) for the
  // skiplist sentinel heads; (2) the ART root Node256 reservation in the
  // ArtNode partition; (3) the ART/SkiplistNode/RegionDesc partition VA
  // windows themselves (eager-reserved through `partition::reserve_or_grow`
  // at init so the SIGSEGV-callable `resolve()` does not block on
  // partition-layer reservation work). Stamped LIBC_INTERNAL by Pass 2 so
  // the va_tracker's own backing VA is correctly classified as
  // libc-internal (not POSIX-visible). See memory/va_tracker.{h,cpp}.
  VaTracker,
};

// One VA range that a Pass 1 handler wants published into the mapping
// table as LIBC_INTERNAL. Plain POD; populated on the walker's stack.
struct Receipt {
  void *base;
  size_t size;
  InternalKind kind;
};

// Signature for per-subsystem Pass 1 work. Populates up to `cap` receipts
// starting at `out`, returns the number actually written. A handler that
// installs internal state but has nothing the mapping table should track
// (e.g. the table's own L1/L2/L3 backing) returns 0.
//
// Trap on failure — libc init has no useful partial-recovery mode, and
// the bootstrap is single-threaded so no unwind coordination is needed.
using MemPrimitiveInitFn = uint32_t (*)(Receipt *out, uint32_t cap);

// One entry per subsystem. Lives in `.libcmem$M` as `const` storage.
struct MemPrimitiveInitEntry {
  MemPrimitiveInitFn init_fn;
  const char *name; // stable string literal for diagnostics
};

// Maximum receipts the walker's stack buffer can hold across every
// handler in one bootstrap. Primitives are a closed set; overflow is a
// build-time bug, not a runtime condition. Sized generously — current
// usage is 3 (substrate S/M/L); bump if a future primitive adds a batch.
inline constexpr uint32_t kMemPrimitiveReceiptCap = 32;

// Tier A Phase 0c.5 walker. Called once from __libc_bootstrap(). Must
// run AFTER pcb_startup_init() — each init_fn may read Zone 0 / Zone 0b
// fields — and BEFORE pcb_seal_readonly_a() so any PCB writes a handler
// performs (substrate secrets, substrate root, etc.) land before seal.
//
// Performs the read-only audit on `.libcmem$*` before dispatching, same
// protection regime as `register_all_static_veh_filters()`.
//
// Trap on any handler or registration failure — see MemPrimitiveInitFn.
void memory_primitives_startup_init();

// "Mapping table is up and accepting `register_mapping_internal` from
// any thread without risk of self-deadlock on a same-thread reentrant
// `ensure_init`." Latched exactly once, at the end of the walker's
// Pass 2 stamping loop — that is, after `mapping_table_init_fn` has
// driven `ensure_init` to INIT_READY AND every Pass 1 receipt has been
// stamped. Until that moment, callers that reserve VA during Tier A
// (substrate seed arenas, ThreadScratch's bootstrap-tier first thread)
// must defer registration via the Receipt mechanism rather than
// stamping inline; an inline stamp during INIT_IN_PROGRESS would
// reenter `ensure_init` on the same thread and deadlock at the
// `init_state_.wait(INIT_IN_PROGRESS)` futex.
//
// Defined in memory_primitives_bootstrap.cpp; the inline accessors
// below give every TU that includes this header zero-overhead read /
// write access without exposing the atomic by raw name.
extern cpp::Atomic<bool> g_mapping_table_ready;

[[nodiscard]] LIBC_INLINE bool is_mapping_table_ready() {
  return g_mapping_table_ready.load(cpp::MemoryOrder::ACQUIRE);
}

LIBC_INLINE void mark_mapping_table_ready() {
  g_mapping_table_ready.store(true, cpp::MemoryOrder::RELEASE);
}

// Atomically latch ready=true and stamp every internal-region receipt
// that queued up during the walker's Pass 2 stamp loop (Pass 2's
// `register_mapping_internal` calls can transitively invoke
// substrate's `slow_acquire_arena`, whose pre-INIT_READY auto-enqueue
// would otherwise leave late-comer arenas unregistered). Defined in
// mapping_table.cpp because it must call register_mapping_internal on
// the drained receipts. Replaces the bare `mark_mapping_table_ready`
// at the end of Pass 2.
void mapping_table_finalize_init();

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

// Register a Pass 1 handler into `.libcmem$P<phase>`. `phase` must
// resolve to a literal digit 0..9 after preprocessor expansion; see
// LIBC_SECTION_REGISTER_PHASED for the contract. `tag` must be unique
// across the link (duplicates produce a linker duplicate-symbol error).
//
// A handler at phase N may consume any subsystem brought up by a handler
// in phase < N. Within a phase, merge order is undefined — document
// intra-bucket order independence or split across phases.
//
// Must expand at namespace scope.
//
// Example:
//
//   static uint32_t my_primitive_init_fn(
//       ::LIBC_NAMESPACE::internal::Receipt *out, uint32_t cap);
//
//   LIBC_REGISTER_MEMORY_PRIMITIVE(my_primitive, 3, &my_primitive_init_fn)
#define LIBC_REGISTER_MEMORY_PRIMITIVE(tag, phase, init_fn_value)              \
  LIBC_SECTION_REGISTER_PHASED(                                                \
      libcmem, ::LIBC_NAMESPACE::internal::MemPrimitiveInitEntry, phase, tag,  \
      {(init_fn_value), #tag})                                                 \
  extern "C" [[gnu::used]] void __libc_mem_anchor_##tag(void) {}

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_MEMORY_PRIMITIVES_BOOTSTRAP_H
