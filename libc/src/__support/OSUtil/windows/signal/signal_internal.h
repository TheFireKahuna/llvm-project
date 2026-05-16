//===-- Windows signal subsystem internal API ----------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Internal types and function declarations shared between the signal subsystem
// .cpp files and signal entrypoints. Not for use outside libc/src/signal/.
//
// Cross-process ABI types (ChildStateBlock, Reserved2Ext), stack inspection
// helpers, and pool/context declarations. GlobalSignalState has been dissolved
// into ProcessControlBlock as canonical typed state blocks in the PCB.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_INTERNAL_H
#define LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_INTERNAL_H

#include "src/__support/OSUtil/windows/signal/signal.h"
#include "src/__support/OSUtil/windows/alloc/legacy/section_region.h"
#include "hdr/types/struct_sigaction.h"
#include "src/__support/macros/config.h"

namespace LIBC_NAMESPACE_DECL {
namespace signal_state {

// ---------------------------------------------------------------------------
// Cross-process shared memory structures (stable layout)
// ---------------------------------------------------------------------------

// Parent-child shared notification block (anonymous section, stable layout).
//
// Mapped between parent and child via posix_spawn's lpReserved2 protocol.
// The parent creates the section and maps it read-write; the child inherits
// the section handle and maps it on startup (init_inherited_child_state).
//
// Fields:
//   state      — stop/continue lifecycle (CHILD_STATE_RUNNING/STOPPED/CONTINUED)
//   exec_count — incremented by the child on each exec. The parent reads this
//                to enforce POSIX setpgid semantics (EACCES after exec).
//                For posix_spawn children this is 1 at startup (exec already
//                happened). For future fork children it starts at 0 and
//                increments when the child calls execve.
struct ChildStateBlock {
  cpp::Atomic<int> state;
  cpp::Atomic<int> exec_count;
};

inline constexpr int CHILD_STATE_RUNNING = 0;
inline constexpr int CHILD_STATE_CONTINUED = -1;

// lpReserved2 extension layout for posix_spawn.
inline constexpr DWORD LLVM_LIBC_RESERVED2_MAGIC = 0x4C4C5643; // 'CVLL' LE
inline constexpr DWORD SPAWN_ATTR_SETSIGMASK = 0x08;
inline constexpr DWORD SPAWN_ATTR_SETSIGDEF = 0x04;
inline constexpr DWORD SPAWN_ATTR_SETSID = 0x40;

struct Reserved2Ext {
  DWORD magic;          // LLVM_LIBC_RESERVED2_MAGIC
  DWORD attr_flags;     // SPAWN_ATTR_SET* bitmask
  HANDLE event;         // state change event handle
  HANDLE section;       // anonymous section backing ChildStateBlock
  uint64_t sigmask;     // inherited signal mask
  uint64_t sigdefault;  // signals to reset to SIG_DFL
};
static_assert(sizeof(Reserved2Ext) == 40,
              "Reserved2Ext is a cross-process ABI; do not change layout");
inline constexpr SIZE_T RESERVED2_EXT_SIZE = sizeof(Reserved2Ext);

// ---------------------------------------------------------------------------
// Internal function declarations — signal subsystem .cpp files only
// ---------------------------------------------------------------------------
// Stack inspection helpers (get_stack_limit, has_stack_space, can_dispatch)
// live in dispatch/alt_stack.h under the signal_stack:: namespace.

// ThreadSignalState lives inline in ThreadLifecycle (lc->sig_state) —
// no separate slab pool is needed. The pool_init / pool_alloc /
// pool_free symbols have been removed.

// Context conversion (signal_delivery.cpp).
void context_win32_to_ucontext(const CONTEXT *win_ctx, ucontext_t *uc);

// Alt-stack trampoline (signal_altstack.cpp).
void deliver_on_alt_stack(void *alt_stack_top, int signum, siginfo_t *info,
                          const struct sigaction *action,
                          ucontext_t *context);

// VEH and console handler registration now lives in Layer 2 transports:
//   transport/veh_transport.h       — statically registered via .libcveh;
//                                     handler self-gates on custom_mask
//   transport/console_transport.h   — console_transport::install/remove

// ---------------------------------------------------------------------------
// Registry iteration — delegated to thread_registry.h
// ---------------------------------------------------------------------------
// Thread registry iteration (registry_for_each, registry_find_by_task_id,
// registry_resolve, etc.) is in src/__support/threads/windows/thread_registry.h.
// The registry is PCB-backed, not owned by the signal subsystem.

} // namespace signal_state
} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SIGNAL_WINDOWS_SIGNAL_INTERNAL_H
