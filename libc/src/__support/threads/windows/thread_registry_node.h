//===-- ThreadRegistryNode — Crystalline-managed registry node ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Polymorphic Crystalline-W retirable header for the thread registry.
//
// One Crystalline domain manages three retirable kinds:
//   * Lifecycle      — per-thread `ThreadLifecycle` (also the Harris node
//                      for the global iter list).
//   * BucketHeadPage — page of `BucketEntry*` heads, demand-allocated as
//                      bucket-index space grows during linear-hash splits.
//   * BucketEntry    — `{task_id, ThreadLifecycle*, next}` Harris-list
//                      node living in a bucket chain.
//
// Why a single domain: keeps the per-thread Crystalline retire batch
// coherent across the three kinds (one drain emptied at thread exit,
// one global epoch advance amortized across all retires), and stays
// well within `kMaxCrystallineDomains` (the registry uses one slot,
// not three).
//
// The registry's FreeFn (`free_thread_registry_node`) dispatches on
// `kind` to release the correct underlying allocation. `kind` is
// stamped once at construction and never mutated thereafter — Crystalline
// reads it after the node is fully quiesced (no live readers), so a
// plain load suffices.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_NODE_H
#define LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_NODE_H

#include "hdr/stdint_proxy.h"
// Pull in the full CrystallineNode definition (not just the forward
// decl from crystalline_local_state.h) so ThreadRegistryNode can
// derive from it as a complete type.
#include "src/__support/OSUtil/windows/concurrent/crystalline_domain.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {

// Discriminator stored in `ThreadRegistryNode::kind`. Non-zero values
// so a zero-initialized header is recognizable as "not yet stamped"
// (the FreeFn traps on unrecognized kinds).
enum class ThreadRegistryNodeKind : uint8_t {
  Lifecycle = 1,
  BucketHeadPage = 2,
  BucketEntry = 3,
};

// Crystalline-managed retirable header. Lives at offset 0 of every
// retirable type so static_cast<NodeT*>(ThreadRegistryNode*) is a
// no-op pointer reinterpret.
//
// `kind` discriminates the dispatch in `free_thread_registry_node`.
// Set once at allocation; Crystalline's reclamation phase reads it
// without contention (the node is quiesced — no other thread holds
// a reservation).
struct ThreadRegistryNode : public concurrent::CrystallineNode {
  ThreadRegistryNodeKind kind;
  // Reserved padding so derived types have a predictable first-field
  // offset for layout math. Header total: 32 bytes (24 from
  // CrystallineNode + 1 + 7 padding).
  uint8_t _pad[7];
};

// Crystalline FreeFn for the thread-registry domain. Dispatches on
// `kind`:
//   * Lifecycle      → robust mutex destroy + slab pool free.
//   * BucketHeadPage → page_free.
//   * BucketEntry    → page_free (entries are page-allocated; a
//                      future slab pool can swap in without touching
//                      the dispatch table).
//
// Receives a node guaranteed by Crystalline reservation accounting
// to have no live readers — the underlying memory is exclusively
// the dispatcher's to release.
void free_thread_registry_node(ThreadRegistryNode *node);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_THREADS_WINDOWS_THREAD_REGISTRY_NODE_H
