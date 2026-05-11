//===-- Global slab registry definition ------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single definition of the global SlabRegistry shared by all SlabPool
// instances. Zero-initialized (BSS). Three-level radix: L1 runtime-sized
// from PCB max_address and substrate-served (Small for 48-bit VA,
// XLarge for 57-bit VA); L2/L3 directory pages substrate-served from
// the Small class on demand per 4 TB / 4 GB region. No growth copies,
// no leaks.
//
// Fini placement at $P3 is load-bearing. Fini runs in reverse phase
// order (high → low), so:
//
//   $P4 fires first: file_pool, ofd_pool, mapping_table, thread_scratch
//                    tear down. Each calls into SlabPool::destroy /
//                    slab.free, which call back through this registry's
//                    `remove_range` / `contains`. Registry must be
//                    alive — and it is, because SlabRegistry::destroy
//                    is in $P3 (later in the reverse-walk).
//
//   $P3 fires next:  lifecycle (TLS teardown only), wait_slot (own
//                    freelist, no SlabPool), and us. None of these
//                    three depend on the others — within-phase order
//                    inside $P3 is safe.
//
//   $P2 fires next:  posix_alloc_fini calls flush_cache_bins →
//                    SlabPool::return_hardened_slot → validate_slot.
//                    validate_slot reads `slab_registry.slab_secret_`,
//                    which destroy() does NOT clear (the secret is a
//                    plain value, not part of the directory tree).
//                    return_hardened_slot then routes the slot via
//                    local_free or xthread — no registry calls. Safe.
//
//   $P1 fires last:  substrate destroy runs. By now every L1/L2/L3
//                    substrate slot has been released by us, so the
//                    arenas backing those slots have correct
//                    `live_count == 0` and the substrate's bookkeeping
//                    sweep finds nothing extraordinary.
//
// This placement guarantees clean teardown order without per-reader
// null guards on `l1_` (which would add a hot-path branch).
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/alloc/legacy/slab_pool.h"
#include "src/__support/OSUtil/windows/libc_fini_registry.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {

SlabRegistry slab_registry = {};

static void slab_registry_fini() { slab_registry.destroy(); }

} // namespace internal
} // namespace LIBC_NAMESPACE_DECL

LIBC_REGISTER_FINI(3, slab_registry,
                   &::LIBC_NAMESPACE::internal::slab_registry_fini)
