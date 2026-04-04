//===-- Signal handler table implementation ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "src/__support/OSUtil/windows/signal/dispatch/handler_table.h"

#include "hdr/signal_macros.h"
#include "src/__support/CPP/atomic.h"
#include "src/__support/OSUtil/windows/process_control_block.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {
namespace handler_table {

// ---------------------------------------------------------------------------
// HandlerEntry view — reinterprets a raw struct sigaction slot as HandlerEntry.
//
// HandlerEntry wraps struct sigaction (same layout, no extra fields), so this
// reinterpret is safe and avoids copying into a temporary.
// ---------------------------------------------------------------------------
static HandlerEntry &entry_of(SignalHandlerState &handler, int signum) {
  static_assert(sizeof(HandlerEntry) == sizeof(struct sigaction),
                "HandlerEntry must be layout-identical to struct sigaction");
  return reinterpret_cast<HandlerEntry &>(handler.handlers[signum]);
}

// Update fast-path bitmasks from a HandlerEntry's disposition. Must be called
// while the handler lock is held so the table and bitmasks stay consistent.
//
// Uses RELAXED load + RELEASE store instead of fetch_or/fetch_and: under the
// lock, no concurrent writer can modify these bitmasks, so a plain
// read-modify-store is correct and avoids 2-3 atomic RMW cache-line bounces
// per sigaction call. The RELEASE store publishes to lock-free readers (VEH,
// console transport) that load with ACQUIRE.
static void sync_bitmasks_locked(SignalHandlerState &handler, int signum,
                                 const HandlerEntry &entry) {
  uint64_t bit = 1ULL << (signum - 1);
  uint64_t ign = handler.ignored.load(cpp::MemoryOrder::RELAXED);
  uint64_t cust = handler.custom.load(cpp::MemoryOrder::RELAXED);

  switch (entry.disposition()) {
  case HandlerEntry::Disposition::Ignored:
    ign |= bit;
    cust &= ~bit;
    break;
  case HandlerEntry::Disposition::Default:
    ign &= ~bit;
    cust &= ~bit;
    break;
  case HandlerEntry::Disposition::Custom:
    ign &= ~bit;
    cust |= bit;
    break;
  }

  handler.ignored.store(ign, cpp::MemoryOrder::RELEASE);
  handler.custom.store(cust, cpp::MemoryOrder::RELEASE);
}

// Reset handler to SIG_DFL under lock and sync bitmasks.
// Used by SA_RESETHAND and deferred_reset.
static void reset_to_default_locked(SignalHandlerState &handler, int signum) {
  HandlerEntry &entry = entry_of(handler, signum);
  entry.reset();
  sync_bitmasks_locked(handler, signum, entry);
}

struct sigaction read_and_consume(int signum) {
  auto &handler = g_pcb.signal_handler;

  handler.lock.lock();
  HandlerEntry &entry = entry_of(handler, signum);
  struct sigaction action = entry; // conversion operator

  // SA_RESETHAND: atomically reset disposition to SIG_DFL.
  if (entry.has_resethand())
    reset_to_default_locked(handler, signum);
  handler.lock.unlock();

  return action;
}

struct sigaction read(int signum) {
  auto &handler = g_pcb.signal_handler;

  handler.lock.lock();
  struct sigaction action = entry_of(handler, signum); // conversion operator
  handler.lock.unlock();

  return action;
}

struct sigaction write(int signum, const struct sigaction *new_act) {
  auto &handler = g_pcb.signal_handler;

  handler.lock.lock();
  HandlerEntry &entry = entry_of(handler, signum);
  struct sigaction old = entry; // conversion operator — snapshot before write
  entry = *new_act;            // assignment operator — typed copy
  sync_bitmasks_locked(handler, signum, entry);
  handler.lock.unlock();

  return old;
}

void deferred_reset(int signum) {
  auto &handler = g_pcb.signal_handler;

  handler.lock.lock();
  reset_to_default_locked(handler, signum);
  handler.lock.unlock();
}

} // namespace handler_table
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL
