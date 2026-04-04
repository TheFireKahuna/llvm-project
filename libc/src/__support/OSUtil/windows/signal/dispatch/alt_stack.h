//===-- Alt-stack space check (Layer 3) ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Two checks:
//   1. Alt-stack: if SA_ONSTACK and already on alt-stack, measure SP distance
//      to alt-stack base. Return false if < SIGNAL_STACK_HEADROOM.
//   2. Main stack: read TEB stack limit (gs:0x10), measure SP - limit.
//
// Both are VEH-safe (no API calls, no allocation).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_ALT_STACK_H
#define LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_ALT_STACK_H

#include "hdr/signal_macros.h"
#include "hdr/types/struct_sigaction.h"
#include "src/__support/OSUtil/windows/signal/signal_types.h"
#include "src/__support/macros/config.h"
#include "src/__support/macros/optimization.h"

#include "hdr/stdint_proxy.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

namespace signal_stack {

// Read the TEB stack limit. VEH-safe — no API call, just a TEB read.
LIBC_INLINE uintptr_t get_stack_limit() {
  uintptr_t limit;
#ifdef __x86_64__
  __asm__ __volatile__("movq %%gs:0x10, %0" : "=r"(limit));
#elif defined(__aarch64__)
  __asm__ __volatile__("ldr %0, [x18, #0x10]" : "=r"(limit));
#else
#error "Unsupported architecture"
#endif
  return limit;
}

// Read the current stack pointer.
LIBC_INLINE uintptr_t get_sp() {
  uintptr_t sp;
#ifdef __x86_64__
  __asm__ __volatile__("mov %%rsp, %0" : "=r"(sp));
#elif defined(__aarch64__)
  __asm__ __volatile__("mov %0, sp" : "=r"(sp));
#else
#error "Unsupported architecture"
#endif
  return sp;
}

// Check if there's enough space on the alt-stack for another handler frame.
LIBC_INLINE bool has_alt_stack_space(const ThreadSignalState *state) {
  if (!state || !(state->alt_stack_flags & SS_ONSTACK))
    return true; // Not on alt-stack — caller should check main stack.

  uintptr_t sp = get_sp();
  uintptr_t base = reinterpret_cast<uintptr_t>(state->alt_stack_sp);
  uintptr_t top = base + state->alt_stack_size;

  // Alt-stack occupies [base, base + size). SP grows downward from top.
  // Reject SP outside the alt-stack bounds entirely (corruption/overflow).
  if (sp <= base || sp > top)
    return false;

  return (sp - base) >= SIGNAL_STACK_HEADROOM;
}

// Check if the main thread stack has enough headroom.
LIBC_INLINE bool has_main_stack_space() {
  uintptr_t low = get_stack_limit(); // TEB StackLimit
  uintptr_t sp = get_sp();
  if (sp <= low)
    return false;
  return (sp - low) >= SIGNAL_STACK_HEADROOM;
}

// Combined check: can we safely invoke a signal handler?
// Checks alt-stack if SA_ONSTACK and currently on alt-stack, else main stack.
LIBC_INLINE bool has_stack_space(const ThreadSignalState *state,
                                 const struct sigaction *act) {
  if (state && (act->sa_flags & SA_ONSTACK) &&
      !(state->alt_stack_flags & SS_DISABLE)) {
    if (state->alt_stack_flags & SS_ONSTACK) {
      // Already on alt-stack — measure actual remaining space.
      return has_alt_stack_space(state);
    }
    // Not yet on alt-stack — full alt-stack size is available.
    return state->alt_stack_size >= SIGNAL_STACK_HEADROOM;
  }

  return has_main_stack_space();
}

// Simple guard for dispatch loop: can we invoke any handler at all?
// Used when we don't yet know which handler (and thus which sa_flags) we'll
// invoke. Conservative: checks current stack context.
LIBC_INLINE bool can_dispatch(const ThreadSignalState *state) {
  if (state && (state->alt_stack_flags & SS_ONSTACK))
    return has_alt_stack_space(state);
  return has_main_stack_space();
}

} // namespace signal_stack
} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC___SUPPORT_OSUTIL_WINDOWS_SIGNAL_DISPATCH_ALT_STACK_H
