//===-- Lock-free slab pool for ThreadSignalState ------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pool allocator for ThreadSignalState objects. Uses SlabPool in growing-slab
// mode. Per-slab freelists with thread ownership — alloc is zero atomics on
// the fast path.
//
// Signal pools use thread_local for the slab pointer (not FLS — avoids
// circular dependency with signal subsystem init). On thread exit, the slab
// is abandoned via the pool backpointer in the header.
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/signal_internal.h"
#include "src/__support/OSUtil/windows/alloc/slab_pool.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

namespace {

constexpr size_t kSlotAlign = alignof(ThreadSignalState);
constexpr size_t kSlotSize =
    (sizeof(ThreadSignalState) + kSlotAlign - 1) & ~(kSlotAlign - 1);

static_assert(kSlotSize >= sizeof(void *),
              "ThreadSignalState must be at least pointer-sized");

internal::SlabPool pool;

// Per-thread slab. Can't use FLS (circular dep with signal subsystem).
static thread_local internal::SlabPool::ThreadSlab my_slab = nullptr;

} // anonymous namespace

ThreadSignalState *pool_alloc() {
  // init() is idempotent — safe to call on every alloc.
  pool.init(kSlotSize, kSlotAlign);

  void *slot = internal::SlabPool::alloc(my_slab);
  if (!slot)
    slot = pool.alloc_slow(&my_slab);
  return static_cast<ThreadSignalState *>(slot);
}

void pool_free(ThreadSignalState *state) {
  internal::SlabPool::free(state);
}

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
