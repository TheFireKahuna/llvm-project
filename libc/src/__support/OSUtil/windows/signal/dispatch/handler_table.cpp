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

// Reset handler to SIG_DFL under lock. Used by SA_RESETHAND and deferred_reset.
static void reset_to_default_locked(signal_state::SignalHandlerState &handler,
                                    int signum) {
  handler.handlers[signum].sa_handler = SIG_DFL;
  handler.handlers[signum].sa_flags = 0;
  __builtin_memset(&handler.handlers[signum].sa_mask, 0,
                   sizeof(handler.handlers[signum].sa_mask));
  uint64_t bit = 1ULL << (signum - 1);
  handler.ignored.fetch_and(~bit, cpp::MemoryOrder::RELEASE);
  handler.custom.fetch_and(~bit, cpp::MemoryOrder::RELEASE);
}

// Update fast-path bitmasks under lock. Must be called while the handler lock is held
// so the handler table and bitmasks are consistent from any reader's view.
static void update_bitmasks_locked(signal_state::SignalHandlerState &handler,
                                   int signum,
                                   const struct sigaction *act) {
  uint64_t bit = 1ULL << (signum - 1);
  if (act->sa_handler == SIG_IGN) {
    handler.ignored.fetch_or(bit, cpp::MemoryOrder::RELEASE);
    handler.custom.fetch_and(~bit, cpp::MemoryOrder::RELEASE);
  } else if (act->sa_handler == SIG_DFL) {
    handler.ignored.fetch_and(~bit, cpp::MemoryOrder::RELEASE);
    handler.custom.fetch_and(~bit, cpp::MemoryOrder::RELEASE);
  } else {
    handler.ignored.fetch_and(~bit, cpp::MemoryOrder::RELEASE);
    handler.custom.fetch_or(bit, cpp::MemoryOrder::RELEASE);
  }
}

struct sigaction read_and_consume(int signum) {
  auto &handler = g_pcb.signal_handler;

  handler.lock.lock();
  struct sigaction action = handler.handlers[signum];

  // SA_RESETHAND: atomically reset disposition to SIG_DFL.
  if (action.sa_flags & SA_RESETHAND)
    reset_to_default_locked(handler, signum);
  handler.lock.unlock();

  return action;
}

struct sigaction read(int signum) {
  auto &handler = g_pcb.signal_handler;

  handler.lock.lock();
  struct sigaction action = handler.handlers[signum];
  handler.lock.unlock();

  return action;
}

struct sigaction write(int signum, const struct sigaction *new_act) {
  auto &handler = g_pcb.signal_handler;

  handler.lock.lock();
  struct sigaction old = handler.handlers[signum];
  handler.handlers[signum] = *new_act;
  update_bitmasks_locked(handler, signum, new_act);
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
