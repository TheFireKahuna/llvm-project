//===- arena_alloc.h - va_tracker Arena allocator cross-TU hooks ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cross-TU init / fork / introspection hooks for the va_tracker's
/// Arena allocator.
///
/// The public allocation surface (\c arena_alloc, \c arena_retire,
/// \c arena_free) lives in \c interval_skiplist.h; \c arena_alloc.cpp
/// owns the Crystalline-W domain instance, the \c VaTrackerArena
/// partition's per-class chunk_table, the per-slot init that follows
/// \c va_chunk_acquire_slot, and the FreeFn that returns slots to the
/// shared \c VaChunkDesc pool. This header surfaces only the plumbing
/// that the orchestrating \c interval_skiplist.cpp and the master
/// fork-reinit hook need to call across the TU boundary.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ARENA_ALLOC_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ARENA_ALLOC_H

#include "hdr/stdint_proxy.h"
#include "src/__support/OSUtil/windows/memory/va_tracker_chunk.h"
#include "src/__support/macros/attributes.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace windows {
namespace va_tracker {

/// One-shot registration of the Arena Crystalline-W domain.
///
/// Called from \c interval_skiplist_init after \c va_chunk_init has
/// brought the shared chunk pool online. Idempotent only in the sense
/// that the domain's own \c init_registration() guards against repeat
/// entry; callers must not race two Tier A init paths.
void arena_init_registration();

/// Visitor signature used by \c arena_fork_reinit_phase to walk every
/// reachable Arena chunk descriptor in the child after fork. The
/// master reinit hook uses this callback to build its leaked-descriptor
/// reclaim bitmap. \p ctx is forwarded verbatim from
/// \c arena_fork_reinit_phase.
using ArenaForkChunkVisitor = void (*)(VaChunkDesc *cd, void *ctx);

/// Fork-reinit phase for the Arena subsystem. Runs in the child after
/// \c RtlCloneUserProcess and must complete before any other thread
/// reaches an Arena code path.
///
/// Three independent repairs:
///   * Clears every per-slot Crystalline pin via the domain's
///     \c clear_all so stale parent-era pins do not block retire.
///   * Refreshes every reachable slot's per-chunk and per-slot canary
///     against the just-rotated \c partition_secret (the parent's
///     canaries are now treated as the attacker's; defence-in-depth
///     against an attacker who controlled parent state across the
///     fork).
///   * Scrubs stale \c LOCKED state on each Arena head's level-0 link;
///     an Arena head must never carry \c LOCKED, but a corrupted
///     pre-fork state would otherwise park a future contender forever.
///
/// \p visit is invoked once for each non-null \c VaChunkDesc * in the
/// Arena chunk_table; pass nullptr if no per-chunk visit is needed.
void arena_fork_reinit_phase(ArenaForkChunkVisitor visit, void *ctx);

/// Returns a relaxed snapshot of the live-Arena count, for
/// \c stats_snapshot. May lag concurrent allocate/free traffic; never
/// negative.
[[nodiscard]] uint32_t arena_live_count();

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ARENA_ALLOC_H
