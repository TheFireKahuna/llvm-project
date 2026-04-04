//===-- Shared IOCP completion router multiplexer --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/reactor/completion_router.h"

#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/reactor/reactor.h"

namespace LIBC_NAMESPACE_DECL {
namespace internal {
namespace completion_router {

// Small fixed-size table. The library has a tiny number of non-reactor
// completion sources; 4 slots is ample. Bump if a fifth subsystem appears.
static constexpr unsigned MAX_SENTINELS = 4;

struct SentinelEntry {
  PVOID key;
  Handler handler;
};

static SentinelEntry g_sentinels[MAX_SENTINELS];
static cpp::Atomic<unsigned> g_sentinel_count{0};
static cpp::Atomic<uintptr_t> g_default_handler{0};
static cpp::Atomic<uint32_t> g_installed{0};

// Single installed router — dispatches by key, falling back to the
// default handler for unknown keys.
static void multiplex_router(PVOID key, PVOID apc_context, NTSTATUS status,
                             ULONG_PTR information) {
  unsigned count = g_sentinel_count.load(cpp::MemoryOrder::ACQUIRE);
  for (unsigned i = 0; i < count; ++i) {
    if (g_sentinels[i].key == key) {
      g_sentinels[i].handler(key, apc_context, status, information);
      return;
    }
  }
  auto def = g_default_handler.load(cpp::MemoryOrder::ACQUIRE);
  if (def)
    reinterpret_cast<Handler>(def)(key, apc_context, status, information);
}

static void ensure_installed() {
  uint32_t expected = 0;
  if (g_installed.compare_exchange_strong(expected, 1,
                                          cpp::MemoryOrder::ACQ_REL,
                                          cpp::MemoryOrder::RELAXED))
    reactor::set_completion_router(multiplex_router);
}

void install_sentinel_handler(PVOID sentinel, Handler h) {
  ensure_installed();

  // Idempotence: scan for an existing entry with the same key+handler.
  // Registration happens serially during init; this is a cheap O(n).
  unsigned count = g_sentinel_count.load(cpp::MemoryOrder::ACQUIRE);
  for (unsigned i = 0; i < count; ++i) {
    if (g_sentinels[i].key == sentinel && g_sentinels[i].handler == h)
      return;
  }
  if (count >= MAX_SENTINELS)
    return; // Silent drop — registration table full. Bump MAX_SENTINELS.

  // Write entry fully before publishing by incrementing count.
  g_sentinels[count].key = sentinel;
  g_sentinels[count].handler = h;
  g_sentinel_count.store(count + 1, cpp::MemoryOrder::RELEASE);
}

void install_default_handler(Handler h) {
  ensure_installed();
  g_default_handler.store(reinterpret_cast<uintptr_t>(h),
                          cpp::MemoryOrder::RELEASE);
}

} // namespace completion_router
} // namespace internal
} // namespace LIBC_NAMESPACE_DECL
