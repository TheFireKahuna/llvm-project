//===-- SA_RESTART stack (Layer 3) ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Per-thread restart state for SA_RESTART.
//
// When signals nest (handler A is interrupted by handler B), each level
// pushes its own SA_RESTART disposition. I/O paths read the top of stack.
// The outer handler's restart flag is preserved across inner deliveries.
//
// Implementation: compact bitfield (uint32_t) stores one restart bit per
// nesting level, supporting 32 levels in 5 bytes total. 32 levels exceeds
// any realistic nesting — alt-stack physical limits cap nesting around
// 8-16 frames, and main stack nesting is bounded by stack size.
//
// Overflow safety: the dispatch engine checks can_push() before invoking
// a handler. At max depth, the signal stays pending and is delivered when
// nesting unwinds — signals are deferred, never lost or desynchronized.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_RESTART_STATE_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_RESTART_STATE_H

#include "src/__support/libc_assert.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include <stdint.h>

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

inline constexpr uint8_t MAX_SIGNAL_NESTING = 32;

struct RestartState {
  uint8_t depth;     // Current nesting level (0 = not in any handler).
  uint32_t flags;    // Bit i = SA_RESTART disposition for nesting level i.
};

namespace signal_restart {

// Check whether another handler invocation can be tracked. The dispatch
// engine must call this before invoke_handler_impl. If false, the signal
// should stay pending — it will be delivered when an outer handler returns
// and pop() frees a slot.
LIBC_INLINE bool can_push(const RestartState &rs) {
  return rs.depth < MAX_SIGNAL_NESTING;
}

// Push a restart flag before handler invocation.
// Caller must verify can_push() first — push at max depth is a logic error.
LIBC_INLINE void push(RestartState &rs, bool sa_restart) {
  LIBC_ASSERT(rs.depth < MAX_SIGNAL_NESTING &&
              "restart push at max nesting depth");
  if (sa_restart)
    rs.flags |= (1U << rs.depth);
  else
    rs.flags &= ~(1U << rs.depth);
  ++rs.depth;
}

// Pop after handler returns. Guard against underflow — longjmp from a
// signal handler can skip the pop, leaving depth at 0 when the dispatch
// engine attempts cleanup.
LIBC_INLINE void pop(RestartState &rs) {
  if (LIBC_LIKELY(rs.depth > 0))
    --rs.depth;
}

// Query from I/O paths after APC wake.
LIBC_INLINE bool should_restart(const RestartState &rs) {
  if (rs.depth == 0)
    return false;
  return (rs.flags & (1U << (rs.depth - 1))) != 0;
}

} // namespace signal_restart
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_RESTART_STATE_H
