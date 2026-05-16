//===- arena_alloc.h - va_tracker Arena allocator cross-TU hooks ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cross-TU init / fork / introspection hooks for the Arena allocator. The
// allocation surface (arena_alloc / arena_retire / arena_free) is declared
// in interval_skiplist.h alongside the Arena type.
//
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

// Tier A bring-up only; not safe to race with another init path.
void arena_init_registration();

// Visitor for arena_fork_reinit_phase; invoked once per live VaChunkDesc
// reachable from the Arena chunk table. ctx is forwarded verbatim.
using ArenaForkChunkVisitor = void (*)(VaChunkDesc *cd, void *ctx);

// Runs in the child after RtlCloneUserProcess and must complete before any
// other thread reaches Arena code. Clears every Crystalline pin (parent-era
// pins would block retire), rotates per-chunk / per-slot canaries against
// the freshly-rotated partition_secret, and scrubs any LOCKED bit left on
// an Arena head's level-0 link — heads are never LOCKED in steady state,
// but a corrupted pre-fork state would park future contenders forever.
// visit fires once per non-null chunk descriptor; pass nullptr to skip.
void arena_fork_reinit_phase(ArenaForkChunkVisitor visit, void *ctx);

// Relaxed snapshot for stats_snapshot. May lag concurrent traffic.
[[nodiscard]] uint32_t arena_live_count();

} // namespace va_tracker
} // namespace windows
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_MEMORY_ARENA_ALLOC_H
